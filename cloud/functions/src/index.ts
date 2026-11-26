import { initializeApp } from 'firebase-admin/app';
import { getAuth } from 'firebase-admin/auth';
import { getDatabase, type Database } from 'firebase-admin/database';
import { HttpsError, onCall, onRequest, type CallableRequest } from 'firebase-functions/v2/https';
import { readCommandRequest, submitCommand, type Submission } from './commands.js';
import { EMULATOR_DATABASE_URL, EMULATOR_PROJECT, FUNCTION_REGION, isRecord, isUid } from './contract.js';
import { beginPairing, claimPairingCode, completePairing, DEVICE_DISABLED, pollPairing } from './pairing.js';

const useDemoDatabase = process.env.FIREBASE_DATABASE_EMULATOR_HOST && process.env.GCLOUD_PROJECT === EMULATOR_PROJECT;
initializeApp(useDemoDatabase ? { projectId: EMULATOR_PROJECT, databaseURL: EMULATOR_DATABASE_URL } : undefined);

const options = { region: FUNCTION_REGION, maxInstances: 5, timeoutSeconds: 15 };
const pairingErrorStatus: Partial<Record<HttpsError['code'], number>> = {
  'invalid-argument': 400, unauthenticated: 401, 'permission-denied': 403, 'resource-exhausted': 429,
  'failed-precondition': 409,
};

async function ownerUid(db: Database, request: CallableRequest<unknown>) {
  if (!request.auth) throw new HttpsError('unauthenticated', 'Sign in to manage your controllers.');
  const { uid, token } = request.auth;
  if (!isUid(uid) || token.firebase?.sign_in_provider !== 'password' ||
      (await db.ref(`owners/${uid}/enabled`).get()).val() !== true) {
    throw new HttpsError('permission-denied', 'Use an approved email and password account.');
  }
  return uid;
}

export const pairing = onRequest(options, async (request, response) => {
  response.set('Cache-Control', 'no-store');
  if (request.method !== 'POST') {
    response.set('Allow', 'POST').status(405).end();
    return;
  }
  const match = /^(?:\/api\/pairing)?\/(begin|poll|complete)$/.exec(request.path);
  if (!match) {
    response.status(404).end();
    return;
  }
  const action = match[1];
  const maxBytes = action === 'complete' ? 2_240 : 512;
  if (!request.is('application/json') || (request.rawBody?.length ?? 0) > maxBytes) {
    response.status(400).json({ error: 'invalid_request' });
    return;
  }
  try {
    const db = getDatabase();
    if (action === 'begin') {
      const data = await beginPairing(db, getAuth(), request.body, request.ip ?? request.socket.remoteAddress ?? 'unknown');
      response.status(200).json(data);
      return;
    }
    if (action === 'complete') {
      await completePairing(db, getAuth(), request.body);
      response.status(204).end();
      return;
    }
    const result = await pollPairing(db, getAuth(), request.body);
    if (result.status !== 200) {
      response.status(result.status).end();
      return;
    }
    const customToken = await getAuth().createCustomToken(result.uid);
    response.status(200).json({ deviceUid: result.uid, customToken });
  } catch (error) {
    if (error instanceof HttpsError) {
      const status = pairingErrorStatus[error.code] ?? 503;
      response.status(status).json({ error: error.details === DEVICE_DISABLED ? DEVICE_DISABLED : error.code });
    } else {
      response.status(503).json({ error: 'pairing_unavailable' });
    }
  }
});

export const claimPairing = onCall(options, async request => {
  const db = getDatabase();
  return claimPairingCode(db, getAuth(), await ownerUid(db, request), request.data);
});

export const issueCommand = onCall(options, async request => {
  const db = getDatabase();
  const owner = await ownerUid(db, request);
  const command = readCommandRequest(request.data);
  if (!command) throw new HttpsError('invalid-argument', 'Invalid command or value.');
  const device = (await db.ref(`deviceRegistry/${command.deviceUid}`).get()).val();
  if (!isRecord(device) || device.ownerUid !== owner || device.active !== true) {
    throw new HttpsError('permission-denied', 'This controller is unavailable for your account.');
  }
  let outcome: Submission | undefined;
  const result = await db.ref(`devices/${command.deviceUid}`).transaction(current => {
    const next = submitCommand(current, command, Date.now());
    outcome = next;
    // A no-op transaction still checks the server version when our cache is cold.
    return 'write' in next && next.write ? next.write : current;
  }, undefined, false);
  if (!result.committed || !outcome) throw new HttpsError('unavailable', 'Could not confirm submission.');
  if ('receipt' in outcome) return outcome.receipt;
  switch (outcome.error) {
    case 'superseded': throw new HttpsError('failed-precondition', 'Disable heat superseded this command.');
    case 'stale-session': throw new HttpsError('failed-precondition', 'The controller session changed. Refresh the dashboard and try again.');
    case 'restart-range': throw new HttpsError('failed-precondition',
      'Choose a restart temperature 0.5 to 20 °C below the current target on a controller that supports this setting.');
    case 'update-unavailable': throw new HttpsError('failed-precondition',
      'Firmware updates require an idle controller with heating and Monitor P1S off.');
    case 'offline': throw new HttpsError('failed-precondition', 'The controller is not ready for commands.');
    case 'queue-full': throw new HttpsError('resource-exhausted', 'The controller is still processing commands.');
    case 'reused-request': throw new HttpsError('already-exists', 'This request ID was used with a different command.');
    case 'sequence-exhausted': throw new HttpsError('failed-precondition', 'The device command counter is full.');
  }
});
