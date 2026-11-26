import { createHash, randomBytes, randomUUID } from 'node:crypto';
import type { Auth, DecodedIdToken } from 'firebase-admin/auth';
import type { Database, Reference } from 'firebase-admin/database';
import { warn } from 'firebase-functions/logger';
import { HttpsError } from 'firebase-functions/v2/https';
import { PAIRING_CODE_TTL_MS, isRecord, isUid } from './contract.js';

const CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
const MAX_SESSION_POLLS = 160;
const MINUTE = 60_000;
const ENROLLMENT_GRACE_MS = 24 * 60 * MINUTE;
const RATE_LIMIT_CLEANUP_BATCH = 8;
const ENROLLMENT_CLEANUP_BATCH = 8;
export const DEVICE_DISABLED = 'device_disabled';

interface Session {
  codeHash: string;
  requestedUid: string;
  expiresAt: number;
  recoveryKeyHash?: string;
  polls?: number;
  ownerUid?: string;
  deviceUid?: string;
}

function digest(value: string) {
  return createHash('sha256').update(value).digest('hex');
}

function isRecoveryKey(value: unknown): value is string {
  return typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);
}

function allowsRecovery(registry: unknown, keyHash: string | undefined): registry is Record<string, unknown> {
  return !!keyHash && isRecord(registry) && registry.active === true && registry.recoveryKeyHash === keyHash;
}

function disabledDevice() {
  return new HttpsError('failed-precondition',
    'The controller’s Firebase account is disabled. Ask an administrator to re-enable it.', DEVICE_DISABLED);
}

async function requireEnabledDevice(auth: Auth, uid: string) {
  const user = await auth.getUser(uid).catch(error => {
    if (isRecord(error) && error.code === 'auth/user-not-found') return undefined;
    throw error;
  });
  if (user?.disabled) throw disabledDevice();
}

function enrollmentPending(registry: Record<string, unknown>, now: number) {
  return registry.active === false && registry.cleanupPending !== true &&
    typeof registry.pendingUntil === 'number' && registry.pendingUntil > now;
}

function makeCode() {
  return Array.from(randomBytes(8), byte => CODE_ALPHABET[byte & 31]).join('');
}

function session(value: unknown): Session | undefined {
  if (!isRecord(value) || typeof value.codeHash !== 'string' || typeof value.requestedUid !== 'string' ||
      typeof value.expiresAt !== 'number') return;
  return value as unknown as Session;
}

async function pruneExpiredRateLimits(limits: Reference) {
  const cutoff = Date.now();
  const expired = await limits.orderByChild('until').startAt(0).endAt(cutoff)
    .limitToFirst(RATE_LIMIT_CLEANUP_BATCH).get();
  const removals: Promise<unknown>[] = [];
  expired.forEach(record => {
    // The window may have been renewed after the query.
    removals.push(record.ref.transaction(current => (
      isRecord(current) && typeof current.until === 'number' && current.until <= cutoff ? null : current
    ), undefined, false));
  });
  await Promise.all(removals);
}

async function rateLimit(db: Database, scope: string, key: string, maximum: number, window: number) {
  const limits = db.ref('pairingLimits').child(scope);
  let allowed = false;
  const result = await limits.child(digest(key)).transaction(current => {
    const now = Date.now();
    const recent = isRecord(current) && typeof current.until === 'number' &&
      typeof current.count === 'number' && current.until > now;
    if (recent && Number(current.count) >= maximum) {
      allowed = false;
      return current;
    }
    allowed = true;
    return recent ? { until: current.until, count: Number(current.count) + 1 } : { until: now + window, count: 1 };
  }, undefined, false);
  if (!result.committed || !allowed) {
    throw new HttpsError('resource-exhausted', 'Too many pairing attempts. Try again shortly.');
  }
  if (result.snapshot.child('count').val() === 1) {
    await pruneExpiredRateLimits(limits).catch(() => {
      warn('Could not prune expired pairing rate limits.', { scope });
    });
  }
}

async function removeExpiredEnrollment(ref: Reference, auth: Auth, cutoff: number) {
  const uid = ref.key;
  if (!isUid(uid)) return;
  const reserved = await ref.transaction(current => {
    if (current === null) return current;
    if (!isRecord(current) || current.active !== false || typeof current.pendingUntil !== 'number' ||
        current.pendingUntil > cutoff) return;
    return { ...current, cleanupPending: true };
  }, undefined, false);
  if (!reserved.committed || reserved.snapshot.child('cleanupPending').val() !== true) return;
  try {
    await auth.deleteUser(uid);
  } catch (error) {
    if (!isRecord(error) || error.code !== 'auth/user-not-found') throw error;
  }
  await ref.transaction(current => {
    if (current === null) return current;
    if (isRecord(current) && current.active === false && current.cleanupPending === true) return null;
  }, undefined, false);
}

