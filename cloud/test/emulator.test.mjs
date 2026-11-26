import assert from 'node:assert/strict';
import { createHash, randomBytes, randomUUID } from 'node:crypto';
import { after, before, test } from 'node:test';
import { initializeTestEnvironment } from '@firebase/rules-unit-testing';
import { call, confirmPairing, database, databaseUrl, enroll, jsonRequest, pair, passwordAccount, project, receipt, runId, signInDevice, telemetry, testDatabaseUrl } from './emulator-helpers.mjs';

let rules;
let owners;
let devices;

before(async () => {
  const endpoint = databaseUrl('');
  rules = await initializeTestEnvironment({
    projectId: project, database: { host: endpoint.hostname, port: Number(endpoint.port) },
  });
  owners = await Promise.all(['owner-a', 'owner-b'].map(name => passwordAccount(`${name}-${runId()}`)));
  devices = await Promise.all(owners.map(owner => enroll(owner)));
});
after(async () => { await rules?.cleanup(); });

const statusPath = device => `devices/${device.uid}/latest`;
const commandsPath = device => `devices/${device.uid}/commands`;
const revisionPath = device => `devices/${device.uid}/disableRevision`;
const resultPath = (device, id) => `devices/${device.uid}/results/${id}`;
const command = (device, type = 'heat.disable', payload = {}) => ({
  deviceUid: device.uid, type, payload, requestId: randomUUID(), sessionId: telemetry().sessionId,
});
const adminSet = (path, value) => rules.withSecurityRulesDisabled(context => context.database(testDatabaseUrl).ref(path).set(value));
const adminGet = async path => {
  const reply = await jsonRequest(databaseUrl(path), 'GET', undefined, 'owner');
  assert.equal(reply.status, 200);
  return reply.data;
};
const authUserExists = async uid => {
  const reply = await jsonRequest(`http://127.0.0.1:9099/identitytoolkit.googleapis.com/v1/projects/${project}/accounts:lookup`,
    'POST', { localId: [uid] }, 'owner');
  assert.equal(reply.status, 200);
  return (reply.data.users ?? []).some(user => user.localId === uid);
};

test('a pairing code is claimed once and the ESP32 can repeat its poll', async () => {
  const start = await pair('begin', { deviceUid: '' });
  assert.equal(start.status, 200);
  assert.match(start.data.code, /^[A-Z2-9]{8}$/);
  assert.match(start.data.sessionSecret, /^[A-Za-z0-9_-]{43}$/);
  assert.equal(start.data.expiresIn, 300);
  const sessionSecret = start.data.sessionSecret;
  assert.equal((await pair('poll', { sessionSecret })).status, 202);
  assert.equal((await call('claimPairing', { code: start.data.code })).status, 401);

  const claims = await Promise.all(owners.map(owner => call('claimPairing', { code: start.data.code }, owner)));
  assert.equal(claims.filter(claim => claim.status === 200).length, 1);
  const winner = claims.findIndex(claim => claim.status === 200);
  const uid = claims[winner].data.result.deviceUid;
  const expiresAt = claims[winner].data.result.expiresAt;
  assert.ok(Number.isSafeInteger(expiresAt) && expiresAt > Date.now());
  assert.deepEqual(Object.keys(claims[winner].data.result).sort(), ['deviceUid', 'expiresAt']);
  const repeatedClaim = await call('claimPairing', { code: start.data.code.toLowerCase() }, owners[winner]);
  assert.equal(repeatedClaim.data.result.deviceUid, uid);
  assert.equal(repeatedClaim.data.result.expiresAt, expiresAt);
  for (let i = 0; i < 2; i++) {
    const poll = await pair('poll', { sessionSecret });
    assert.equal(poll.status, 200);
    assert.equal(poll.data.deviceUid, uid);
    const { idToken, isNewUser } = await signInDevice(poll.data.customToken);
    assert.equal(isNewUser, false, 'the pairing backend provisions the device identity');
    const claims = JSON.parse(Buffer.from(idToken.split('.')[1], 'base64url').toString());
    assert.equal(claims.sub, uid);
  }
});

