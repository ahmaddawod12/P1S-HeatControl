import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { test } from 'node:test';
import { readCommandRequest, submitCommand } from '../functions/lib/commands.js';
import { readCommandAction, readLatest, readResult } from '../functions/lib/contract.js';
import { telemetry } from './emulator-helpers.mjs';

const now = 1_800_000_000_000;
const state = () => ({ latest: telemetry({ updatedAt: now }) });
const request = (type = 'heat.enable', payload = {}, disableRevision = 0) => ({
  type, payload, disableRevision, sessionId: telemetry().sessionId, deviceUid: 'p1s_test', requestId: randomUUID(),
});

test('command validation matches the firmware target, calibration and empty payload shapes', () => {
  assert.deepEqual(readCommandAction('heat.setTarget', { targetC: 60.5 })?.payload, { targetC: 60.5 });
  for (const targetC of [NaN, Infinity, 60.25, -0.5, 80.5, '60']) {
    assert.equal(readCommandAction('heat.setTarget', { targetC }), undefined);
  }
  assert.ok(readCommandAction('sensor.setCalibration', { calibrationOffsetC: -10 }));
  assert.equal(readCommandAction('sensor.setCalibration', { calibrationOffsetC: 10.1 }), undefined);
  assert.equal(readCommandAction('heat.disable', { extra: 1 }), undefined);
  assert.equal(readCommandRequest({ ...request(), deviceUid: 'another/controller' }), undefined);
  assert.equal(readCommandRequest({ ...request(), requestId: 'invalid' }), undefined);
  for (const sessionId of [null, '', 'invalid']) {
    assert.equal(readCommandRequest({ ...request(), sessionId }), undefined);
  }
  for (const disableRevision of [-1, 1.5, null, '0', 4_294_967_296]) {
    assert.equal(readCommandRequest({ ...request(), disableRevision }), undefined);
  }
});

test('latest and result decoding handles missing temperature and rejects mismatched acknowledgements', () => {
  assert.ok(readLatest(telemetry({ updatedAt: now, temperatureValid: false, temperatureC: undefined })));
  assert.equal(readLatest(telemetry({ updatedAt: now, restorePending: true })).restorePending, true);
  assert.equal(readLatest(telemetry({ updatedAt: now, restorePending: 'true' })), undefined);
  assert.equal(readLatest(telemetry({ updatedAt: now, temperatureC: undefined })), undefined);
  assert.equal(readLatest(telemetry({ updatedAt: now, schemaVersion: 2 })), undefined);
  assert.equal(readResult({ id: 'other', status: 'applied', timestamp: now, reason: '' }, 'request'), undefined);
});

test('heap readings are optional and contain an ordered pair of byte counts', () => {
  assert.equal(readLatest(telemetry({ updatedAt: now })).heap, undefined);
  const heap = { freeBytes: 80092, minimumFreeBytes: 0 };
  assert.deepEqual(readLatest(telemetry({ updatedAt: now, heap })).heap, heap);
  for (const invalid of [null, { freeBytes: 10 }, { freeBytes: 10, minimumFreeBytes: 11 },
    { freeBytes: 10.5, minimumFreeBytes: 0 }, { freeBytes: 4_294_967_296, minimumFreeBytes: 0 }]) {
    assert.equal(readLatest(telemetry({ updatedAt: now, heap: invalid })), undefined);
  }
});

test('restart requests respect the current target and require firmware support', () => {
  const device = { latest: telemetry({ updatedAt: now, targetC: 60, restartC: 59 }) };
  assert.equal(readLatest(device.latest).restartC, 59);
  assert.equal(readLatest({ ...device.latest, restartC: 60 }), undefined);
  for (const restartC of [40, 55, 59.5]) {
    const command = request('heat.setRestart', { restartC });
    assert.deepEqual(readCommandRequest(command)?.payload, { restartC });
    assert.equal(submitCommand(device, command, now).write.commands[command.requestId].payload.restartC, restartC);
  }
  for (const restartC of [39.5, 60, 60.5]) {
    assert.deepEqual(submitCommand(device, request('heat.setRestart', { restartC }), now), { error: 'restart-range' });
  }
  assert.equal(readCommandAction('heat.setRestart', { restartC: 55.25 }), undefined);
  assert.deepEqual(submitCommand(state(), request('heat.setRestart', { restartC: 55 }), now), { error: 'restart-range' });
});

