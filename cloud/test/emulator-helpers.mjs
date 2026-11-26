import assert from 'node:assert/strict';
import { randomBytes, randomUUID } from 'node:crypto';

export const runId = () => randomUUID().slice(0, 8);
export const project = 'demo-p1s-heat';
export const namespace = `${project}-default-rtdb`;
export const testDatabaseUrl = `https://${namespace}.firebaseio.com`;
const authOrigin = 'http://127.0.0.1:9099/identitytoolkit.googleapis.com/v1';
const functionOrigin = `http://127.0.0.1:5001/${project}/us-central1`;
const databaseHost = process.env.FIREBASE_DATABASE_EMULATOR_HOST ?? '127.0.0.1:9000';
export const hostingOrigin = 'http://127.0.0.1:5000';

export async function jsonRequest(url, method, body, token) {
  const headers = body === undefined ? {} : { 'Content-Type': 'application/json' };
  if (token) headers.Authorization = `Bearer ${token}`;
  const response = await fetch(url, {
    method,
    headers,
    body: body === undefined ? undefined : JSON.stringify(body),
    // The first request includes Functions emulator worker startup time.
    signal: AbortSignal.timeout(45_000),
  });
  const content = await response.text();
  const data = content && response.headers.get('content-type')?.includes('json') ? JSON.parse(content) : content;
  return { status: response.status, data };
}

export function call(name, data, user) {
  return jsonRequest(`${functionOrigin}/${name}`, 'POST', { data }, user?.idToken);
}

export function pair(action, body) {
  return jsonRequest(`${hostingOrigin}/api/pairing/${action}`, 'POST', body);
}

export async function passwordAccount(name, { approved = true } = {}) {
  const email = `${name}@example.test`;
  const password = `Test-${randomUUID()}`;
  const created = approved
    ? await jsonRequest(`${authOrigin}/projects/${project}/accounts`, 'POST', { email, password }, 'owner')
    : await jsonRequest(`${authOrigin}/accounts:signUp?key=demo-api-key`, 'POST', { email, password });
  assert.equal(created.status, 200, 'create an account in the local Auth emulator');
  if (approved) {
    const enabled = databaseUrl(`owners/${created.data.localId}/enabled`);
    assert.equal((await jsonRequest(enabled, 'PUT', true, 'owner')).status, 200, 'approve the owner in the local database');
  }
  const reply = await jsonRequest(`${authOrigin}/accounts:signInWithPassword?key=demo-api-key`, 'POST', {
    email, password, returnSecureToken: true,
  });
  assert.equal(reply.status, 200, 'sign in with an email and password in the Auth emulator');
  return { ...reply.data, email, password };
}

export async function signInDevice(customToken) {
  const reply = await jsonRequest(`${authOrigin}/accounts:signInWithCustomToken?key=demo-api-key`, 'POST', {
    token: customToken, returnSecureToken: true,
  });
  assert.equal(reply.status, 200, 'custom token exchange in the local Auth emulator');
  return reply.data;
}

export function confirmPairing(device, recoveryKey) {
  return pair('complete', { idToken: device.idToken, recoveryKey });
}

export async function enroll(owner) {
  const begin = await pair('begin', { deviceUid: '' });
  assert.equal(begin.status, 200, 'pairing starts through Hosting');
  const session = begin.data;
  const claim = await call('claimPairing', { code: session.code }, owner);
  assert.equal(claim.status, 200, 'owner claims controller');
  const poll = await pair('poll', { sessionSecret: session.sessionSecret });
  assert.equal(poll.status, 200, 'ESP32 receives the custom token');
  const credentials = await signInDevice(poll.data.customToken);
  const recoveryKey = randomBytes(32).toString('hex');
  assert.equal((await confirmPairing(credentials, recoveryKey)).status, 204, 'ESP32 confirms its saved credentials');
  return { uid: poll.data.deviceUid, credentials, recoveryKey };
}

export function databaseUrl(path, user) {
  const url = new URL(`http://${databaseHost}/${path}.json`);
  url.searchParams.set('ns', namespace);
  if (user) url.searchParams.set('auth', user.idToken);
  return url;
}

export function database(path, user, method = 'GET', data) {
  return jsonRequest(databaseUrl(path, user), method, data);
}

export function telemetry(overrides = {}) {
  return {
    schemaVersion: 1, updatedAt: { '.sv': 'timestamp' }, name: 'Enclosure P1S',
    firmwareVersion: '1.0.1', state: 'heating', fault: 'none',
    targetC: 60, calibrationOffsetC: 0, temperatureValid: true, temperatureC: 52.4,
    heatEnabled: true, heaterOn: true, fanOn: true, printerConfigured: true,
    canMonitor: true, printerConnected: true, printerStateKnown: true, printerState: 3,
    monitorPhase: 0, updatePhase: 0, updateResult: 0, updateReceived: 0, updateTotal: 0,
    commandReady: true, sessionId: '0123456789abcdef0123456789abcdef', ...overrides,
  };
}

export function receipt(id, status = 'applied', reason = '') {
  return { id, status, reason, timestamp: { '.sv': 'timestamp' } };
}