test('the directory only lists controllers that confirm their saved credentials', async () => {
  const owner = await passwordAccount(`interrupted-${runId()}`);
  const directory = `owners/${owner.localId}/devices`;
  const start = await pair('begin', { deviceUid: '' });
  assert.equal(start.status, 200);
  const abandoned = await call('claimPairing', { code: start.data.code }, owner);
  assert.equal(abandoned.status, 200);
  const abandonedUid = abandoned.data.result.deviceUid;
  const abandonedRegistration = `deviceRegistry/${abandonedUid}`;
  assert.equal(await adminGet(abandonedRegistration), null);
  assert.equal(await authUserExists(abandonedUid), false);
  const poll = await pair('poll', { sessionSecret: start.data.sessionSecret });
  assert.equal(poll.status, 200);
  const pending = await signInDevice(poll.data.customToken);
  assert.equal(await authUserExists(abandonedUid), true);
  assert.equal((await adminGet(abandonedRegistration)).active, false);
  assert.equal((await database(`devices/${abandonedUid}/latest`, pending, 'PUT', telemetry())).status, 401);
  assert.equal((await confirmPairing(owner)).status, 403);
  assert.equal((await database(directory, owner)).data, null);

  const expiredAt = Date.now() - 1;
  const secretHash = createHash('sha256').update(start.data.sessionSecret).digest('hex');
  await adminSet(`pairing/sessions/${secretHash}/expiresAt`, expiredAt);
  await adminSet(`${abandonedRegistration}/pendingUntil`, expiredAt);
  assert.equal((await confirmPairing(pending)).status, 403);
  const next = await pair('begin', { deviceUid: '' });
  assert.equal(next.status, 200);
  assert.equal(await adminGet(abandonedRegistration), null);
  assert.equal(await authUserExists(abandonedUid), false);
  assert.equal((await call('claimPairing', { code: next.data.code }, owner)).status, 200);
  const nextPoll = await pair('poll', { sessionSecret: next.data.sessionSecret });
  assert.equal(nextPoll.status, 200);
  const device = await signInDevice(nextPoll.data.customToken);
  const uid = nextPoll.data.deviceUid;
  assert.notEqual(uid, abandoned.data.result.deviceUid);
  assert.equal((await database(directory, owner)).data, null);

  // A reboot after the flash commit refreshes the ID token before confirming.
  const refresh = await fetch('http://127.0.0.1:9099/securetoken.googleapis.com/v1/token?key=demo-api-key', {
    method: 'POST',
    body: new URLSearchParams({ grant_type: 'refresh_token', refresh_token: device.refreshToken }),
    signal: AbortSignal.timeout(15_000),
  });
  assert.equal(refresh.status, 200);
  const refreshed = { idToken: (await refresh.json()).id_token };
  // Existing firmware can confirm with its original request. The updated
  // controller registers a recovery key using the same authenticated UID.
  assert.equal((await confirmPairing(refreshed)).status, 204);
  const recoveryKey = randomBytes(32).toString('hex');
  assert.deepEqual(await pair('begin', { deviceUid: uid, recoveryKey }), {
    status: 403, data: { error: 'permission-denied' },
  });
  assert.equal((await confirmPairing(refreshed, recoveryKey)).status, 204);
  assert.equal((await confirmPairing(refreshed, recoveryKey)).status, 204);
  assert.deepEqual((await database(directory, owner)).data, { [uid]: true });
  const confirmed = await adminGet(`deviceRegistry/${uid}`);
  assert.equal(confirmed.active, true);
  assert.equal(Object.hasOwn(confirmed, 'pendingUntil'), false);
});

test('a new pairing rate window prunes a bounded batch of expired counters', async () => {
  const owner = await passwordAccount(`limit-cleanup-${runId()}`);
  const scope = 'pairingLimits/claim';
  const fixture = runId();
  const expired = Array.from({ length: 10 }, (_, index) => `${fixture}-expired-${index}`);
  const live = `${fixture}-live`;
  const activeWindow = { until: Date.now() + 60_000, count: 3 };
  await Promise.all([
    ...expired.map((key, index) => adminSet(`${scope}/${key}`, { until: index + 1, count: 4 })),
    adminSet(`${scope}/${live}`, activeWindow),
  ]);

  const start = await pair('begin', { deviceUid: '' });
  assert.equal(start.status, 200);
  assert.equal((await call('claimPairing', { code: start.data.code }, owner)).status, 200);
  const counters = await jsonRequest(databaseUrl(scope), 'GET', undefined, 'owner');
  assert.equal(counters.status, 200);
  assert.deepEqual(counters.data[live], activeWindow);
  assert.deepEqual(expired.filter(key => Object.hasOwn(counters.data, key)), expired.slice(8));
});