test('command issuance binds a fresh session and expires after 30 seconds', () => {
  const command = request('heat.setTarget', { targetC: 60 });
  const device = state();
  const submitted = submitCommand(device, command, now);
  assert.deepEqual(submitted.receipt, {
    id: command.requestId, expiresAt: now + 30_000, sessionId: device.latest.sessionId,
  });
  assert.deepEqual(submitted.write.commands[command.requestId], {
    id: command.requestId, type: command.type, payload: command.payload, sequence: 1,
    sessionId: device.latest.sessionId, issuedAt: now, expiresAt: now + 30_000,
  });
  assert.deepEqual(submitCommand(device, command, now + 45_000), { error: 'offline' });
  assert.deepEqual(submitCommand({ latest: { ...device.latest, commandReady: false } }, command, now), { error: 'offline' });
  assert.deepEqual(submitCommand({ latest: { ...device.latest, sessionId: '' } }, command, now), { error: 'offline' });
});

test('firmware updates require a capable, idle controller', () => {
  const command = request('firmware.update');
  assert.ok(readCommandRequest(command));
  assert.equal(readCommandAction(command.type, { extra: 1 }), undefined);
  const latest = { ...state().latest, canUpdate: true, heatEnabled: false, heaterOn: false,
    fanOn: false, monitorPhase: 0, updatePhase: 0 };
  for (const phase of [0, 7, 8]) {
    const submitted = submitCommand({ latest: { ...latest, updatePhase: phase } }, command, now);
    assert.equal(submitted.write.commands[command.requestId].type, 'firmware.update');
  }
  for (const override of [
    { canUpdate: undefined }, { heatEnabled: true }, { restorePending: true },
    { heaterOn: true }, { fanOn: true }, { monitorPhase: 1 }, { updatePhase: 3 },
  ]) {
    assert.deepEqual(submitCommand({ latest: { ...latest, ...override } }, command, now),
      { error: 'update-unavailable' });
  }
  assert.equal(readLatest({ ...latest, canUpdate: 'true' }), undefined);
});

test('retries retain the submitted receipt while the device is offline', () => {
  const command = request();
  const first = submitCommand(state(), command, now);
  const retry = submitCommand(first.write, command, now + 45_000);
  assert.deepEqual(retry.receipt, first.receipt);
  assert.equal(retry.write, undefined);
  assert.deepEqual(submitCommand(first.write, { ...command, type: 'heat.disable' }, now), { error: 'reused-request' });

  const reconnected = {
    ...first.write,
    latest: telemetry({ updatedAt: now + 1, sessionId: '89abcdef0123456789abcdef01234567' }),
  };
  assert.deepEqual(submitCommand(reconnected, command, now + 1), { receipt: first.receipt });
  assert.deepEqual(submitCommand(reconnected, { ...command, sessionId: reconnected.latest.sessionId }, now + 1),
    { error: 'reused-request' });
  const { sessionId: _originalSession, ...legacyRequest } = command;
  assert.deepEqual(submitCommand(reconnected, legacyRequest, now + 1), { receipt: first.receipt });
  delete reconnected.commandReceipts[command.requestId].sessionId;
  assert.deepEqual(submitCommand(reconnected, command, now + 1), {
    receipt: { id: command.requestId, expiresAt: now + 30_000 },
  });
});

test('fresh readings accept Disable before the command session opens', () => {
  const device = { latest: telemetry({ updatedAt: now, commandReady: false, sessionId: '' }) };
  const { sessionId: _session, ...disable } = request('heat.disable');
  const stop = submitCommand(device, disable, now);
  assert.equal(stop.write.commands[disable.requestId].type, 'heat.disable');
  assert.equal(stop.receipt.sessionId, '00000000000000000000000000000000');
  assert.equal(stop.write.disableRevision, 1);
  assert.deepEqual(submitCommand(device, request(), now), { error: 'offline' });
  assert.deepEqual(submitCommand(device, disable, now + 45_000), { error: 'offline' });
});

test('a new Enable must match the session in which it was submitted', () => {
  const oldEnable = request();
  const reconnected = { latest: telemetry({ updatedAt: now, sessionId: '89abcdef0123456789abcdef01234567' }) };
  assert.deepEqual(submitCommand(reconnected, oldEnable, now), { error: 'stale-session' });
  const { sessionId: _oldSession, ...legacyEnable } = oldEnable;
  assert.deepEqual(submitCommand(reconnected, legacyEnable, now), { error: 'stale-session' });

  const fresh = { ...request(), sessionId: reconnected.latest.sessionId };
  assert.equal(submitCommand(reconnected, fresh, now).receipt.sessionId, fresh.sessionId);
  const disable = request('heat.disable');
  assert.equal(submitCommand(reconnected, disable, now).receipt.sessionId, fresh.sessionId);
});

