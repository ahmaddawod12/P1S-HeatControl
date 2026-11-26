import { expect, test } from '@playwright/test';
import { createHash } from 'node:crypto';
import { confirmPairing, database, databaseUrl, enroll, jsonRequest, pair, passwordAccount, receipt, runId, signInDevice, telemetry } from './emulator-helpers.mjs';

test('a claimed code stops showing as waiting when the controller misses its pairing deadline', async ({ page }) => {
  const owner = await passwordAccount(`expired-pairing-${runId()}`);
  await page.clock.install();
  await page.goto('/');
  await page.getByLabel('Email', { exact: true }).fill(owner.email);
  await page.getByLabel('Password', { exact: true }).fill(owner.password);
  await page.getByRole('button', { name: 'Sign in', exact: true }).click();
  await expect(page.getByRole('heading', { name: 'Pair your first controller' })).toBeVisible();

  const begin = await pair('begin', { deviceUid: '' });
  expect(begin.status).toBe(200);
  const sessionKey = createHash('sha256').update(begin.data.sessionSecret).digest('hex');
  const sessionUrl = databaseUrl(`pairing/sessions/${sessionKey}`);
  expect((await jsonRequest(sessionUrl, 'PATCH', { expiresAt: Date.now() + 60_000 }, 'owner')).status).toBe(200);
  await page.locator('#pair-first').click();
  const dialog = page.getByRole('dialog');
  await dialog.getByLabel('Pairing code').fill(begin.data.code);
  await dialog.getByRole('button', { name: 'Pair controller' }).click();
  await expect(dialog).toBeHidden({ timeout: 65_000 });
  await expect(page.locator('#pairing-status')).toHaveText('Waiting for the controller to finish pairing.');

  expect((await jsonRequest(sessionUrl, 'PATCH', { expiresAt: Date.now() - 1 }, 'owner')).status).toBe(200);
  expect((await pair('poll', { sessionSecret: begin.data.sessionSecret })).status).toBe(410);
  await page.clock.fastForward(61_000);
  await expect(page.locator('#pairing-status')).toHaveText('Pairing code expired. Get a new code from the controller and try again.');
  await expect(page.locator('#devices li')).toHaveCount(0);
  await expect(page.locator('#pair-first')).toBeEnabled();
});