test('unapproved password accounts cannot claim, control, or view controllers', async () => {
  const device = devices[0];
  const unapproved = await passwordAccount(`unapproved-${runId()}`, { approved: false });
  const start = await pair('begin', { deviceUid: '' });
  assert.equal(start.status, 200);
  assert.equal((await call('claimPairing', { code: start.data.code }, unapproved)).status, 403);
  assert.equal((await call('issueCommand', command(device), unapproved)).status, 403);
  assert.equal((await database(`owners/${unapproved.localId}/enabled`, unapproved, 'PUT', true)).status, 401);
  assert.equal((await database(`owners/${unapproved.localId}/devices`, unapproved)).status, 401);

  const owner = owners[0];
  const approval = `owners/${owner.localId}/enabled`;
  await adminSet(approval, false);
  try {
    assert.equal((await database(statusPath(device), owner)).status, 401);
    assert.equal((await database(resultPath(device, randomUUID()), owner)).status, 401);
    assert.equal((await call('issueCommand', command(device), owner)).status, 403);
  } finally {
    await adminSet(approval, true);
  }
});

test('re-enrollment requires both the stored recovery key and the current owner', async () => {
  const device = devices[0];
  assert.equal((await pair('begin', { deviceUid: device.uid })).status, 400);
  const wrongKey = randomBytes(32).toString('hex');
  assert.equal((await pair('begin', { deviceUid: device.uid, recoveryKey: wrongKey })).status, 403);
  assert.equal((await confirmPairing(owners[0], wrongKey)).status, 403);
  assert.equal((await confirmPairing(device.credentials, wrongKey)).status, 403);
  const start = await pair('begin', { deviceUid: device.uid, recoveryKey: device.recoveryKey });
  assert.equal(start.status, 200);
  assert.equal((await call('claimPairing', { code: start.data.code }, owners[1])).status, 400);
  const claim = await call('claimPairing', { code: start.data.code }, owners[0]);
  assert.equal(claim.status, 200);
  assert.equal(claim.data.result.deviceUid, device.uid);
  const poll = await pair('poll', { sessionSecret: start.data.sessionSecret });
  assert.equal(poll.status, 200);
  assert.equal(poll.data.deviceUid, device.uid);
  const credentials = await signInDevice(poll.data.customToken);
  const token = JSON.parse(Buffer.from(credentials.idToken.split('.')[1], 'base64url').toString());
  assert.equal(token.sub, device.uid);
  assert.equal((await confirmPairing(credentials, device.recoveryKey)).status, 204);

  // Outstanding legacy sessions lack a device-authorized recovery key.
  const legacyCode = 'OLDTEST2';
  const legacySecret = randomBytes(32).toString('base64url');
  const key = createHash('sha256').update(legacySecret).digest('hex');
  const codeHash = createHash('sha256').update(legacyCode).digest('hex');
  await adminSet(`pairing/codes/${codeHash}`, key);
  await adminSet(`pairing/sessions/${key}`, {
    codeHash, requestedUid: device.uid, expiresAt: Date.now() + 60000,
    ownerUid: owners[0].localId, deviceUid: device.uid,
  });
  assert.equal((await call('claimPairing', { code: legacyCode }, owners[0])).status, 400);
  assert.equal((await pair('poll', { sessionSecret: legacySecret })).status, 202);
});