async function pruneExpiredEnrollments(db: Database, auth: Auth) {
  const cutoff = Date.now();
  const expired = await db.ref('deviceRegistry').orderByChild('pendingUntil').startAt(0).endAt(cutoff)
    .limitToFirst(ENROLLMENT_CLEANUP_BATCH).get();
  const removals: Promise<void>[] = [];
  expired.forEach(record => { removals.push(removeExpiredEnrollment(record.ref, auth, cutoff)); });
  await Promise.all(removals);
}

export async function beginPairing(db: Database, auth: Auth, input: unknown, address: string) {
  if (!isRecord(input) || (input.deviceUid !== '' && !isUid(input.deviceUid)) ||
      (input.deviceUid === '' ? Object.keys(input).length !== 1 :
        Object.keys(input).length !== 2 || !isRecoveryKey(input.recoveryKey))) {
    throw new HttpsError('invalid-argument', 'Invalid enrollment request.');
  }
  const requestedUid = input.deviceUid;
  await rateLimit(db, 'begin', address, 20, 10 * MINUTE);
  await pruneExpiredEnrollments(db, auth).catch(() => {
    warn('Could not prune expired controller enrollments.');
  });
  const recoveryKeyHash = isRecoveryKey(input.recoveryKey) ? digest(input.recoveryKey) : undefined;
  if (requestedUid) {
    const registry = (await db.ref(`deviceRegistry/${requestedUid}`).get()).val();
    if (!allowsRecovery(registry, recoveryKeyHash)) {
      throw new HttpsError('permission-denied', 'The controller could not verify its enrollment.');
    }
    await requireEnabledDevice(auth, requestedUid);
  }
  const secret = randomBytes(32).toString('base64url');
  const secretHash = digest(secret);
  for (let attempt = 0; attempt < 4; attempt++) {
    const code = makeCode();
    const codeHash = digest(code);
    const result = await db.ref('pairing').transaction(current => {
      const now = Date.now();
      const previous = isRecord(current?.sessions) ? current.sessions : {};
      const sessions: Record<string, Session> = {};
      const codes: Record<string, string> = {};
      for (const [key, value] of Object.entries(previous)) {
        const entry = session(value);
        if (entry && entry.expiresAt > now) {
          sessions[key] = entry;
          codes[entry.codeHash] = key;
        }
      }
      if (Object.hasOwn(codes, codeHash)) return;
      const entry: Session = { codeHash, requestedUid, expiresAt: now + PAIRING_CODE_TTL_MS };
      if (recoveryKeyHash) entry.recoveryKeyHash = recoveryKeyHash;
      sessions[secretHash] = entry;
      codes[codeHash] = secretHash;
      return { sessions, codes };
    }, undefined, false);
    if (result.committed) return { code, sessionSecret: secret, expiresIn: PAIRING_CODE_TTL_MS / 1_000 };
  }
  throw new HttpsError('unavailable', 'Pairing is busy. Try again shortly.');
}

function unavailableCode() {
  return new HttpsError('failed-precondition', 'This pairing code is unavailable. Check the controller for a current code.');
}

export async function claimPairingCode(db: Database, auth: Auth, ownerUid: string, input: unknown) {
  const code = isRecord(input) && Object.keys(input).length === 1 && typeof input.code === 'string'
    ? input.code.trim().toUpperCase()
    : '';
  if (!/^[A-Z2-9]{8}$/.test(code)) {
    throw new HttpsError('invalid-argument', 'Enter the eight-character code from the controller.');
  }
  await rateLimit(db, 'claim', ownerUid, 12, 5 * MINUTE);
  const codeHash = digest(code);
  const key = (await db.ref(`pairing/codes/${codeHash}`).get()).val();
  if (typeof key !== 'string') throw unavailableCode();
  const ref = db.ref(`pairing/sessions/${key}`);
  const existing = session((await ref.get()).val());
  if (!existing || existing.codeHash !== codeHash || existing.expiresAt <= Date.now()) throw unavailableCode();
  if (existing.requestedUid) {
    const registry = (await db.ref(`deviceRegistry/${existing.requestedUid}`).get()).val();
    if (!allowsRecovery(registry, existing.recoveryKeyHash) || registry.ownerUid !== ownerUid) {
      throw unavailableCode();
    }
    await requireEnabledDevice(auth, existing.requestedUid);
  }
  const newUid = existing.requestedUid || `p1s_${randomUUID()}`;
  const result = await ref.transaction(current => {
    const value = session(current);
    if (!value || value.codeHash !== codeHash || value.expiresAt <= Date.now() ||
        (value.ownerUid && value.ownerUid !== ownerUid)) return current;
    return { ...value, ownerUid, deviceUid: value.deviceUid ?? newUid };
  }, undefined, false);
  const claimed = session(result.snapshot.val());
  if (!result.committed || claimed?.ownerUid !== ownerUid || !isUid(claimed.deviceUid) ||
      claimed.expiresAt <= Date.now()) throw unavailableCode();

  return { deviceUid: claimed.deviceUid, expiresAt: claimed.expiresAt };
}

