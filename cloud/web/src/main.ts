import { FirebaseError } from 'firebase/app';
import { onAuthStateChanged, signInWithEmailAndPassword, signOut, type User } from 'firebase/auth';
import { onValue, ref } from 'firebase/database';
import {
  COMMAND_TTL_MS, MAX_RESTART_DROP_C, MIN_RESTART_DROP_C, PAIRING_CODE_TTL_MS, canRequestFirmwareUpdate,
  commandLabels, isCommandSession, isDisableRevision,
  isFresh, isRecord, isUid, readCommandAction,
  readLatest, readResult, type CommandAction, type CommandResult, type CommandType, type LatestStatus,
} from '../../functions/src/contract.js';
import { connectCloud } from './firebase.js';
import './style.css';

const element = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;
const signInButton = element<HTMLButtonElement>('sign-in');
const signInEmail = element<HTMLInputElement>('sign-in-email');
const signInPassword = element<HTMLInputElement>('sign-in-password');
const pairDialog = element<HTMLDialogElement>('pair-dialog');
const pairCode = element<HTMLInputElement>('pair-code');
const pairSubmit = element<HTMLButtonElement>('pair-submit');
const commandButtons = Array.from(document.querySelectorAll<HTMLButtonElement>('[data-command]'));
const targetInput = element<HTMLInputElement>('target-input');
const restartInput = element<HTMLInputElement>('restart-input');
const restartForm = element<HTMLFormElement>('restart-form');
const calibrationInput = element<HTMLInputElement>('calibration-input');
const HIGH_TARGET_C = 60;
const printerStates = ['Unknown', 'Idle', 'Preparing', 'Printing', 'Paused', 'Finished', 'Failed'];
const monitorPhases = ['Off', 'Waiting for print', 'Monitoring'];
const updatePhases = ['Idle', 'Checking', 'Cooling', 'Downloading', 'Verifying', 'Rebooting', 'Checking new firmware', 'Succeeded', 'Failed'];
const updateResults = ['', 'Up to date', 'No release found', 'Installed', 'Check failed', 'Update blocked',
  'Download failed', 'Invalid image', 'Storage failed', 'Rolled back', 'Boot check failed', 'Setup required', 'Interrupted'];
const rejectionDetails: Record<string, string> = {
  invalid_payload: 'The controller rejected the value.',
  precondition: 'The controller refused the action. Check its fault and operating state.',
  controller_error: 'The controller could not complete the action.',
  clock_unavailable: 'The controller could not verify the command time.',
  deadline: 'The command expired at the controller.',
};

type SettingField = 'targetC' | 'restartC' | 'calibrationOffsetC';
const settingFields: Partial<Record<CommandType, SettingField>> = {
  'heat.setTarget': 'targetC',
  'heat.setRestart': 'restartC',
  'sensor.setCalibration': 'calibrationOffsetC',
};

type CommandStage = 'sending' | 'pending' | 'syncing' | 'unconfirmed' | CommandResult['status'];
type TrackedCommand = {
  id: string;
  type: CommandType;
  submittedSessionId: string;
  acceptedSessionId?: string;
  stage: CommandStage;
  expiresAt: number;
  detail: string;
  setting?: { field: SettingField; value: number; edit?: symbol };
  appliedAt?: number;
  unsubscribe?: () => void;
};
type Device = {
  uid: string;
  latest?: LatestStatus;
  disableRevision?: number;
  unreadable?: boolean;
  row: HTMLLIElement;
  button: HTMLButtonElement;
  name: HTMLElement;
  identifier: HTMLElement;
  status: HTMLElement;
  commands: TrackedCommand[];
  unsubscribe?: () => void;
  unsubscribeDisableRevision?: () => void;
};

function text(node: HTMLElement, value: string) {
  if (node.textContent !== value) node.textContent = value;
}

function banner(id: string, message: string) {
  const node = element(id);
  text(node, message);
  node.hidden = message === '';
}

function message(error: unknown, fallback: string) {
  return error instanceof FirebaseError && error.message ? error.message : fallback;
}