test('a disabled controller cannot get a recovery code or token until an administrator restores it', async () => {
  const owner = owners[0];
  const device = await enroll(owner);
  const recovery = { deviceUid: device.uid, recoveryKey: device.recoveryKey };
  const claimed = await pair('begin', recovery);
  const unclaimed = await pair('begin', recovery);
  assert.equal(claimed.status, 200);
  assert.equal(unclaimed.status, 200);
  assert.equal((await call('claimPairing', { code: claimed.data.code }, owner)).status, 200);

  const setDisabled = async disableUser => {
    const reply = await jsonRequest(
      `http://127.0.0.1:9099/identitytoolkit.googleapis.com/v1/projects/${project}/accounts:update`,
      'POST', { localId: device.uid, disableUser }, 'owner');
    assert.equal(reply.status, 200);
  };
  await setDisabled(true);
  const disabled = { status: 409, data: { error: 'device_disabled' } };
  assert.deepEqual(await pair('begin', recovery), disabled);
  assert.equal((await pair('begin', { ...recovery, recoveryKey: randomBytes(32).toString('hex') })).status, 403);
  const claim = await call('claimPairing', { code: unclaimed.data.code }, owner);
  assert.equal(claim.status, 400);
  assert.equal(claim.data.error.details, 'device_disabled');
  assert.deepEqual(await pair('poll', { sessionSecret: claimed.data.sessionSecret }), disabled);
  assert.deepEqual(await confirmPairing(device.credentials, device.recoveryKey), disabled);

  await setDisabled(false);
  const poll = await pair('poll', { sessionSecret: claimed.data.sessionSecret });
  assert.equal(poll.status, 200);
  assert.equal(poll.data.deviceUid, device.uid);
  assert.equal((await confirmPairing(await signInDevice(poll.data.customToken), device.recoveryKey)).status, 204);
});

test('expired pairing fails', async () => {
  const expire = await pair('begin', { deviceUid: '' });
  assert.equal(expire.status, 200);
  const key = createHash('sha256').update(expire.data.sessionSecret).digest('hex');
  await adminSet(`pairing/sessions/${key}/polls`, 160);
  assert.equal((await pair('poll', { sessionSecret: expire.data.sessionSecret })).status, 429);
  await adminSet(`pairing/sessions/${key}/expiresAt`, Date.now() - 1000);
  assert.equal((await pair('poll', { sessionSecret: expire.data.sessionSecret })).status, 410);
  assert.equal((await call('claimPairing', { code: expire.data.code }, owners[0])).status, 400);
});

test('rules isolate owners and device access and validate the firmware snapshot', async () => {
  const [a, b] = devices;
  for (const device of devices) {
    assert.equal((await database(statusPath(device), device.credentials, 'PUT', telemetry())).status, 200);
  }
  const directory = await database(`owners/${owners[0].localId}/devices`, owners[0]);
  assert.equal(directory.status, 200);
  assert.equal(directory.data[a.uid], true);
  assert.equal(directory.data[b.uid], undefined);
  assert.equal((await database(statusPath(a), owners[0])).status, 200);
  assert.equal((await database(statusPath(a), owners[1])).status, 401);
  assert.equal((await database(statusPath(a))).status, 401);
  assert.equal((await database(statusPath(a), b.credentials, 'PUT', telemetry())).status, 401);
  assert.equal((await database(statusPath(a), owners[0], 'PUT', telemetry())).status, 401);
  assert.equal((await database(commandsPath(a), a.credentials)).status, 200);
  assert.equal((await database(commandsPath(b), a.credentials)).status, 401);
  assert.equal((await database(commandsPath(a), a.credentials, 'PUT', { unauthorized: true })).status, 401);
  assert.equal((await database(commandsPath(a), owners[0], 'PUT', { unauthorized: true })).status, 401);
  assert.equal((await database(revisionPath(a), owners[0])).status, 200);
  assert.equal((await database(revisionPath(a), owners[1])).status, 401);
  assert.equal((await database(revisionPath(a), a.credentials, 'PUT', 1)).status, 401);
  assert.equal((await database(revisionPath(a), owners[0], 'PUT', 1)).status, 401);
  assert.equal((await database(`devices/${a.uid}`, owners[0])).status, 401);
  assert.equal((await database('pairing', owners[0])).status, 401);
  assert.equal((await database(statusPath(a), a.credentials, 'PUT', telemetry({ extra: true }))).status, 401);
  assert.equal((await database(statusPath(a), a.credentials, 'PUT', telemetry({ boardId: 'not-a-board' }))).status, 401);
  assert.equal((await database(statusPath(a), a.credentials, 'PUT', telemetry({ restorePending: 'true' }))).status, 401);
  for (const heap of [{ freeBytes: 10 }, { freeBytes: 10, minimumFreeBytes: 11 },
    { freeBytes: 10.5, minimumFreeBytes: 0 }, { freeBytes: 80092, minimumFreeBytes: -1 }]) {
    assert.equal((await database(statusPath(a), a.credentials, 'PUT', telemetry({ heap }))).status, 401);
  }
  const heap = { freeBytes: 80092, minimumFreeBytes: 25328 };
  assert.equal((await database(statusPath(a), a.credentials, 'PUT',
    telemetry({ targetC: 60, restartC: 55, boardId: '7C87CE01020A', heap }))).status, 200);
  const latest = (await database(statusPath(a), owners[0])).data;
  assert.equal(latest.boardId, '7C87CE01020A');
  assert.deepEqual(latest.heap, heap);
  assert.equal((await database(statusPath(a), a.credentials, 'PATCH',
    { targetC: 54, updatedAt: { '.sv': 'timestamp' } })).status, 401);
  assert.equal((await database(statusPath(a), a.credentials, 'PUT',
    telemetry({ targetC: 60, restartC: 39.5 }))).status, 401);
  assert.equal((await database(statusPath(a), a.credentials, 'PUT', telemetry({
    temperatureValid: false, temperatureC: undefined, commandReady: false, sessionId: '',
    heatEnabled: false, heaterOn: false, fanOn: false, restorePending: true,
  }))).status, 200);
  assert.equal((await database(statusPath(a), owners[0])).data.restorePending, true);
  await adminSet(`deviceRegistry/${b.uid}/active`, false);
  assert.equal((await database(statusPath(b), b.credentials, 'PUT', telemetry())).status, 401);
  assert.equal((await database(commandsPath(b), b.credentials)).status, 401);
});