test('pair a browser account, control a device and render confirmed and missing replies', async ({ page }, info) => {
  const owner = await passwordAccount(`browser-${runId()}`);
  await page.clock.install();
  await page.goto('/');
  const email = page.getByLabel('Email', { exact: true });
  const password = page.getByLabel('Password', { exact: true });
  const login = page.getByRole('button', { name: 'Sign in', exact: true });
  await email.fill(owner.email);
  await password.fill('Incorrect-test-password');
  await login.click();
  await expect(page.locator('#sign-in-error')).toHaveText('Could not sign in with that email and password.');
  await expect(password).toHaveValue('');
  await password.fill(owner.password);
  await login.click();
  await expect(page.getByRole('heading', { name: 'Pair your first controller' })).toBeVisible();

  const begin = await pair('begin', { deviceUid: '' });
  expect(begin.status).toBe(200);
  let claims = 0;
  await page.route('**/claimPairing', async route => {
    if (route.request().method() !== 'POST' || ++claims > 1) return route.continue();
    const response = await route.fetch({ timeout: 30_000 });
    expect(response.ok()).toBeTruthy();
    // The backend commits the claim and the browser loses its first reply.
    await route.abort('failed');
  });
  await page.locator('#pair-first').click();
  const dialog = page.getByRole('dialog');
  await dialog.getByLabel('Pairing code').fill(begin.data.code);
  await dialog.getByRole('button', { name: 'Pair controller' }).click();
  await expect(dialog).toBeHidden({ timeout: 65_000 });
  expect(claims).toBe(2);
  await expect(page.locator('#pairing-status')).toHaveText('Waiting for the controller to finish pairing.');
  await expect(page.locator('#devices li')).toHaveCount(0);

  const poll = await pair('poll', { sessionSecret: begin.data.sessionSecret });
  expect(poll.status).toBe(200);
  const id = poll.data.deviceUid;
  const device = await signInDevice(poll.data.customToken);
  expect((await confirmPairing(device)).status).toBe(204);
  await expect(page.locator('#last-seen')).toHaveText('Waiting for first update');
  await expect(page.locator('#pairing-status')).toBeHidden();
  const path = `devices/${id}`;
  expect((await database(`${path}/latest`, device, 'PUT', telemetry())).status).toBe(200);
  await expect(page.getByRole('heading', { name: 'Enclosure P1S' })).toBeVisible();
  await expect(page.locator('#freshness')).toHaveText('Online');
  await expect(page.locator('#temperature')).toHaveText('52.4 °C');
  await page.screenshot({ path: info.outputPath('dashboard-desktop.png'), fullPage: true });

  await page.getByLabel('Target temperature').fill('58.5');
  await page.getByRole('button', { name: 'Set target' }).click();
  const targetResult = page.locator('#command-results li').filter({ hasText: 'Set target' });
  await expect(targetResult).toContainText('Pending', { timeout: 30_000 });
  await expect(page.locator('#target')).toHaveText('60.0 °C');
  const beforeStop = (await database(`${path}/commands`, device)).data;
  const target = Object.values(beforeStop).find(command => command.type === 'heat.setTarget');
  expect(target.payload).toEqual({ targetC: 58.5 });

  const disableButton = page.getByRole('button', { name: 'Disable heat' });
  await expect(disableButton).toBeEnabled();
  await disableButton.click();
  const stopResult = page.locator('#command-results li').filter({ hasText: 'Disable heat' });
  await expect(stopResult).toContainText('Pending', { timeout: 30_000 });
  const stop = Object.values((await database(`${path}/commands`, device)).data)[0];
  expect(stop.type).toBe('heat.disable');
  expect((await database(`${path}/results/${stop.id}`, device, 'PUT', receipt(stop.id))).status).toBe(200);
  await expect(stopResult).toContainText('Applied');
  await expect(page.locator('#heater')).toHaveText('On');
  expect((await database(`${path}/latest`, device, 'PUT', telemetry({
    state: 'idle', heaterOn: false, fanOn: false, heatEnabled: false,
  }))).status).toBe(200);
  await expect(page.locator('#heater')).toHaveText('Off');

  await page.clock.fastForward(31_000);
  await expect(targetResult).toContainText('Unconfirmed');
  expect((await database(`${path}/results/${target.id}`, device, 'PUT', receipt(target.id, 'expired', 'deadline'))).status).toBe(200);
  await expect(targetResult).toContainText('Expired');
  await page.setViewportSize({ width: 390, height: 844 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(390);
  await page.screenshot({ path: info.outputPath('dashboard-mobile.png'), fullPage: true });

  await page.clock.fastForward(16_000);
  await expect(page.locator('#freshness')).toHaveText('Stale');
  await expect(disableButton).toBeDisabled();
  await page.getByRole('button', { name: 'Sign out' }).click();
  await expect(page.getByRole('heading', { name: 'Chamber control' })).toBeVisible();
  await expect(page.locator('#temperature')).toHaveText('');
  await expect(page.locator('#command-results li')).toHaveCount(0);

  const unapproved = await passwordAccount(`browser-unapproved-${runId()}`, { approved: false });
  await email.fill(unapproved.email);
  await password.fill(unapproved.password);
  await login.click();
  await expect(page.locator('#workspace-error')).toHaveText('Dashboard access requires an approved email and password account.');
  await expect(page.locator('#open-pairing')).toBeHidden();
});

test('one owner can pair multiple controllers and keep their controls and results separate', async ({ page }) => {
  const owner = await passwordAccount(`browser-multiple-boards-${runId()}`);
  const first = await enroll(owner);
  const firstPath = `devices/${first.uid}`;
  const firstReading = telemetry({ boardId: '7C87CE01020A', heap: { freeBytes: 80092, minimumFreeBytes: 25328 } });
  expect((await database(`${firstPath}/latest`, first.credentials, 'PUT', firstReading)).status).toBe(200);

  await page.goto('/');
  await page.getByLabel('Email', { exact: true }).fill(owner.email);
  await page.getByLabel('Password', { exact: true }).fill(owner.password);
  await page.getByRole('button', { name: 'Sign in', exact: true }).click();

  const selectedBoard = page.locator('#device-id');
  const targetInput = page.getByLabel('Target temperature');
  const pairingStatus = page.locator('#pairing-status');
  const rows = page.locator('#devices button');
  const openPairing = page.locator('#open-pairing');
  await expect(selectedBoard).toHaveText('Board 7C87CE01020A');
  await expect(page.locator('#free-ram')).toHaveText('78.2 KiB');
  await expect(page.locator('#heap-low-water')).toHaveText('24.7 KiB');
  await targetInput.fill('55');
  const sessions = await Promise.all([pair('begin', { deviceUid: '' }), pair('begin', { deviceUid: '' })]);
  for (const session of sessions) {
    expect(session.status).toBe(200);
    await openPairing.click();
    const dialog = page.getByRole('dialog');
    await dialog.getByLabel('Pairing code').fill(session.data.code);
    await dialog.getByRole('button', { name: 'Pair controller' }).click();
    await expect(dialog).toBeHidden();
  }
  await expect(pairingStatus).toHaveText('Waiting for 2 controllers to finish pairing.');

  const poll = await pair('poll', { sessionSecret: sessions[0].data.sessionSecret });
  expect(poll.status).toBe(200);
  expect(poll.data.deviceUid).not.toBe(first.uid);
  const secondPath = `devices/${poll.data.deviceUid}`;
  const second = await signInDevice(poll.data.customToken);
  expect((await confirmPairing(second)).status).toBe(204);
  const secondReading = telemetry({
    boardId: '7C87CE01020B', temperatureC: 43, targetC: 48, sessionId: 'fedcba9876543210fedcba9876543210',
  });
  expect((await database(`${secondPath}/latest`, second, 'PUT', secondReading)).status).toBe(200);
  await expect(rows).toHaveCount(2);
  await expect(pairingStatus).toHaveText('Waiting for the controller to finish pairing.');
  await expect(selectedBoard).toHaveText('Board 7C87CE01020A');
  await expect(targetInput).toHaveValue('55');

  const results = page.locator('#command-results li');
  await page.getByRole('button', { name: 'Set target' }).click();
  await expect(results).toContainText('Pending');
  const [target] = Object.values((await database(`${firstPath}/commands`, first.credentials)).data);
  expect(target.payload).toEqual({ targetC: 55 });
  expect(target.sessionId).toBe(firstReading.sessionId);
  expect((await database(`${secondPath}/commands`, second)).data).toBeNull();

  const secondRow = rows.filter({ hasText: 'Board 7C87CE01020B' });
  await secondRow.click();
  await expect(selectedBoard).toHaveText('Board 7C87CE01020B');
  await expect(page.locator('#temperature')).toHaveText('43.0 °C');
  await expect(page.locator('#free-ram')).toHaveText('—');
  await expect(page.locator('#heap-low-water')).toHaveText('—');
  await expect(targetInput).toHaveValue('48');
  await expect(results).toHaveCount(0);
  await page.getByRole('button', { name: 'Disable heat' }).click();
  await expect(results).toContainText('Pending');
  const [disable] = Object.values((await database(`${secondPath}/commands`, second)).data);
  expect(disable.type).toBe('heat.disable');
  expect(disable.sessionId).toBe(secondReading.sessionId);
  expect(Object.keys((await database(`${firstPath}/commands`, first.credentials)).data)).toEqual([target.id]);
  expect((await database(`${secondPath}/commands`, first.credentials)).status).toBe(401);

  const targetResult = `${firstPath}/results/${target.id}`;
  expect((await database(targetResult, second, 'PUT', receipt(target.id))).status).toBe(401);
  expect((await database(targetResult, first.credentials, 'PUT', receipt(target.id))).status).toBe(200);
  expect((await database(`${firstPath}/latest`, first.credentials, 'PUT', { ...firstReading, targetC: 55 })).status).toBe(200);
  await expect(results).toHaveCount(1);
  await expect(results).toContainText('Disable heat');
  await expect(results).toContainText('Pending');

  expect((await database(`${secondPath}/results/${disable.id}`, second, 'PUT', receipt(disable.id))).status).toBe(200);
  await expect(results).toContainText('Applied');
  await rows.filter({ hasText: 'Board 7C87CE01020A' }).click();
  await expect(page.locator('#target')).toHaveText('55.0 °C');
  await expect(page.locator('#free-ram')).toHaveText('78.2 KiB');
  await expect(results).toHaveCount(1);
  await expect(results).toContainText('Set target');
  await expect(results).toContainText('Applied');

  await secondRow.click();
  const membership = databaseUrl(`owners/${owner.localId}/devices/${poll.data.deviceUid}`);
  expect((await jsonRequest(membership, 'DELETE', undefined, 'owner')).status).toBe(200);
  await expect(rows).toHaveCount(1);
  await expect(page.locator('#device-view')).toBeHidden();
  await expect(page.getByRole('heading', { name: 'Select a controller' })).toBeVisible();
});

test('identifies unnamed controllers even when their short IDs collide', async ({ page }) => {
  const owner = await passwordAccount(`browser-controllers-${runId()}`);
  const prefix = `p1s_${runId()}`;
  const left = `${prefix}-left-1234abcd`;
  const middle = `${prefix}-middle-5678abcd`;
  const right = `${prefix}-${'right'.repeat(18)}-1234abcd`;
  const uids = [left, middle, right];
  const directory = databaseUrl(`owners/${owner.localId}/devices`);
  const linked = await jsonRequest(directory, 'PUT', Object.fromEntries(uids.map(uid => [uid, true])), 'owner');
  expect(linked.status).toBe(200);
  const uploaded = await Promise.all(uids.map(uid =>
    jsonRequest(databaseUrl(`devices/${uid}/latest`), 'PUT', telemetry({ name: '' }), 'owner')));
  expect(uploaded.map(({ status }) => status)).toEqual([200, 200, 200]);

  await page.goto('/');
  await page.getByLabel('Email', { exact: true }).fill(owner.email);
  await page.getByLabel('Password', { exact: true }).fill(owner.password);
  await page.getByRole('button', { name: 'Sign in', exact: true }).click();
  await expect(page.locator('#devices button')).toHaveCount(3);
  await expect(page.getByRole('heading', { name: 'Select a controller' })).toBeVisible();
  await expect(page.locator('#device-view')).toBeHidden();
  const headerId = page.locator('#device-id');
  for (const label of [left, '5678abcd', right]) {
    const identifier = `ID ${label}`;
    const button = page.getByRole('button', { name: `Controller ${identifier} Online`, exact: true });
    await button.click();
    await expect(button).toHaveAttribute('aria-current', 'true');
    await expect(headerId).toHaveText(identifier);
  }

  await page.setViewportSize({ width: 390, height: 844 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(390);
});

for (const setting of [
  { label: 'Target temperature', button: 'Set target', key: 'targetC', submitted: 58.5, draft: 65 },
  { label: 'Sensor calibration offset', button: 'Set calibration', key: 'calibrationOffsetC', submitted: -1, draft: 2 },
]) {
  test(`a late ${setting.button} result preserves the new draft`, async ({ page }) => {
    const owner = await passwordAccount(`browser-draft-${runId()}`);
    const { uid, credentials } = await enroll(owner);
    const path = `devices/${uid}`;
    expect((await database(`${path}/latest`, credentials, 'PUT', telemetry())).status).toBe(200);
    await page.clock.install();
    await page.goto('/');
    await page.getByLabel('Email', { exact: true }).fill(owner.email);
    await page.getByLabel('Password', { exact: true }).fill(owner.password);
    await page.getByRole('button', { name: 'Sign in', exact: true }).click();

    const input = page.getByLabel(setting.label, { exact: true });
    await input.fill(String(setting.submitted));
    await page.getByRole('button', { name: setting.button, exact: true }).click();
    const result = page.locator('#command-results li').filter({ hasText: setting.button });
    await expect(result).toContainText('Pending');
    const [command] = Object.values((await database(`${path}/commands`, credentials)).data);
    expect(command.payload).toEqual({ [setting.key]: setting.submitted });

    await page.clock.fastForward(31_000);
    await expect(result).toContainText('Unconfirmed');
    await input.fill(String(setting.draft));
    await input.blur();
    expect((await database(`${path}/latest`, credentials, 'PUT',
      telemetry({ [setting.key]: setting.submitted }))).status).toBe(200);
    expect((await database(`${path}/results/${command.id}`, credentials, 'PUT', receipt(command.id))).status).toBe(200);
    await expect(result).toContainText('Applied');
    await expect(input).toHaveValue(String(setting.draft));
  });
}

test('shows the high-target warning for an edited or reported target above 60 °C', async ({ page }) => {
  const owner = await passwordAccount(`high-target-${runId()}`);
  const { uid, credentials } = await enroll(owner);
  const statusPath = `devices/${uid}/latest`;
  expect((await database(statusPath, credentials, 'PUT', telemetry())).status).toBe(200);
  await page.goto('/');
  await page.getByLabel('Email', { exact: true }).fill(owner.email);
  await page.getByLabel('Password', { exact: true }).fill(owner.password);
  await page.getByRole('button', { name: 'Sign in', exact: true }).click();
  const target = page.getByLabel('Target temperature');
  const warning = page.locator('#high-target-warning');
  await expect(target).toBeEditable();
  await expect(warning).toBeHidden();

  await target.fill('60.5');
  await expect(warning).toBeVisible();
  await target.fill('60');
  await expect(warning).toBeHidden();

  expect((await database(statusPath, credentials, 'PUT', telemetry({ targetC: 70 }))).status).toBe(200);
  await expect(page.locator('#target')).toHaveText('70.0 °C');
  await expect(warning).toBeVisible();
  await target.fill('55');
  await expect(warning).toBeVisible();
  expect((await database(statusPath, credentials, 'PUT', telemetry({ targetC: 60 }))).status).toBe(200);
  await expect(warning).toBeHidden();
});

test('target confirmation waits for updated restart limits', async ({ page }) => {
  const owner = await passwordAccount(`browser-restart-${runId()}`);
  const { uid, credentials } = await enroll(owner);
  const path = `devices/${uid}`;
  expect((await database(`${path}/latest`, credentials, 'PUT', telemetry({ targetC: 55, restartC: 54 }))).status).toBe(200);
  await page.goto('/');
  await page.getByLabel('Email', { exact: true }).fill(owner.email);
  await page.getByLabel('Password', { exact: true }).fill(owner.password);
  await page.getByRole('button', { name: 'Sign in', exact: true }).click();

  const restart = page.getByLabel('Restart heating at', { exact: true });
  const saveRestart = page.getByRole('button', { name: 'Set restart temperature', exact: true });
  const targetResult = page.locator('#command-results li').filter({ hasText: 'Set target' });
  await expect(restart).toHaveAttribute('max', '54.5');
  await page.getByLabel('Target temperature', { exact: true }).fill('60');
  await page.getByRole('button', { name: 'Set target', exact: true }).click();
  await expect(targetResult).toContainText('Pending');
  const [target] = Object.values((await database(`${path}/commands`, credentials)).data);
  expect((await database(`${path}/results/${target.id}`, credentials, 'PUT', receipt(target.id))).status).toBe(200);

  await expect(targetResult).toContainText('Refreshing settings');
  await expect(saveRestart).toBeDisabled();
  await expect(page.getByRole('button', { name: 'Disable heat', exact: true })).toBeEnabled();

  expect((await database(`${path}/latest`, credentials, 'PUT', telemetry({ targetC: 60, restartC: 59 }))).status).toBe(200);
  await expect(targetResult).toContainText('Applied');
  await expect(restart).toHaveAttribute('max', '59.5');
  await expect(restart).toHaveValue('59');
  await restart.fill('55');
  await saveRestart.click();
  const restartResult = page.locator('#command-results li').filter({ hasText: 'Set restart temperature' });
  await expect(restartResult).toContainText('Pending');
  const command = Object.values((await database(`${path}/commands`, credentials)).data)
    .find(({ type }) => type === 'heat.setRestart');
  expect(command?.payload).toEqual({ restartC: 55 });
});

test('a confirmed setting releases the controls when updated readings are late', async ({ page }) => {
  const owner = await passwordAccount(`browser-settings-timeout-${runId()}`);
  const { uid, credentials } = await enroll(owner);
  const path = `devices/${uid}`;
  expect((await database(`${path}/latest`, credentials, 'PUT', telemetry())).status).toBe(200);
  await page.clock.install();
  await page.goto('/');
  await page.getByLabel('Email', { exact: true }).fill(owner.email);
  await page.getByLabel('Password', { exact: true }).fill(owner.password);
  await page.getByRole('button', { name: 'Sign in', exact: true }).click();
  await expect(page.locator('#freshness')).toHaveText('Online');

  const target = page.getByLabel('Target temperature', { exact: true });
  const result = page.locator('#command-results li').filter({ hasText: 'Set target' });
  const setCalibration = page.getByRole('button', { name: 'Set calibration', exact: true });
  await target.fill('62');
  await page.getByRole('button', { name: 'Set target', exact: true }).click();
  await expect(result).toContainText('Pending');
  const [command] = Object.values((await database(`${path}/commands`, credentials)).data);
  expect((await database(`${path}/results/${command.id}`, credentials, 'PUT', receipt(command.id))).status).toBe(200);
  await expect(result).toContainText('Refreshing settings');
  await expect(setCalibration).toBeDisabled();

  await page.clock.fastForward(31_000);
  await expect(result).toContainText('Applied');
  const waiting = 'Waiting for updated controller readings.';
  await expect(result).toContainText(waiting);
  await expect(setCalibration).toBeEnabled();
  await expect(page.locator('#target')).toHaveText('60.0 °C');

  await target.fill('65');
  await target.blur();
  expect((await database(`${path}/latest`, credentials, 'PUT', telemetry({ targetC: 62 }))).status).toBe(200);
  await expect(page.locator('#target')).toHaveText('62.0 °C');
  await expect(result).not.toContainText(waiting);
  await expect(target).toHaveValue('65');
});

test('Disable supersedes a delayed Enable and the dashboard can send a fresh one', async ({ page }) => {
  const owner = await passwordAccount(`browser-disable-${runId()}`);
  const { uid, credentials } = await enroll(owner);
  const path = `devices/${uid}`;
  expect((await database(`${path}/latest`, credentials, 'PUT', telemetry({
    state: 'idle', heatEnabled: false, heaterOn: false, fanOn: false,
  }))).status).toBe(200);

  const firstEnable = Promise.withResolvers();
  const releaseEnable = Promise.withResolvers();
  let delayed = false;
  await page.route('**/issueCommand', async route => {
    if (route.request().method() === 'POST' && route.request().postDataJSON().data.type === 'heat.enable' && !delayed) {
      delayed = true;
      firstEnable.resolve(route.request().postDataJSON().data);
      await releaseEnable.promise;
    }
    await route.continue();
  });

  await page.goto('/');
  await page.getByLabel('Email', { exact: true }).fill(owner.email);
  await page.getByLabel('Password', { exact: true }).fill(owner.password);
  await page.getByRole('button', { name: 'Sign in', exact: true }).click();
  const enableButton = page.getByRole('button', { name: 'Enable heat', exact: true });
  const enableResults = page.locator('#command-results li').filter({ hasText: 'Enable heat' });
  await enableButton.click();
  const original = await firstEnable.promise;
  try {
    expect(original.disableRevision).toBe(0);
    expect(original.sessionId).toBe(telemetry().sessionId);
    await page.getByRole('button', { name: 'Disable heat', exact: true }).click();
    const disableResult = page.locator('#command-results li').filter({ hasText: 'Disable heat' });
    await expect(disableResult).toContainText('Pending', { timeout: 30_000 });
    const inbox = (await database(`${path}/commands`, credentials)).data;
    const stop = Object.values(inbox)[0];
    expect(stop.type).toBe('heat.disable');
    expect((await database(`${path}/results/${stop.id}`, credentials, 'PUT', receipt(stop.id))).status).toBe(200);
    await expect(disableResult).toContainText('Applied');
  } finally {
    releaseEnable.resolve();
  }
  await expect(enableResults).toContainText('Rejected');
  await expect(enableResults).toContainText('Disable heat superseded this command.');
  expect((await database(`${path}/commands/${original.requestId}`, credentials)).data).toBeNull();

  await enableButton.click();
  await expect(enableResults.first()).toContainText('Pending', { timeout: 30_000 });
  const inbox = (await database(`${path}/commands`, credentials)).data;
  expect(Object.values(inbox).map(command => command.type)).toEqual(['heat.enable']);
});

test('a delayed Enable cannot cross into a replacement command session', async ({ page }) => {
  const owner = await passwordAccount(`browser-delayed-enable-${runId()}`);
  const { uid, credentials } = await enroll(owner);
  const path = `devices/${uid}`;
  const initial = telemetry({ state: 'idle', heatEnabled: false, heaterOn: false, fanOn: false });
  expect((await database(`${path}/latest`, credentials, 'PUT', { ...initial, restorePending: true })).status).toBe(200);

  const incoming = Promise.withResolvers();
  const release = Promise.withResolvers();
  let delayed = false;
  await page.route('**/issueCommand', async route => {
    if (route.request().method() === 'POST' && !delayed) {
      delayed = true;
      incoming.resolve(route.request().postDataJSON().data);
      await release.promise;
    }
    await route.continue();
  });

  await page.goto('/');
  await page.getByLabel('Email', { exact: true }).fill(owner.email);
  await page.getByLabel('Password', { exact: true }).fill(owner.password);
  await page.getByRole('button', { name: 'Sign in', exact: true }).click();
  const enable = page.getByRole('button', { name: 'Enable heat', exact: true });
  const results = page.locator('#command-results li').filter({ hasText: 'Enable heat' });
  await expect(page.locator('#heat-mode')).toHaveText('Heating recovery pending');
  await expect(enable).toBeDisabled();
  await expect(page.getByRole('button', { name: 'Disable heat', exact: true })).toBeEnabled();
  expect((await database(`${path}/latest`, credentials, 'PUT', { ...initial, restorePending: false })).status).toBe(200);
  await expect(page.locator('#heat-mode')).toHaveText('Heating disabled');
  await enable.click();
  const original = await incoming.promise;
  const sessionId = '89abcdef0123456789abcdef01234567';
  try {
    expect(original.sessionId).toBe(initial.sessionId);
    expect((await database(`${path}/latest`, credentials, 'PUT', { ...initial, sessionId })).status).toBe(200);
  } finally {
    release.resolve();
  }
  await expect(results.first()).toContainText('The controller session changed.');
  expect((await database(`${path}/commands/${original.requestId}`, credentials)).data).toBeNull();

  await enable.click();
  await expect(results.first()).toContainText('Pending');
  const inbox = (await database(`${path}/commands`, credentials)).data;
  expect(Object.values(inbox)).toEqual([expect.objectContaining({ type: 'heat.enable', sessionId })]);
});

test('fresh readings allow Disable before a control session is ready', async ({ page }) => {
  const owner = await passwordAccount(`browser-readings-${runId()}`);
  const { uid, credentials } = await enroll(owner);
  const path = `devices/${uid}`;
  const latest = telemetry({ commandReady: false, sessionId: '' });
  expect((await database(`${path}/latest`, credentials, 'PUT', latest)).status).toBe(200);
  await page.goto('/');
  await page.getByLabel('Email', { exact: true }).fill(owner.email);
  await page.getByLabel('Password', { exact: true }).fill(owner.password);
  await page.getByRole('button', { name: 'Sign in', exact: true }).click();

  const stop = page.getByRole('button', { name: 'Disable heat', exact: true });
  await expect(page.locator('#freshness')).toHaveText('Readings only');
  await expect(page.getByRole('button', { name: 'Set target', exact: true })).toBeDisabled();
  await expect(stop).toBeEnabled();
  await stop.click();
  const result = page.locator('#command-results li').filter({ hasText: 'Disable heat' });
  await expect(result).toContainText('Waiting for the controller to confirm');
  const [queued] = Object.values((await database(`${path}/commands`, credentials)).data);
  expect(queued.type).toBe('heat.disable');
  expect(queued.sessionId).toBe('00000000000000000000000000000000');

  expect((await database(`${path}/results/${queued.id}`, credentials, 'PUT', receipt(queued.id))).status).toBe(200);
  expect((await database(`${path}/latest`, credentials, 'PUT', telemetry({
    ...latest, state: 'idle', heatEnabled: false, heaterOn: false, fanOn: false,
  }))).status).toBe(200);
  await expect(result).toContainText('Applied');
  await expect(page.locator('#heater')).toHaveText('Off');
});

test('Disable can be retried in a new command session while the old result is pending', async ({ page }) => {
  const owner = await passwordAccount(`browser-reconnect-${runId()}`);
  const { uid, credentials } = await enroll(owner);
  const path = `devices/${uid}`;
  const initial = telemetry();
  expect((await database(`${path}/latest`, credentials, 'PUT', initial)).status).toBe(200);

  await page.goto('/');
  await page.getByLabel('Email', { exact: true }).fill(owner.email);
  await page.getByLabel('Password', { exact: true }).fill(owner.password);
  await page.getByRole('button', { name: 'Sign in', exact: true }).click();
  const disable = page.getByRole('button', { name: 'Disable heat', exact: true });
  const results = page.locator('#command-results li');
  await disable.click();
  await expect(results.first()).toContainText('Pending');
  await expect(disable).toBeDisabled();
  const [old] = Object.values((await database(`${path}/commands`, credentials)).data);
  expect(old.sessionId).toBe(initial.sessionId);

  const sessionId = '89abcdef0123456789abcdef01234567';
  expect((await database(`${path}/latest`, credentials, 'PUT', telemetry({ sessionId }))).status).toBe(200);
  await expect(disable).toBeEnabled();
  await disable.click();
  await expect(results).toHaveCount(2);
  await expect(results.first()).toContainText('Pending');
  const [current] = Object.values((await database(`${path}/commands`, credentials)).data);
  expect(current).toMatchObject({ type: 'heat.disable', sessionId });
  expect(current.id).not.toBe(old.id);
  await expect(disable).toBeDisabled();

  expect((await database(`${path}/results/${old.id}`, credentials, 'PUT', receipt(old.id))).status).toBe(200);
  await expect(results.last()).toContainText('Applied');
  await expect(disable).toBeDisabled();
  expect((await database(`${path}/results/${current.id}`, credentials, 'PUT', receipt(current.id))).status).toBe(200);
  await expect(results.first()).toContainText('Applied');
});
