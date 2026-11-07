(function () {
  'use strict';
  var el = {
    form: document.getElementById('printer-form'), address: document.getElementById('printer-ipv4'),
    serial: document.getElementById('printer-serial'), code: document.getElementById('printer-access-code'),
    enabled: document.getElementById('printer-enabled'),
    save: document.getElementById('printer-save'), forget: document.getElementById('printer-forget'),
    message: document.getElementById('printer-message'), connection: document.getElementById('printer-connection'),
    state: document.getElementById('printer-state'), settings: document.getElementById('printer-settings'),
    monitor: document.getElementById('monitor-button'), monitorStatus: document.getElementById('monitor-status')
  };
  var status = null, online = false, fetching = false, timer = null, generation = 0;
  var dirty = false, saving = false, savePending = null, configUncertain = false;
  var sending = false, error = '';
  var holds = {
    paused: 'Heater paused: printer is paused.',
    disconnected: 'Heater paused: printer connection lost.',
    unknown: 'Heater paused: waiting for printer status.'
  };
  var endings = {
    finished: 'Print finished: heating turned off.',
    failed: 'Print failed or was cancelled: heating turned off.',
    idle: 'Printer went idle: heating turned off.',
    no_print: 'No print started within 60 minutes: heating turned off.',
    stopped: 'Monitoring stopped. Heating continues.',
    heating_off: 'Monitoring ended because heating was turned off.',
    settings: 'Printer settings changed: heating turned off.'
  };
  var reasons = {
    ready: 'Press Monitor P1S to turn heating off automatically when the print ends.',
    heating_off: 'Enable heating to use Monitor P1S.',
    disconnected: 'Printer not connected.',
    state_unknown: 'Waiting for printer status.',
    configuration_required: 'Set up the printer connection below.',
    printer_disabled: 'Printer monitoring is disabled in printer settings.'
  };
  el.code.value = '';
  function text(node, value) { if (node.textContent !== value) node.textContent = value; }
  function phase() { return status && status.monitor ? status.monitor.phase : 'off'; }
  function monitorText() {
    if (!status) return '';
    if (!online) return 'Controller status is unavailable.';
    var m = status.monitor;
    if (m.phase !== 'off') {
      if (holds[m.hold]) return holds[m.hold];
      if (m.phase === 'waiting') return 'Waiting for the print to start (stops in ' + Math.ceil(m.wait_remaining_ms / 60000) + ' min).';
      return 'Monitoring the print. Heating turns off when it finishes.';
    }
    return endings[m.last_end] || reasons[status.reason] || '';
  }
  function render() {
    var configuring = saving || !!savePending || !!(status && status.applying);
    el.monitor.disabled = !online || !status || !status.csrf_token || sending || (phase() === 'off' && !status.can_monitor);
    el.monitor.setAttribute('aria-pressed', String(phase() !== 'off'));
    text(el.monitorStatus, monitorText());
    el.save.disabled = !online || !status || !status.csrf_token || configuring;
    el.forget.disabled = el.save.disabled;
    var message = !status ? 'Reading printer status…' : !online ? 'Controller status is unavailable. Check the printer directly.' : '';
    if (saving) message = 'Saving printer settings…';
    else if (savePending || (status && status.applying)) message = 'Saving settings and preparing the printer connection…';
    else if (configUncertain) message = 'The settings change was not confirmed. Review the connection before saving again.';
    else if (error) message = error;
    text(el.message, message);
    if (error || configUncertain || (status && !online)) el.message.setAttribute('data-tone', 'error');
    else el.message.removeAttribute('data-tone');
    if (status) {
      text(el.connection, !online ? 'Status unavailable' : !status.connected ? 'Disconnected' : 'Connected');
      text(el.state, status.printer_state || '—');
    }
  }
  function schedule() {
    if (timer !== null) clearTimeout(timer);
    timer = null;
    if (!document.hidden) timer = setTimeout(refresh, 1000);
  }
  function refresh() {
    if (fetching || document.hidden) return Promise.resolve();
    fetching = true;
    var requestedGeneration = generation;
    return fetch('/api/printer', {cache: 'no-store'}).then(function (response) {
      if (!response.ok) throw new Error('printer status');
      return response.json();
    }).then(function (snapshot) {
      if (requestedGeneration !== generation) return;
      if (!snapshot || !snapshot.monitor || typeof snapshot.csrf_token !== 'string') throw new Error('printer status');
      if (!status) el.settings.open = !snapshot.configured;
      status = snapshot; online = snapshot.reason !== 'unavailable';
      if (savePending && !snapshot.applying) {
        var matches = snapshot.ipv4 === savePending.ipv4 && snapshot.serial === savePending.serial &&
          snapshot.enabled === savePending.enabled;
        if (snapshot.config_result === 'saved' && matches && !configUncertain) {
          if (sameDraft(savePending)) { dirty = false; el.code.value = ''; }
        } else if (snapshot.config_result === 'save_failed') error = 'Printer settings could not be saved. Your draft is kept.';
        else if (!matches && !configUncertain) error = 'Saved settings changed in another request. Your draft is kept; review the connection.';
        savePending = null;
      }
      if (!dirty && !saving && !savePending) {
        el.address.value = snapshot.ipv4 || ''; el.serial.value = snapshot.serial || '';
        el.enabled.checked = !snapshot.configured || !!snapshot.enabled;
      }
      render();
    }).catch(function () {
      if (requestedGeneration === generation) { online = false; render(); }
    }).then(function () { fetching = false; schedule(); });
  }
  function draft() { return {enabled: el.enabled.checked, ipv4: el.address.value, serial: el.serial.value}; }
  function sameDraft(value) {
    var current = draft();
    return current.enabled === value.enabled && current.ipv4 === value.ipv4 && current.serial === value.serial &&
      el.code.value === (value.access_code || '');
  }
  function mutation(path, method, body) {
    return fetch(path, {method: method, headers: {'Content-Type': 'application/json', 'X-P1S-CSRF': status.csrf_token},
      body: JSON.stringify(body)}).then(function (response) {
      if (!response.ok) { var failure = new Error('request rejected'); failure.status = response.status; throw failure; }
    });
  }
  function edited() { dirty = true; error = ''; configUncertain = false; render(); }
  [el.address, el.serial, el.code].forEach(function (node) { node.addEventListener('input', edited); });
  el.enabled.addEventListener('change', edited);
  el.form.addEventListener('submit', function (event) {
    event.preventDefault(); if (el.save.disabled) return;
    var value = draft();
    if (el.code.value) value.access_code = el.code.value;
    if (!value.ipv4 || !/^[A-Za-z0-9]{1,32}$/.test(value.serial) ||
        (!value.access_code && (!status.access_code_set || value.serial !== status.serial)) ||
        (value.access_code && new TextEncoder().encode(value.access_code).length > 64)) {
      error = 'Enter a printer address, serial number and access code. A saved code can be kept only for the same serial.'; render(); return;
    }
    ++generation; saving = true; savePending = value; configUncertain = false; error = ''; render();
    mutation('/api/printer/config', 'PUT', value).catch(function (failure) {
      if (failure.status && failure.status !== 503) { savePending = null; error = failure.status === 409 ? 'Another operation is active. Your draft is kept.' : 'Printer settings were rejected. Review the details.'; }
      else configUncertain = true;
    }).then(function () { saving = false; render(); return refresh(); });
  });
  el.forget.addEventListener('click', function () {
    if (el.forget.disabled) return;
    var value = draft();
    if (el.code.value) value.access_code = el.code.value;
    ++generation; saving = true; error = ''; configUncertain = false; render();
    mutation('/api/printer/config', 'DELETE', {}).then(function () {
      if (sameDraft(value)) { dirty = false; el.code.value = ''; }
    }).catch(function (failure) {
      if (failure.status && failure.status !== 503) error = 'Printer settings could not be forgotten. Another operation may be active.';
      else configUncertain = true;
    }).then(function () { saving = false; render(); return refresh(); });
  });
  el.monitor.addEventListener('click', function () {
    if (el.monitor.disabled) return;
    var stopping = phase() !== 'off';
    ++generation; sending = true; error = ''; render();
    mutation(stopping ? '/api/printer/monitor/stop' : '/api/printer/monitor', 'POST', {}).catch(function (failure) {
      error = failure.status === 409 ? 'Monitor P1S is not available right now.'
        : stopping ? 'Stopping Monitor P1S was not confirmed. Check the controller status.'
        : 'Monitor P1S was not confirmed. Check the controller status.';
    }).then(function () { sending = false; render(); return refresh(); });
  });
  document.addEventListener('visibilitychange', function () {
    ++generation;
    if (document.hidden) { if (timer !== null) clearTimeout(timer); timer = null; }
    else refresh();
  });
  render(); refresh();
})();