test('command submission validates ownership, deduplicates concurrent retries, and accepts only device confirmations', async () => {
  const [a, b] = devices;
  assert.equal((await database(statusPath(a), a.credentials, 'PUT', telemetry())).status, 200);
  const request = command(a, 'heat.setTarget', { targetC: 56.5 });
  assert.equal((await call('issueCommand', request, owners[1])).status, 403);
  assert.equal((await call('issueCommand', request, a.credentials)).status, 403);
  assert.equal((await call('issueCommand', { ...request, payload: { targetC: 56.25 } }, owners[0])).status, 400);
  const submission = await Promise.all([1, 2].map(() => call('issueCommand', request, owners[0])));
  assert.deepEqual(submission.map(result => result.status), [200, 200]);
  assert.deepEqual(submission[0].data.result, submission[1].data.result);
  assert.equal((await call('issueCommand', { ...request, payload: { targetC: 55 } }, owners[0])).status, 409);

  const other = [command(a, 'sensor.setCalibration', { calibrationOffsetC: 1 }), command(a, 'heat.enable')];
  const accepted = await Promise.all(other.map(item => call('issueCommand', item, owners[0])));
  assert.deepEqual(accepted.map(result => result.status), [200, 200]);
  const inbox = (await database(commandsPath(a), a.credentials)).data;
  assert.deepEqual(Object.values(inbox).map(item => item.sequence).sort((x, y) => x - y), [1, 2, 3]);
  assert.equal(inbox[request.requestId].expiresAt - inbox[request.requestId].issuedAt, 30_000);
  assert.equal(inbox[request.requestId].sessionId, telemetry().sessionId);

  const path = resultPath(a, request.requestId);
  const result = receipt(request.requestId);
  assert.equal((await database(path, owners[0], 'PUT', result)).status, 401);
  assert.equal((await database(path, b.credentials, 'PUT', result)).status, 401);
  assert.equal((await database(resultPath(a, randomUUID()), a.credentials, 'PUT', result)).status, 401);
  assert.equal((await database(path, a.credentials, 'PUT', result)).status, 200);
  assert.equal((await database(path, a.credentials, 'PUT', result)).status, 200);
  assert.equal((await database(path, a.credentials, 'PUT', receipt(request.requestId, 'rejected', 'precondition'))).status, 401);
  assert.equal((await database(path, owners[0])).data.status, 'applied');
  assert.equal((await database(path, owners[1])).status, 401);
});