test('the queue keeps an outstanding Disable across token renewal', () => {
  const stop = request('heat.disable');
  const disabled = submitCommand(state(), stop, now).write;
  const previous = request('heat.setTarget', { targetC: 55 }, disabled.disableRevision);
  const queued = submitCommand(disabled, previous, now + 1).write;
  const latest = telemetry({ updatedAt: now + 2, sessionId: '89abcdef0123456789abcdef01234567' });
  const current = { ...queued, latest };
  const fresh = { ...request('heat.enable', {}, current.disableRevision), sessionId: latest.sessionId };

  const carried = submitCommand(current, fresh, now + 2).write;
  assert.deepEqual(Object.keys(carried.commands), [stop.requestId, fresh.requestId]);
  assert.equal(carried.commands[stop.requestId].sessionId, stop.sessionId);
  assert.equal(carried.commands[stop.requestId].expiresAt, now + 30_000);
  assert.equal(carried.commands[fresh.requestId].sessionId, latest.sessionId);

  const expired = submitCommand(current, fresh, now + 30_000).write;
  assert.deepEqual(Object.keys(expired.commands), [fresh.requestId]);
});

test('the queue leaves space for Disable and prunes expired and superseded records', () => {
  let device = state();
  for (let i = 0; i < 7; i++) device = submitCommand(device, request(), now).write;
  assert.equal(Object.keys(device.commands).length, 7);
  assert.deepEqual(submitCommand(device, request(), now), { error: 'queue-full' });
  assert.ok(Buffer.byteLength(JSON.stringify({ path: '/', data: device.commands })) < 6144);
  const stop = request('heat.disable');
  const disabled = submitCommand(device, stop, now).write;
  assert.deepEqual(Object.keys(disabled.commands), [stop.requestId]);
  assert.equal(disabled.commandSequence, 8);
  assert.equal(Object.keys(disabled.commandReceipts).length, 8);
  const next = submitCommand(disabled, request('heat.enable', {}, disabled.disableRevision), now + 30_001).write;
  assert.equal(Object.keys(next.commands).length, 1);
  assert.equal(next.commandSequence, 9);
});

test('Disable rejects earlier commands arriving later and allows a fresh Enable', () => {
  const inFlight = request();
  const queued = request('heat.setTarget', { targetC: 62 });
  const beforeDisable = submitCommand(state(), queued, now);
  const disable = request('heat.disable');
  const disabled = submitCommand(beforeDisable.write, disable, now + 1).write;
  assert.equal(disabled.disableRevision, 2);
  assert.deepEqual(submitCommand(disabled, inFlight, now + 2), { error: 'superseded' });
  assert.deepEqual(submitCommand(disabled, queued, now + 2), { receipt: beforeDisable.receipt });
  assert.equal(submitCommand(disabled, disable, now + 2).write, undefined);

  const fresh = request('heat.enable', {}, disabled.disableRevision);
  assert.equal(submitCommand(disabled, fresh, now + 3).write.commands[fresh.requestId].sequence, 3);
  assert.equal(submitCommand(disabled, request('heat.disable'), now + 3).write.disableRevision, 3);
});

test('confirmed commands free queue capacity and retain their receipts', () => {
  let device = state();
  const commands = Array.from({ length: 7 }, () => request());
  for (const command of commands) device = submitCommand(device, command, now).write;
  device.results = Object.fromEntries(commands.map(({ requestId: id }, index) => [id, {
    id, status: ['applied', 'rejected', 'expired'][index % 3], reason: '', timestamp: now + 1,
  }]));
  const next = request('heat.setTarget', { targetC: 64 });
  const submitted = submitCommand(device, next, now + 1);
  assert.deepEqual(Object.keys(submitted.write.commands), [next.requestId]);
  assert.equal(submitted.write.commandSequence, 8);
  assert.equal(Object.keys(submitted.write.results).length, 7);
  const retry = submitCommand(submitted.write, commands[0], now + 2);
  assert.deepEqual(retry.receipt, {
    id: commands[0].requestId, expiresAt: now + 30_000, sessionId: device.latest.sessionId,
  });
  assert.equal(retry.write, undefined);
});