async function provisionDevice(auth: Auth, uid: string) {
  // An existing account lets the ESP32 exchange its custom token when self-service creation is disabled.
  try {
    await auth.createUser({ uid });
  } catch (error) {
    if (!isRecord(error) || error.code !== 'auth/uid-already-exists') throw error;
    await requireEnabledDevice(auth, uid);
  }
}

export async function pollPairing(db: Database, auth: Auth, input: unknown) {
  const secret = isRecord(input) && Object.keys(input).length === 1 ? input.sessionSecret : undefined;
  if (typeof secret !== 'string' || !/^[A-Za-z0-9_-]{32,128}$/.test(secret)) {
    throw new HttpsError('invalid-argument', 'Invalid pairing session.');
  }
  const key = digest(secret);
  const poll = await db.ref(`pairing/sessions/${key}`).transaction(current => {
    const value = session(current);
    if (!value || value.expiresAt <= Date.now()) return current;
    if ((value.polls ?? 0) >= MAX_SESSION_POLLS) return;
    return { ...value, polls: (value.polls ?? 0) + 1 };
  }, undefined, false);
  const value = session(poll.snapshot.val());
  if (!value || value.expiresAt <= Date.now()) return { status: 410 } as const;
  if (!poll.committed) {
    throw new HttpsError('resource-exhausted', 'Too many pairing attempts. Try again shortly.');
  }
  if (!isUid(value.ownerUid) || !isUid(value.deviceUid)) return { status: 202 } as const;
  const registryRef = db.ref(`deviceRegistry/${value.deviceUid}`);
  if (value.requestedUid) {
    const registry = (await registryRef.get()).val();
    if (value.deviceUid !== value.requestedUid || !allowsRecovery(registry, value.recoveryKeyHash) ||
        registry.ownerUid !== value.ownerUid) return { status: 202 } as const;
  } else {
    const registration = await registryRef.transaction(current => current ?? {
      ownerUid: value.ownerUid, active: false, pendingUntil: value.expiresAt + ENROLLMENT_GRACE_MS,
    }, undefined, false);
    const registry = registration.snapshot.val();
    if (!registration.committed || !isRecord(registry) || registry.ownerUid !== value.ownerUid ||
        (registry.active !== true && !enrollmentPending(registry, Date.now()))) return { status: 202 } as const;
  }
  await provisionDevice(auth, value.deviceUid);
  return { status: 200, uid: value.deviceUid } as const;
}

export async function completePairing(db: Database, auth: Auth, input: unknown) {
  if (!isRecord(input) || typeof input.idToken !== 'string' || !input.idToken.length ||
      input.idToken.length >= 2048 || (Object.hasOwn(input, 'recoveryKey')
        ? Object.keys(input).length !== 2 || !isRecoveryKey(input.recoveryKey) : Object.keys(input).length !== 1)) {
    throw new HttpsError('invalid-argument', 'Invalid controller token.');
  }

  let token: DecodedIdToken;
  try {
    token = await auth.verifyIdToken(input.idToken, true);
  } catch (error) {
    const code = isRecord(error) ? error.code : undefined;
    if (code === 'auth/user-disabled') throw disabledDevice();
    if (code === 'auth/argument-error' || code === 'auth/invalid-id-token' || code === 'auth/id-token-expired' ||
        code === 'auth/id-token-revoked' || code === 'auth/user-not-found') {
      throw new HttpsError('unauthenticated', 'Invalid controller token.');
    }
    throw error;
  }
  if (!isUid(token.uid) || token.firebase?.sign_in_provider !== 'custom') {
    throw new HttpsError('permission-denied', 'Use the controller account to complete pairing.');
  }

  // The controller calls this after committing its refresh credentials to flash.
  const keyHash = isRecoveryKey(input.recoveryKey) ? digest(input.recoveryKey) : undefined;
  const result = await db.ref(`deviceRegistry/${token.uid}`).transaction(current => {
    if (current === null) return current;
    if (!isRecord(current) || !isUid(current.ownerUid) ||
        (current.active !== true && !enrollmentPending(current, Date.now())) ||
        (keyHash && current.recoveryKeyHash !== undefined && current.recoveryKeyHash !== keyHash)) return;
    const confirmed: Record<string, unknown> = { ...current, active: true };
    delete confirmed.pendingUntil;
    if (keyHash) confirmed.recoveryKeyHash = keyHash;
    return confirmed;
  }, undefined, false);
  const registry = result.snapshot.val();
  if (!result.committed || !isRecord(registry) || registry.active !== true || !isUid(registry.ownerUid)) {
    throw new HttpsError('permission-denied', 'The controller could not verify its enrollment.');
  }
  await db.ref(`owners/${registry.ownerUid}/devices/${token.uid}`).set(true);
}