function pairingUnconfirmed(error: unknown) {
  return error instanceof FirebaseError && ['functions/deadline-exceeded', 'functions/unavailable',
    'functions/internal', 'functions/unknown'].includes(error.code);
}

const title = (value: string) => value ? value[0].toUpperCase() + value.slice(1).replaceAll('_', ' ') : '';
const temperature = (value?: number) => value === undefined ? '—' : `${value.toFixed(1)} °C`;
const kibibytes = (bytes?: number) => bytes === undefined ? '—' : `${(bytes / 1024).toFixed(1)} KiB`;
const fraction = (value: number) => `${Math.max(0, Math.min(100, value * 1.25))}%`;

function mount(cloud: ReturnType<typeof connectCloud>) {
  let owner: User | null = null;
  let directoryUnsubscribe: (() => void) | undefined;
  let directoryState: 'loading' | 'ready' | 'unavailable' = 'loading';
  let selectedUid = '';
  const pendingPairings = new Map<string, number>();
  let connected = false;
  let timeOffset = 0;
  let pairing = false;
  let settingEdits: Partial<Record<CommandType, symbol>> = {};
  let lastResultList = '';
  const devices = new Map<string, Device>();
  const now = () => Date.now() + timeOffset;

  function resetEdits() {
    settingEdits = {};
    targetInput.value = '';
    restartInput.value = '';
    calibrationInput.value = '';
  }

  function choose(uid: string) {
    if (selectedUid === uid) return;
    selectedUid = uid;
    resetEdits();
  }

  function state(device: Device, time: number): [string, string] {
    if (device.unreadable) return ['Unavailable', 'stale'];
    const latest = device.latest;
    if (!latest) return ['Waiting for controller', 'pending'];
    if (!connected) return ['Disconnected', 'stale'];
    if (!isFresh(latest.updatedAt, time)) return ['Stale', 'stale'];
    if (!latest.commandReady || !isCommandSession(latest.sessionId)) return ['Readings only', 'pending'];
    return ['Online', 'online'];
  }

  function isBusy(command: TrackedCommand) {
    return command.stage === 'sending' || command.stage === 'pending' || command.stage === 'syncing';
  }

  function canSend(device: Device, type: CommandType) {
    const latest = device.latest;
    if (!latest || !connected || device.unreadable || !isFresh(latest.updatedAt, now())) return false;
    const pending = device.commands.filter(isBusy);
    if (type === 'heat.disable') {
      return !pending.some(command => command.type === type &&
        (command.submittedSessionId === latest.sessionId || command.acceptedSessionId === latest.sessionId));
    }
    if (!latest.commandReady || !isCommandSession(latest.sessionId)) return false;
    if (pending.length || device.disableRevision === undefined) return false;
    if (type === 'heat.enable') return !latest.heatEnabled && !latest.restorePending && latest.fault === 'none' &&
      latest.temperatureValid && latest.targetC > 0;
    if (type === 'heat.setRestart') return latest.restartC !== undefined && latest.targetC > 0;
    if (type === 'heat.ackFault') return latest.fault !== 'none';
    if (type === 'printer.monitorStart') return latest.canMonitor;
    if (type === 'printer.monitorStop') return latest.monitorPhase !== 0;
    if (type === 'firmware.update') return canRequestFirmwareUpdate(latest);
    return true;
  }

  function renderResults(device: Device) {
    const list = device.commands.map(command => [command.id, command.stage, command.detail]);
    const signature = JSON.stringify(list);
    if (signature === lastResultList) return;
    lastResultList = signature;
    element('empty-results').hidden = list.length > 0;
    element('command-results').replaceChildren(...device.commands.map(command => {
      const row = document.createElement('li');
      const copy = document.createElement('div');
      const name = document.createElement('strong');
      const detail = document.createElement('p');
      const badge = document.createElement('span');
      name.textContent = commandLabels[command.type];
      detail.textContent = command.detail;
      detail.hidden = !command.detail;
      badge.className = `badge ${command.stage}`;
      badge.textContent = command.stage === 'syncing' ? 'Refreshing settings' :
        command.stage === 'sending' ? 'Submitting' : title(command.stage);
      copy.append(name, detail);
      row.append(copy, badge);
      return row;
    }));
  }

  function render() {
    if (!owner) return;
    const time = now();
    const shortIdCounts = new Map<string, number>();
    const boardIdCounts = new Map<string, number>();
    for (const { uid, latest } of devices.values()) {
      const shortId = uid.slice(-8);
      shortIdCounts.set(shortId, (shortIdCounts.get(shortId) ?? 0) + 1);
      const boardId = latest?.boardId;
      if (boardId) boardIdCounts.set(boardId, (boardIdCounts.get(boardId) ?? 0) + 1);
    }
    function identifier({ uid, latest }: Device) {
      const shortId = uid.slice(-8);
      const deviceId = `ID ${shortIdCounts.get(shortId) === 1 ? shortId : uid}`;
      const boardId = latest?.boardId;
      if (!boardId) return deviceId;
      return boardIdCounts.get(boardId) === 1 ? `Board ${boardId}` : `Board ${boardId} · ${deviceId}`;
    }

    for (const device of devices.values()) {
      for (const command of device.commands) {
        const { setting, appliedAt } = command;
        if (setting && appliedAt !== undefined) {
          const latest = device.latest;
          // The result and its settings snapshot arrive on separate subscriptions.
          if (latest && (latest[setting.field] === setting.value || latest.updatedAt > appliedAt)) {
            command.stage = 'applied';
            delete command.appliedAt;
            command.detail = '';
            if (selectedUid === device.uid && settingEdits[command.type] === setting.edit) {
              delete settingEdits[command.type];
            }
          } else if (time >= command.expiresAt) {
            command.stage = 'applied';
            command.detail = 'Waiting for updated controller readings.';
          }
        } else if (isBusy(command) && time >= command.expiresAt) {
          command.stage = 'unconfirmed';
          command.detail = 'No controller result received.';
        }
      }
      text(device.name, device.latest?.name || 'Controller');
      text(device.identifier, identifier(device));
      text(device.status, state(device, time)[0]);
      device.button.setAttribute('aria-current', String(device.uid === selectedUid));
    }
    const device = devices.get(selectedUid);
    const directoryReady = directoryState === 'ready';
    const pairingMessages: string[] = [];
    if (directoryReady) {
      let waiting = 0;
      for (const expiresAt of pendingPairings.values()) if (time < expiresAt) waiting++;
      if (waiting) pairingMessages.push(waiting === 1 ? 'Waiting for the controller to finish pairing.' :
        `Waiting for ${waiting} controllers to finish pairing.`);
      if (waiting < pendingPairings.size) {
        pairingMessages.push('Pairing code expired. Get a new code from the controller and try again.');
      }
    }
    banner('pairing-status', pairingMessages.join(' '));
    element('device-view').hidden = !device;
    element('empty-workspace').hidden = !!device || directoryState === 'unavailable';
    const firstController = directoryReady && devices.size === 0;
    element('pair-first').hidden = !firstController;
    element('pair-first-help').hidden = !firstController;
    element('open-pairing').hidden = !directoryReady;
    text(element('empty-heading'), !directoryReady ? 'Loading controllers…' :
      firstController ? 'Pair your first controller' : 'Select a controller');
    if (!device) return;

    const latest = device.latest;
    const [label, tone] = state(device, time);
    const badge = element('freshness');
    text(badge, label);
    badge.className = `badge ${tone}`;
    text(element('device-name'), latest?.name || 'Controller');
    text(element('device-id'), identifier(device));
    const age = latest ? Math.max(0, Math.floor((time - latest.updatedAt) / 1_000)) : 0;
    const seen = age < 2 ? 'just now' : age < 60 ? `${age}s ago` :
      `${Math.floor(age / 60)}m ago`;
    text(element('last-seen'), latest ? `Last seen ${seen}` : 'Waiting for first update');
    banner('availability', !connected ? 'The dashboard lost its Firebase connection.' :
      device.unreadable ? 'The dashboard cannot read this controller’s status.' :
      !latest ? '' : tone === 'stale' ? 'The last reading is stale. Commands resume when the controller checks in.' :
      tone !== 'online' ? 'Waiting for the controller’s command connection. You can still request Disable heat.' : '');
    text(element('temperature'), temperature(latest?.temperatureValid ? latest.temperatureC : undefined));
    text(element('temperature-note'), latest && !latest.temperatureValid ? 'Sensor reading unavailable' : '');
    text(element('target'), temperature(latest?.targetC));
    text(element('heat-mode'), latest ? latest.restorePending ? 'Heating recovery pending' :
      latest.heatEnabled ? 'Heating enabled' : 'Heating disabled' : '');
    text(element('heater'), latest ? latest.heaterOn ? 'On' : 'Off' : '—');
    element('heater').classList.toggle('on', latest?.heaterOn === true);
    text(element('heat-state'), latest ? title(latest.state) : '');
    const scale = element('temperature-scale');
    scale.hidden = !latest?.temperatureValid;
    scale.style.setProperty('--temperature', fraction(latest?.temperatureC ?? 0));
    scale.style.setProperty('--target', fraction(latest?.targetC ?? 0));

    const fault = element('fault');
    text(fault, latest?.fault === 'restart_lockout' ? 'Heating interrupted. Acknowledge once the probe is healthy and cool.'
      : latest ? title(latest.fault) : '—');
    fault.classList.toggle('active-fault', !!latest && latest.fault !== 'none');
    text(element('printer-connection'), latest ? !latest.printerConfigured ? 'Setup required' :
      latest.printerConnected ? 'Connected' : 'Disconnected' : '—');
    text(element('printer-state'), latest ? latest.printerStateKnown ? printerStates[latest.printerState] ?? 'Unknown' : 'Unknown' : '—');
    text(element('monitor-phase'), latest ? monitorPhases[latest.monitorPhase] ?? 'Unknown' : '—');
    text(element('firmware-version'), latest?.firmwareVersion || '—');
    text(element('update-phase'), latest ? updatePhases[latest.updatePhase] ?? 'Unknown' : '—');
    const heap = latest && tone !== 'stale' ? latest.heap : undefined;
    text(element('free-ram'), kibibytes(heap?.freeBytes));
    text(element('heap-low-water'), kibibytes(heap?.minimumFreeBytes));
    const progress = element<HTMLProgressElement>('update-progress');
    progress.hidden = !latest || latest.updateTotal === 0;
    progress.value = latest?.updateTotal ? Math.min(1, latest.updateReceived / latest.updateTotal) : 0;
    banner('update-result', latest ? updateResults[latest.updateResult] ?? '' : '');
    element<HTMLButtonElement>('firmware-update').hidden = latest?.canUpdate !== true;

    if (latest && !settingEdits['heat.setTarget'] && document.activeElement !== targetInput) {
      targetInput.value = String(latest.targetC);
    }
    element('high-target-warning').hidden =
      !(targetInput.valueAsNumber > HIGH_TARGET_C || (latest?.targetC ?? 0) > HIGH_TARGET_C);
    restartForm.hidden = latest?.restartC === undefined || latest.targetC <= 0;
    if (latest?.restartC !== undefined) {
      restartInput.min = String(latest.targetC - MAX_RESTART_DROP_C);
      restartInput.max = String(latest.targetC - MIN_RESTART_DROP_C);
      if (!settingEdits['heat.setRestart'] && document.activeElement !== restartInput) {
        restartInput.value = String(latest.restartC);
      }
    }
    if (latest && !settingEdits['sensor.setCalibration'] && document.activeElement !== calibrationInput) {
      calibrationInput.value = String(latest.calibrationOffsetC);
    }
    const targetButton = element('target-form').querySelector<HTMLButtonElement>('button')!;
    const restartButton = restartForm.querySelector<HTMLButtonElement>('button')!;
    const calibrationButton = element('calibration-form').querySelector<HTMLButtonElement>('button')!;
    targetButton.disabled = !canSend(device, 'heat.setTarget');
    restartButton.disabled = !canSend(device, 'heat.setRestart');
    calibrationButton.disabled = !canSend(device, 'sensor.setCalibration');
    targetInput.readOnly = targetButton.disabled;
    restartInput.readOnly = restartButton.disabled;
    calibrationInput.readOnly = calibrationButton.disabled;
    for (const button of commandButtons) button.disabled = !canSend(device, button.dataset.command as CommandType);
    renderResults(device);
  }

  function addDevice(uid: string, user: User) {
    const row = document.createElement('li');
    const button = document.createElement('button');
    const name = document.createElement('span');
    const identifier = document.createElement('span');
    const status = document.createElement('span');
    button.type = 'button';
    identifier.className = 'device-id';
    button.append(name, identifier, status);
    row.append(button);
    element('devices').append(row);
    const device = { uid, row, button, name, identifier, status, commands: [] } as Device;
    devices.set(uid, device);
    button.addEventListener('click', () => { choose(uid); render(); });
    device.unsubscribe = onValue(ref(cloud.database, `devices/${uid}/latest`), snapshot => {
      if (owner !== user || devices.get(uid) !== device) return;
      device.latest = readLatest(snapshot.val());
      device.unreadable = snapshot.exists() && !device.latest;
      render();
    }, () => {
      if (owner !== user || devices.get(uid) !== device) return;
      device.latest = undefined;
      device.unreadable = true;
      render();
    });
    device.unsubscribeDisableRevision = onValue(ref(cloud.database, `devices/${uid}/disableRevision`), snapshot => {
      if (owner !== user || devices.get(uid) !== device) return;
      const revision: unknown = snapshot.val() ?? 0;
      device.disableRevision = isDisableRevision(revision) ? revision : undefined;
      render();
    }, () => {
      if (owner !== user || devices.get(uid) !== device) return;
      device.disableRevision = undefined;
      render();
    });
  }

  function removeDevice(device: Device) {
    device.unsubscribe?.();
    device.unsubscribeDisableRevision?.();
    for (const command of device.commands) command.unsubscribe?.();
    devices.delete(device.uid);
    device.row.remove();
  }

  function setOwner(user: User | null) {
    directoryUnsubscribe?.();
    directoryUnsubscribe = undefined;
    for (const device of devices.values()) removeDevice(device);
    choose('');
    lastResultList = '';
    element('command-results').replaceChildren();
    for (const id of ['device-name', 'device-id', 'last-seen', 'freshness', 'temperature', 'temperature-note', 'target',
      'heat-mode', 'heater', 'heat-state', 'fault', 'printer-connection', 'printer-state', 'monitor-phase',
      'firmware-version', 'update-phase', 'free-ram', 'heap-low-water', 'update-result']) text(element(id), '');
    element('temperature-scale').removeAttribute('style');
    element<HTMLProgressElement>('update-progress').value = 0;
    banner('availability', '');
    pendingPairings.clear();
    directoryState = 'loading';
    owner = user;
    if (pairDialog.open) pairDialog.close();
    banner('workspace-error', '');
    element('account').hidden = !user;
    element('sign-in-view').hidden = !!user;
    element('workspace').hidden = !user;
    text(element('account-name'), user?.displayName || user?.email || '');
    if (!user) return;
    directoryUnsubscribe = onValue(ref(cloud.database, `owners/${user.uid}/devices`), snapshot => {
      if (owner !== user) return;
      directoryState = 'ready';
      const value: unknown = snapshot.val();
      const uids = isRecord(value) ? Object.keys(value).filter(uid => isUid(uid) && value[uid] === true) : [];
      const activeUids = new Set(uids);
      const firstController = devices.size === 0 && uids.length === 1;
      for (const device of devices.values()) if (!activeUids.has(device.uid)) removeDevice(device);
      for (const uid of uids) {
        pendingPairings.delete(uid);
        if (!devices.has(uid)) addDevice(uid, user);
      }
      if (firstController) choose(uids[0]);
      else if (!devices.has(selectedUid)) choose('');
      banner('workspace-error', '');
      render();
    }, () => {
      if (owner !== user) return;
      directoryState = 'unavailable';
      for (const device of devices.values()) removeDevice(device);
      choose('');
      render();
      banner('workspace-error', 'Dashboard access requires an approved email and password account.');
    });
    render();
  }

  async function send(action: CommandAction) {
    const device = devices.get(selectedUid);
    const latest = device?.latest;
    if (!device || !latest || !canSend(device, action.type)) return;
    const id = crypto.randomUUID();
    const disableRevision = device.disableRevision ?? 0;
    const field = settingFields[action.type];
    const command: TrackedCommand = {
      id, type: action.type, submittedSessionId: latest.sessionId,
      stage: 'sending', expiresAt: now() + COMMAND_TTL_MS, detail: '',
      setting: field && { field, value: action.payload[field], edit: settingEdits[action.type] },
    };
    device.commands.unshift(command);
    if (device.commands.length > 8) device.commands.pop()?.unsubscribe?.();
    const tracking = () => devices.get(device.uid) === device && device.commands.includes(command);
    command.unsubscribe = onValue(ref(cloud.database, `devices/${device.uid}/results/${id}`), snapshot => {
      if (!tracking()) return;
      const result = readResult(snapshot.val(), id);
      if (!result) return;
      command.stage = result.status;
      if (result.status === 'applied' && command.setting) {
        command.stage = 'syncing';
        command.appliedAt = result.timestamp;
      }
      command.detail = rejectionDetails[result.reason] ?? '';
      command.unsubscribe?.();
      render();
    }, () => {
      if (!tracking()) return;
      command.stage = 'unconfirmed';
      command.detail = 'Could not read the controller’s response.';
      render();
    });
    render();
    try {
      const { data } = await cloud.issue({
        ...action, deviceUid: device.uid, requestId: id, disableRevision,
        ...(isCommandSession(command.submittedSessionId) && { sessionId: command.submittedSessionId }),
      });
      if (!tracking()) return;
      if (data.id !== id || !Number.isSafeInteger(data.expiresAt)) throw new Error('Invalid command receipt');
      command.expiresAt = data.expiresAt;
      if (isCommandSession(data.sessionId)) command.acceptedSessionId = data.sessionId;
      if (command.stage === 'sending') {
        command.stage = 'pending';
        if (action.type === 'heat.disable' && !command.submittedSessionId) {
          command.detail = 'Stop requested. Waiting for the controller to confirm.';
        }
      }
    } catch (error) {
      if (!tracking() || command.stage !== 'sending') return;
      const code = error instanceof FirebaseError ? error.code : '';
      const rejected = ['functions/permission-denied', 'functions/unauthenticated', 'functions/invalid-argument',
        'functions/failed-precondition', 'functions/resource-exhausted', 'functions/already-exists'].includes(code);
      command.stage = rejected ? 'rejected' : 'unconfirmed';
      command.detail = rejected ? message(error, 'The command was rejected.') : 'Could not confirm command submission.';
      if (rejected) command.unsubscribe?.();
    } finally {
      render();
    }
  }

  element('sign-in-form').addEventListener('submit', async event => {
    event.preventDefault();
    if (signInButton.disabled) return;
    signInButton.disabled = true;
    text(element('sign-in-error'), '');
    try {
      await signInWithEmailAndPassword(cloud.auth, signInEmail.value.trim(), signInPassword.value);
    } catch (error) {
      const code = error instanceof FirebaseError ? error.code : '';
      const feedback = code === 'auth/network-request-failed' ? 'Could not reach Firebase. Try again.' :
        code === 'auth/too-many-requests' ? 'Too many sign-in attempts. Try again later.' :
          'Could not sign in with that email and password.';
      text(element('sign-in-error'), feedback);
    } finally {
      signInPassword.value = '';
      signInButton.disabled = false;
    }
  });
  element('sign-out').addEventListener('click', () => {
    void signOut(cloud.auth).catch(() => banner('workspace-error', 'Could not sign out.'));
  });
  for (const id of ['open-pairing', 'pair-first']) {
    element(id).addEventListener('click', () => {
      if (!pairDialog.open) {
        pairCode.value = '';
        text(element('pair-error'), '');
        pairDialog.showModal();
      }
    });
  }
  element('close-pairing').addEventListener('click', () => pairDialog.close());
  element('pair-form').addEventListener('submit', async event => {
    event.preventDefault();
    const currentOwner = owner;
    if (!currentOwner || pairing) return;
    const code = pairCode.value.trim();
    pairing = true;
    pairSubmit.disabled = true;
    text(element('pair-error'), '');
    const submittedAt = now();
    let checking = false;
    try {
      const { data } = await cloud.claim({ code }).catch(error => {
        if (!pairingUnconfirmed(error) || owner !== currentOwner || !pairDialog.open) throw error;
        // A successful claim is idempotent for this code and owner.
        checking = true;
        text(element('pair-error'), 'Checking whether the controller was paired…');
        return cloud.claim({ code });
      });
      if (owner !== currentOwner) return;
      for (const [uid, expiresAt] of pendingPairings) {
        if (expiresAt <= submittedAt) pendingPairings.delete(uid);
      }
      if (!devices.has(data.deviceUid)) {
        const expiresAt = data.expiresAt;
        pendingPairings.set(data.deviceUid, expiresAt !== undefined && Number.isSafeInteger(expiresAt)
          ? expiresAt : now() + PAIRING_CODE_TTL_MS);
      }
      pairDialog.close();
      render();
    } catch (error) {
      if (owner === currentOwner) {
        const feedback = checking || pairingUnconfirmed(error)
          ? 'Pairing could not be confirmed. Check your controller list or enter the current code again.'
          : message(error, 'Could not pair the controller.');
        text(element('pair-error'), feedback);
      }
    } finally {
      pairing = false;
      pairSubmit.disabled = false;
    }
  });
  for (const button of commandButtons) {
    button.addEventListener('click', () => {
      const action = readCommandAction(button.dataset.command, {});
      if (action) void send(action);
    });
  }
  targetInput.addEventListener('input', () => {
    settingEdits['heat.setTarget'] = Symbol();
    render();
  });
  restartInput.addEventListener('input', () => { settingEdits['heat.setRestart'] = Symbol(); });
  calibrationInput.addEventListener('input', () => { settingEdits['sensor.setCalibration'] = Symbol(); });
  element('target-form').addEventListener('submit', event => {
    event.preventDefault();
    const action = readCommandAction('heat.setTarget', { targetC: targetInput.valueAsNumber });
    if (action) void send(action);
  });
  restartForm.addEventListener('submit', event => {
    event.preventDefault();
    const action = readCommandAction('heat.setRestart', { restartC: restartInput.valueAsNumber });
    if (action) void send(action);
  });
  element('calibration-form').addEventListener('submit', event => {
    event.preventDefault();
    const action = readCommandAction('sensor.setCalibration', { calibrationOffsetC: calibrationInput.valueAsNumber });
    if (action) void send(action);
  });
  onValue(ref(cloud.database, '.info/connected'), snapshot => { connected = snapshot.val() === true; render(); });
  onValue(ref(cloud.database, '.info/serverTimeOffset'), snapshot => {
    timeOffset = typeof snapshot.val() === 'number' ? snapshot.val() : 0;
    render();
  });
  onAuthStateChanged(cloud.auth, user => {
    text(element('sign-in-error'), '');
    signInButton.disabled = false;
    setOwner(user);
  });
  setInterval(render, 1_000);
}

try {
  mount(connectCloud());
} catch (error) {
  text(element('sign-in-error'), message(error, error instanceof Error ? error.message : 'Could not start Firebase.'));
}
