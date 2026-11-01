'use strict';

(function () {
  var HIGH_TARGET_C = 60;

  var el = {
    connection: document.getElementById('connection-state'),
    current: document.getElementById('current-temperature'),
    raw: document.getElementById('raw-temperature'),
    targetReadout: document.getElementById('target-readout'),
    stateBadge: document.getElementById('state-badge'),
    heaterBadge: document.getElementById('heater-badge'),
    fanBadge: document.getElementById('fan-badge'),
    slider: document.getElementById('target-slider'),
    number: document.getElementById('target-temperature'),
    hint: document.getElementById('target-hint'),
    warning: document.getElementById('high-target-warning'),
    enable: document.getElementById('enable-button'),
    disable: document.getElementById('disable-button'),
    fanToggle: document.getElementById('fan-toggle-button'),
    faultPanel: document.getElementById('fault-panel'),
    faultName: document.getElementById('fault-name'),
    acknowledge: document.getElementById('acknowledge-button'),
    ip: document.getElementById('ip-address'),
    rssi: document.getElementById('wifi-rssi'),
    uptime: document.getElementById('uptime'),
    validCount: document.getElementById('consecutive-valid'),
    message: document.getElementById('status-message'),
    firmwareVersion: document.getElementById('firmware-version'),
    firmwareUpdate: document.getElementById('firmware-update-button'),
    firmwareProgress: document.getElementById('firmware-progress'),
    firmwareMessage: document.getElementById('firmware-message')
  };

  var STATE_LABELS = {
    boot: 'Boot',
    idle: 'Idle',
    heating: 'Heating',
    holding: 'Holding',
    fault: 'Fault'
  };

  var FAULT_LABELS = {
    none: 'None',
    sensor_open: 'Sensor open circuit',
    sensor_short: 'Sensor short circuit',
    sensor_range: 'Sensor out of range',
    sensor_invalid: 'Sensor reading invalid',
    sensor_stale: 'Sensor data stale',
    overtemperature: 'Over-temperature',
    nvs: 'Stored settings recovered',
    output_init: 'Relay output initialisation failed'
  };

  var busy = false;
  var editingTarget = false;
  var lastStatus = null;
  var firmware = null;
  var firstFirmwareVersion = null;
  var firmwareReloaded = false;
  var firmwareTimer = null;
  var firmwareFetching = false;
  var wifiChanging = false;
  var updateRequested = false;
  var updateRequestUncertain = false;

  function firmwareBusy() {
    return firmware && ['checking', 'cooling', 'downloading', 'verifying', 'rebooting', 'trial'].indexOf(firmware.phase) >= 0;
  }

  function maintenanceActive() {
    return firmware && (firmware.maintenance || firmware.trial_pending);
  }

  function setMessage(text, tone) {
    el.message.textContent = text || '';
    if (tone) {
      el.message.setAttribute('data-tone', tone);
    } else {
      el.message.removeAttribute('data-tone');
    }
  }

  function setConnection(mode, label) {
    el.connection.setAttribute('data-connection', mode);
    el.connection.textContent = label;
  }

  function formatTemperature(value) {
    return typeof value === 'number' && isFinite(value) ? value.toFixed(1) : '--';
  }

  function formatUptime(milliseconds) {
    if (typeof milliseconds !== 'number' || !isFinite(milliseconds)) {
      return '--';
    }
    var total = Math.floor(milliseconds / 1000);
    var hours = Math.floor(total / 3600);
    var minutes = Math.floor((total % 3600) / 60);
    var seconds = total % 60;
    return hours + 'h ' + minutes + 'm ' + seconds + 's';
  }

  function updateWarning(target) {
    el.warning.hidden = !(target > HIGH_TARGET_C);
    el.hint.textContent = target === 0
      ? 'Target zero: heating is disabled.'
      : 'Zero disables heating. Steps of 0.5 °C.';
  }

  function setTargetInputs(target) {
    if (!editingTarget) {
      el.slider.value = String(target);
      el.number.value = String(target);
    }
    updateWarning(target);
  }

  function setControlsBusy(value) {
    busy = value;
    render(lastStatus);
  }

  function render(status) {
    if (!status) {
      return;
    }
    lastStatus = status;

    el.current.textContent = formatTemperature(status.filtered_c);
    el.raw.textContent = formatTemperature(status.raw_c);
    el.targetReadout.textContent = formatTemperature(status.target_c);

    var state = status.state || 'boot';
    el.stateBadge.setAttribute('data-state', state);
    el.stateBadge.textContent = STATE_LABELS[state] || state;

    /* Relay state comes from the server, never inferred from temperature. */
    el.heaterBadge.setAttribute('data-on', status.heater_on ? 'true' : 'false');
    el.heaterBadge.textContent = status.heater_on ? 'Heater on' : 'Heater off';
    el.fanBadge.setAttribute('data-on', status.fan_on ? 'true' : 'false');
    el.fanBadge.textContent = status.fan_on ? 'Fan on' : 'Fan off';

    setTargetInputs(status.target_c);

    var faulted = state === 'fault';
    el.faultPanel.hidden = !faulted;
    el.faultName.textContent = FAULT_LABELS[status.fault] || status.fault || '--';
    var locked = busy || maintenanceActive();
    el.acknowledge.disabled = locked || !faulted;

    var readyToEnable = !faulted &&
      status.valid === true &&
      status.consecutive_valid >= 3 &&
      status.target_c > 0;
    el.enable.disabled = locked || status.enabled === true || !readyToEnable;
    el.disable.disabled = busy || (status.enabled !== true && !maintenanceActive());
    var manualFanOn = status.manual_fan_on === true;
    el.fanToggle.disabled = locked;
    el.fanToggle.setAttribute('aria-pressed', manualFanOn ? 'true' : 'false');
    el.fanToggle.textContent = manualFanOn
      ? 'Stop manual fan'
      : 'Run fan manually';
    el.slider.disabled = locked;
    el.number.disabled = locked;

    el.ip.textContent = status.ip_addr || '--';
    el.rssi.textContent = status.wifi_rssi ? status.wifi_rssi + ' dBm' : '--';
    el.uptime.textContent = formatUptime(status.uptime_ms);
    el.validCount.textContent = typeof status.consecutive_valid === 'number'
      ? String(status.consecutive_valid)
      : '--';
  }

  function describeFailure(response, body) {
    if (body && typeof body.message === 'string') {
      return body.message;
    }
    return 'Request failed with status ' + response.status + '.';
  }

  function send(path, options, successMessage) {
    setControlsBusy(true);
    setMessage('Working…');

    return fetch(path, options).then(function (response) {
      if (response.ok) {
        setMessage(successMessage || '');
        return null;
      }
      return response.json().catch(function () {
        return null;
      }).then(function (body) {
        setMessage(describeFailure(response, body), 'error');
        return null;
      });
    }).catch(function () {
      setMessage('The controller could not be reached.', 'error');
    }).then(function () {
      setControlsBusy(false);
      return refreshStatus();
    });
  }

  function refreshStatus() {
    return fetch('/api/status').then(function (response) {
      if (!response.ok) {
        throw new Error('status');
      }
      return response.json();
    }).then(function (status) {
      render(status);
      setConnection('live', 'Connected');
    }).catch(function () {
      setConnection('lost', 'Disconnected');
    });
  }

  function postCommand(path, message) {
    return send(path, { method: 'POST' }, message);
  }

  function commitTarget(value) {
    var target = Math.round(value * 2) / 2;
    if (!isFinite(target) || target < 0 || target > 80) {
      setMessage('Target must be between 0 and 80 °C.', 'error');
      return;
    }
    send('/api/target', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ target_c: target })
    }, 'Target set to ' + target.toFixed(1) + ' °C.');
  }

  el.slider.addEventListener('input', function () {
    editingTarget = true;
    el.number.value = el.slider.value;
    updateWarning(parseFloat(el.slider.value));
  });

  el.slider.addEventListener('change', function () {
    editingTarget = false;
    commitTarget(parseFloat(el.slider.value));
  });

  el.number.addEventListener('input', function () {
    editingTarget = true;
    var value = parseFloat(el.number.value);
    if (isFinite(value)) {
      el.slider.value = String(value);
      updateWarning(value);
    }
  });

  el.number.addEventListener('change', function () {
    editingTarget = false;
    commitTarget(parseFloat(el.number.value));
  });

  el.enable.addEventListener('click', function () {
    postCommand('/api/control/enable', 'Heating enabled.');
  });

  el.disable.addEventListener('click', function () {
    postCommand('/api/control/disable', 'Heating disabled.');
  });

  el.fanToggle.addEventListener('click', function () {
    var manualFanOn = lastStatus && lastStatus.manual_fan_on === true;
    postCommand(manualFanOn
      ? '/api/control/fan/disable'
      : '/api/control/fan/enable',
    manualFanOn ? 'Manual fan stopped.' : 'Manual fan started.');
  });

  el.acknowledge.addEventListener('click', function () {
    postCommand('/api/fault/acknowledge', 'Fault acknowledged.');
  });

  function connectEvents() {
    var source = new EventSource('/api/events');

    source.addEventListener('open', function () {
      setConnection('live', 'Connected');
    });

    source.addEventListener('status', function (event) {
      try {
        render(JSON.parse(event.data));
        setConnection('live', 'Connected');
      } catch (error) {
        setConnection('lost', 'Disconnected');
      }
    });

    /* EventSource reconnects on its own; a status poll fills the gap and
     * keeps the last good measurement on screen. */
    source.addEventListener('error', function () {
      setConnection('lost', 'Disconnected');
      refreshStatus();
    });
  }

  var FIRMWARE_MESSAGES = {
    none: 'Ready to check for updates.',
    up_to_date: 'Already up to date.',
    no_release: 'Firmware releases are not available yet.',
    installed: 'Firmware confirmed. Heating remains disabled.',
    check_failed: 'Could not check for updates. Check the controller’s internet connection.',
    unsafe: 'Outputs could not be confirmed safe. Heating is locked off.',
    download_failed: 'Firmware download failed. Heating remains disabled.',
    invalid_image: 'Firmware verification failed. The previous firmware is retained.',
    storage_failed: 'Device settings could not be saved. Check the controller.',
    rolled_back: 'Update failed; rolled back to the previous firmware.',
    trial_failed: 'The new firmware did not pass its startup checks.',
    setup_required: 'Initial firmware setup needs attention. Heating is disabled.',
    interrupted: 'Update was interrupted. The previous firmware is running.'
  };
  var FIRMWARE_PHASES = {
    checking: 'Checking for a new release…', cooling: 'Heating stopped. Finishing the fan cooldown…',
    downloading: 'Downloading firmware…', verifying: 'Verifying firmware…',
    rebooting: 'Restarting the controller. Waiting to reconnect…', trial: 'Checking firmware after restart…'
  };
  function renderFirmware() {
    el.firmwareUpdate.disabled = !firmware || !firmware.csrf_token || wifiChanging || updateRequested || firmwareBusy() || maintenanceActive();
    if (!firmware) return;
    el.firmwareVersion.textContent = firmware.installed || '--';
    var message = FIRMWARE_PHASES[firmware.phase] || FIRMWARE_MESSAGES[firmware.result] || 'Firmware update failed.';
    if (updateRequestUncertain && firmware.phase === 'idle' && firmware.result === 'none') {
      message = 'The update request was not confirmed. Check status before trying again.';
    }
    el.firmwareProgress.hidden = firmware.phase !== 'downloading' && firmware.phase !== 'verifying';
    if (firmware.total > 0) {
      var percent = Math.min(100, Math.floor(firmware.received * 100 / firmware.total));
      el.firmwareProgress.value = percent;
      if (firmware.phase === 'downloading') message += ' ' + percent + '%';
    } else el.firmwareProgress.removeAttribute('value');
    el.firmwareMessage.textContent = message;
    if (firmware.phase === 'failed' || updateRequestUncertain) el.firmwareMessage.setAttribute('data-tone', 'error');
    else el.firmwareMessage.removeAttribute('data-tone');
    render(lastStatus);
  }
  function scheduleFirmwarePoll() {
    if (firmwareTimer !== null) clearTimeout(firmwareTimer);
    firmwareTimer = null;
    if (!document.hidden) firmwareTimer = setTimeout(refreshFirmware, firmwareBusy() || updateRequested ? 2000 : 15000);
  }
  function refreshFirmware() {
    if (firmwareFetching) return Promise.resolve();
    firmwareFetching = true;
    return fetch('/api/firmware', {cache: 'no-store'}).then(function (response) {
      if (!response.ok) throw new Error('firmware status');
      return response.json();
    }).then(function (snapshot) {
      firmware = snapshot;
      if (firstFirmwareVersion === null) firstFirmwareVersion = snapshot.installed;
      if (snapshot.phase !== 'idle' || snapshot.result !== 'none') updateRequestUncertain = false;
      renderFirmware();
      if (snapshot.installed !== firstFirmwareVersion && !firmwareReloaded) {
        firmwareReloaded = true;
        window.location.reload();
      }
    }).catch(function () {
      el.firmwareMessage.textContent = firmwareBusy()
        ? 'Waiting to reconnect to the controller…'
        : 'Firmware status is unavailable. Reconnecting…';
    }).then(function () {
      firmwareFetching = false;
      scheduleFirmwarePoll();
    });
  }
  el.firmwareUpdate.addEventListener('click', function () {
    if (!firmware || !firmware.csrf_token || wifiChanging || updateRequested || firmwareBusy() || maintenanceActive()) return;
    updateRequested = true;
    updateRequestUncertain = false;
    firmware.phase = 'checking';
    renderFirmware();
    fetch('/api/firmware/update', {
      method: 'POST', headers: {'Content-Type': 'application/json', 'X-P1S-CSRF': firmware.csrf_token}, body: '{}'
    }).then(function (response) {
      if (!response.ok) throw new Error('update request');
    }).catch(function () {
      updateRequestUncertain = true;
    }).then(function () {
      updateRequested = false;
      return refreshFirmware();
    });
  });
  document.addEventListener('visibilitychange', function () {
    if (document.hidden) { if (firmwareTimer !== null) clearTimeout(firmwareTimer); firmwareTimer = null; }
    else refreshFirmware();
  });

  refreshStatus();
  if (typeof window.addEventListener === 'function') {
    window.addEventListener('p1s-wifi-changing', function (event) {
      wifiChanging = !!event.detail; renderFirmware();
    });
  }
  connectEvents();
  refreshFirmware();
})();
