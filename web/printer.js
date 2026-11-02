(function () {
  'use strict';
  var el = {
    form: document.getElementById('printer-form'), address: document.getElementById('printer-ipv4'),
    serial: document.getElementById('printer-serial'), code: document.getElementById('printer-access-code'),
    ntp: document.getElementById('printer-ntp'), enabled: document.getElementById('printer-enabled'),
    save: document.getElementById('printer-save'), forget: document.getElementById('printer-forget'),
    confirm: document.getElementById('printer-confirm'), preheat: document.getElementById('printer-preheat'),
    stop: document.getElementById('printer-stop'), message: document.getElementById('printer-message'),
    connection: document.getElementById('printer-connection'), job: document.getElementById('printer-job'),
    progress: document.getElementById('printer-progress'), settings: document.getElementById('printer-settings')
  };
  var status = null, online = false, fetching = false, timer = null, generation = 0;
  var dirty = false, saving = false, savePending = null, configUncertain = false;
  var armSending = false, armPending = false, armUncertain = false, stopping = false, stopUncertain = false;
  var armSequence = 0, armBaseId = '', blockedToken = '', lastToken = '', error = '';
  el.confirm.checked = false;
  el.code.value = '';
  function text(node, value) { if (node.textContent !== value) node.textContent = value; }
  function cycleActive() { return status && status.cycle && ['preheating', 'resume_pending', 'printing'].indexOf(status.cycle.state) >= 0; }
  function active() { return !!(cycleActive() || armSending || armPending || armUncertain || stopping || stopUncertain); }
  function reason() {
    if (!status) return 'Reading printer status…';
    var cycle = status.cycle;
    if (cycle.state === 'preheating') return 'Preheating to ' + cycle.target_c + ' °C. Waiting for a stable chamber temperature.';
    if (cycle.state === 'resume_pending') return 'Waiting for the one Resume attempt and printer confirmation.';
    if (cycle.state === 'printing') return 'Printing. Chamber control remains active until the printer reports finish.';
    if (cycle.outcome === 'completed') return 'Print finished. Chamber control ended.';
    var outcomes = {
      later_pause: 'Automatic chamber control ended at a later printer pause. A manual printer resume does not restart chamber control.',
      operator_stop: 'Chamber control stopped. This does not cancel the print.',
      resume_uncertain: 'Resume result is uncertain. Chamber control ended. Check the printer directly; no retry will be sent.',
      resume_rejected: 'Resume was not accepted. Chamber control ended. Check the printer directly.',
      link_lost: 'Printer connection was lost. Chamber control ended.',
      telemetry_stale: 'Printer status became stale. Chamber control ended.',
      identity_changed: 'The printer job changed. Chamber control ended.',
      heat_fault: 'Chamber control ended because heating needs attention.',
      lease_expired: 'Chamber control lost heat ownership and ended.',
      printer_error: 'The printer reported an error. Chamber control ended.',
      cancelled: 'The printer job stopped or changed. Chamber control ended.',
      preheat_timeout: 'The preheat time limit was reached. Chamber control ended.'
    };
    if (outcomes[cycle.outcome]) return outcomes[cycle.outcome];
    if (!status.configured) return 'Enter your printer connection details below.';
    if (!status.enabled) return 'Printer monitoring is disabled in connection settings.';
    if (!status.connected) return 'Connecting to the printer. Check its LAN address, access code, network mode and controller clock.';
    if (!status.fresh || !status.baseline_valid) return 'Waiting for fresh, complete printer status.';
    if (status.reason === 'qualification_required') return 'Monitoring is available. Automatic Resume is disabled pending printer compatibility checks.';
    if (status.can_arm) return 'Initial pause is eligible. Confirm the startup pause, then select Preheat & Resume.';
    return 'Waiting for an eligible initial pause and healthy chamber. Prepare and submit the job from your PC.';
  }
  function render() {
    var owned = active(), configuring = saving || !!savePending || !!(status && status.applying);
    el.preheat.textContent = 'Preheat & Resume';
    el.stop.textContent = 'Stop chamber control';
    el.preheat.disabled = !online || !status || !status.csrf_token || !status.can_arm ||
      !status.connected || !status.fresh || !status.baseline_valid || !status.arm_token ||
      status.arm_token === blockedToken || !el.confirm.checked || owned || configuring || stopping;
    el.confirm.disabled = !online || !status || !status.can_arm || owned || configuring;
    el.stop.disabled = !status || !status.csrf_token || stopping;
    el.save.disabled = !online || !status || !status.csrf_token || owned || configuring;
    el.forget.disabled = el.save.disabled;
    window.dispatchEvent(new CustomEvent('p1s-printer-control', {detail: {active: owned, known: online}}));
    var message = !online && status ? 'Controller status is unavailable. Check the printer directly. Stop chamber control remains available.' : reason();
    if (armSending) message = 'Submitting the preheat request…';
    else if (armUncertain) message = 'Preheat admission was not confirmed. Check printer and chamber status; this request will not be repeated.';
    else if (armPending) message = 'Preheat request accepted. Waiting for controller status.';
    else if (stopping) message = 'Stopping chamber control…';
    else if (saving) message = 'Saving printer settings…';
    else if (savePending || (status && status.applying)) message = 'Saving settings and preparing the printer connection…';
    else if (configUncertain) message = 'The settings change was not confirmed. Review the connection before saving again.';
    else if (error) message = error;
    text(el.message, message);
    if (error || armUncertain || configUncertain || !online) el.message.setAttribute('data-tone', 'error');
    else el.message.removeAttribute('data-tone');
    if (status) {
      text(el.connection, !online ? 'Status unavailable' : !status.connected ? 'Disconnected' : !status.fresh ? 'Status stale' : 'Connected · ' + status.printer_state);
      text(el.job, status.job_label || 'No job observed');
      text(el.progress, typeof status.progress === 'number' ? status.progress + '%' +
        (typeof status.remaining_minutes === 'number' ? ' · ' + status.remaining_minutes + ' min remaining' : '') : '—');
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
      if (!snapshot || !snapshot.cycle || typeof snapshot.csrf_token !== 'string') throw new Error('printer status');
      if (snapshot.arm_token !== lastToken) { el.confirm.checked = false; lastToken = snapshot.arm_token; }
      if (!status) el.settings.open = !snapshot.configured;
      status = snapshot; online = snapshot.reason !== 'unavailable';
      if ((armPending || armUncertain) && (cycleActive() || String(snapshot.cycle.id) !== armBaseId)) {
        armPending = armUncertain = false; error = '';
      }
      if (savePending && !snapshot.applying) {
        var matches = snapshot.ipv4 === savePending.ipv4 && snapshot.serial === savePending.serial &&
          snapshot.ntp_host === savePending.ntp_host && snapshot.enabled === savePending.enabled;
        if (snapshot.config_result === 'saved' && matches && !configUncertain) {
          if (sameDraft(savePending)) { dirty = false; el.code.value = ''; }
        } else if (snapshot.config_result === 'save_failed') error = 'Printer settings could not be saved. Your draft is kept.';
        else if (!matches && !configUncertain) error = 'Saved settings changed in another request. Your draft is kept; review the connection.';
        savePending = null;
      }
      if (!dirty && !saving && !savePending) {
        el.address.value = snapshot.ipv4 || ''; el.serial.value = snapshot.serial || '';
        el.ntp.value = snapshot.ntp_host || 'pool.ntp.org'; el.enabled.checked = !snapshot.configured || !!snapshot.enabled;
      }
      render();
    }).catch(function () {
      if (requestedGeneration === generation) { online = false; render(); }
    }).then(function () { fetching = false; schedule(); });
  }
  function draft() { return {enabled: el.enabled.checked, ipv4: el.address.value, serial: el.serial.value, ntp_host: el.ntp.value || 'pool.ntp.org'}; }
  function sameDraft(value) {
    var current = draft();
    return current.enabled === value.enabled && current.ipv4 === value.ipv4 && current.serial === value.serial &&
      current.ntp_host === value.ntp_host && el.code.value === (value.access_code || '');
  }
  function mutation(path, method, body) {
    return fetch(path, {method: method, headers: {'Content-Type': 'application/json', 'X-P1S-CSRF': status.csrf_token},
      body: JSON.stringify(body)}).then(function (response) {
      if (!response.ok) { var failure = new Error('request rejected'); failure.status = response.status; throw failure; }
    });
  }
  function edited() { dirty = true; error = ''; configUncertain = false; render(); }
  [el.address, el.serial, el.code, el.ntp].forEach(function (node) { node.addEventListener('input', edited); });
  el.enabled.addEventListener('change', edited);
  el.confirm.addEventListener('change', render);
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
      el.confirm.checked = false;
    }).catch(function (failure) {
      if (failure.status && failure.status !== 503) error = 'Printer settings could not be forgotten. Another operation may be active.';
      else configUncertain = true;
    }).then(function () { saving = false; render(); return refresh(); });
  });
  el.preheat.addEventListener('click', function () {
    if (el.preheat.disabled) return;
    var token = status.arm_token, request = ++armSequence;
    armBaseId = String(status.cycle.id); ++generation; armSending = true; armPending = true;
    armUncertain = false; blockedToken = token; error = ''; el.confirm.checked = false; render();
    mutation('/api/printer/cycle', 'POST', {arm_token: token, initial_pause_confirmed: true}).catch(function (failure) {
      if (request !== armSequence) return;
      if (failure.status && failure.status !== 503) {
        armPending = false; error = failure.status === 409 ? 'The offered pause changed or another browser already armed it. Review printer status.' : 'Preheat request was rejected. Review printer status.';
      } else { armPending = false; armUncertain = true; }
    }).then(function () { if (request === armSequence) armSending = false; render(); return refresh(); });
  });
  el.stop.addEventListener('click', function () {
    if (el.stop.disabled) return;
    ++generation; ++armSequence; stopping = true; armSending = armPending = armUncertain = false; error = ''; render();
    mutation('/api/printer/cycle/stop', 'POST', {}).then(function () { stopUncertain = false; }).catch(function () {
      stopUncertain = true;
      error = 'Stop was not confirmed. Use the local Disable control or check the controller directly.';
    }).then(function () { stopping = false; render(); return refresh(); });
  });
  document.addEventListener('visibilitychange', function () {
    ++generation;
    if (document.hidden) { if (timer !== null) clearTimeout(timer); timer = null; }
    else refresh();
  });
  render(); refresh();
})();