test('the backend rejects stale sessions and Disable supersedes old requests', async () => {
  const [device] = devices;
  await adminSet(`${statusPath(device)}/updatedAt`, Date.now() - 46_000);
  assert.equal((await call('issueCommand', command(device), owners[0])).status, 400);
  const sessionId = '89abcdef0123456789abcdef01234567';
  assert.equal((await database(statusPath(device), device.credentials, 'PUT', telemetry({ sessionId }))).status, 200);
  const fromOldSession = command(device, 'heat.enable');
  assert.equal((await call('issueCommand', fromOldSession, owners[0])).status, 400);
  assert.equal((await database(`${commandsPath(device)}/${fromOldSession.requestId}`, device.credentials)).data, null);
  const enable = { ...command(device, 'heat.enable'), sessionId };
  assert.equal((await call('issueCommand', enable, owners[0])).status, 200);
  const delayed = { ...command(device, 'heat.enable'), sessionId, disableRevision: 0 };
  const disable = command(device);
  assert.equal((await call('issueCommand', disable, owners[0])).status, 200);
  assert.equal((await call('issueCommand', delayed, owners[0])).status, 400);
  const { disableRevision: _legacy, ...legacyEnable } = delayed;
  assert.equal((await call('issueCommand', legacyEnable, owners[0])).status, 400);
  const inbox = await database(commandsPath(device), device.credentials);
  assert.deepEqual(Object.keys(inbox.data), [disable.requestId]);
  assert.equal(inbox.data[disable.requestId].sessionId, sessionId);
  assert.equal((await database(resultPath(device, enable.requestId), device.credentials,
    'PUT', receipt(enable.requestId, 'expired', 'deadline'))).status, 200);
  const revision = (await database(revisionPath(device), owners[0])).data;
  assert.equal(revision, inbox.data[disable.requestId].sequence);
  const fresh = { ...command(device, 'heat.enable'), sessionId, disableRevision: revision };
  assert.equal((await call('issueCommand', fresh, owners[0])).status, 200);
});

async function* commandEvents(body) {
  let pending = '';
  for await (const chunk of body.pipeThrough(new TextDecoderStream())) {
    pending = (pending + chunk).replaceAll('\r\n', '\n');
    let end;
    while ((end = pending.indexOf('\n\n')) !== -1) {
      const frame = pending.slice(0, end);
      pending = pending.slice(end + 2);
      const match = /^event: (put|patch)\ndata: (.+)$/m.exec(frame);
      if (match) yield { event: match[1], ...JSON.parse(match[2]) };
    }
  }
}

test('the ESP32 REST stream receives a command after a silent telemetry upload', async () => {
  const owner = owners[0];
  const device = await enroll(owner);
  const status = databaseUrl(statusPath(device), device.credentials);
  status.searchParams.set('print', 'silent');
  assert.equal((await jsonRequest(status, 'PUT', telemetry())).status, 204);

  const stream = new AbortController();
  try {
    const response = await fetch(databaseUrl(commandsPath(device), device.credentials), {
      headers: { Accept: 'text/event-stream' },
      signal: AbortSignal.any([stream.signal, AbortSignal.timeout(10_000)]),
    });
    assert.equal(response.status, 200);
    assert.match(response.headers.get('content-type'), /^text\/event-stream/);
    const events = commandEvents(response.body);
    assert.deepEqual((await events.next()).value, { event: 'put', path: '/', data: null });

    const disable = command(device);
    assert.equal((await call('issueCommand', disable, owner)).status, 200);
    const update = (await events.next()).value;
    assert.ok(update.path === '/' || update.path === `/${disable.requestId}`);
    const queued = update.path === '/' ? update.data[disable.requestId] : update.data;
    assert.equal(queued.id, disable.requestId);
    assert.equal(queued.type, 'heat.disable');
    assert.equal(queued.sessionId, telemetry().sessionId);
    assert.ok(Buffer.byteLength(JSON.stringify(update)) < 6144);
  } finally {
    stream.abort();
  }
});
