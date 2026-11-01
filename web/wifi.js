(function () {
  'use strict';
  var el = {
    form: document.getElementById('wifi-form'), ssid: document.getElementById('wifi-ssid'),
    password: document.getElementById('wifi-password'), save: document.getElementById('wifi-save-button'),
    message: document.getElementById('wifi-message'), ap: document.getElementById('wifi-hotspot'),
    link: document.getElementById('wifi-station-link'), discovery: document.getElementById('wifi-discovery-link')
  };
  var status = null, dirty = false, sending = false, waiting = null, uncertain = false;
  var error = '', online = false, fetching = false, timer = null, generation = 0;
  var recovery = 'Join your new Wi-Fi and open http://p1s-heat.local/. If connection failed, join P1S-Heat-Setup and open http://192.168.4.1. The setup hotspot may have closed after a successful connection.';
  var errors = {
    setup_required: 'Enter your Wi-Fi details and select Save & Connect.',
    no_network: 'The saved network was not found. Check its name and 2.4 GHz availability.',
    auth_failed: 'The network rejected the connection. Check the visible password and save again.',
    save_failed: 'Could not save Wi-Fi settings. The previous settings are retained. Try again.',
    ap_failed: 'The setup hotspot could not start. Settings were not changed. Try again.',
    driver_failed: 'The Wi-Fi connection failed. Retrying; you can correct the details below.',
    storage_failed: 'Wi-Fi storage could not be read. Setup access remains available; saving is unavailable.'
  };
  function sameFields(pair) { return el.ssid.value === pair.ssid && el.password.value === pair.password; }
  function render() {
    var busy = sending || !!waiting || !!(status && status.applying);
    el.save.disabled = !online || !status || !status.csrf_token || !status.can_save || busy;
    el.save.textContent = busy ? 'Connecting…' : 'Save & Connect';
    el.discovery.href = 'http://p1s-heat.local/';
    el.discovery.hidden = false;
    window.dispatchEvent(new CustomEvent('p1s-wifi-changing', {detail: busy}));
    var message = 'Reading Wi-Fi settings…', bad = false;
    if (sending) message = 'Saving Wi-Fi settings. ' + recovery;
    else if (!online && status) { message = 'Controller unreachable. ' + recovery; bad = true; }
    else if (waiting) message = (uncertain ? 'Save was not confirmed. Checking status. ' : 'Connecting with the saved details. ') + recovery;
    else if (error) { message = error + ' ' + recovery; bad = true; }
    else if (status) {
      if (status.connected) message = 'Connected to your Wi-Fi.' + (status.ap_active
        ? ' Rejoin that network and open the controller address below. The setup hotspot closes 10 seconds after connection.' : '');
      else if (status.applying || status.result === 'connecting') message = 'Connecting to the saved network. ' + recovery;
      else { message = (errors[status.result] || 'Waiting for a Wi-Fi connection.') + ' ' + recovery; bad = status.result !== 'setup_required'; }
    }
    el.message.textContent = message;
    if (bad) el.message.setAttribute('data-tone', 'error'); else el.message.removeAttribute('data-tone');
    if (status) {
      el.ap.textContent = status.ap_active ? status.ap_ssid + ' · http://' + status.ap_ip : 'Starts automatically if Wi-Fi is unavailable';
      if (online) {
        el.link.hidden = !status.connected || !!waiting || sending;
        if (!el.link.hidden) {
          el.link.href = 'http://' + status.ip_addr + '/';
          el.link.textContent = 'Open controller · ' + status.ip_addr;
        }
      }
    }
  }
  function schedule() {
    if (timer !== null) clearTimeout(timer);
    timer = null;
    if (!document.hidden) timer = setTimeout(refresh, waiting || !online || (status && (status.ap_active || !status.connected)) ? 2000 : 15000);
  }
  function refresh() {
    if (fetching) return Promise.resolve();
    fetching = true;
    var requestedGeneration = generation;
    return fetch('/api/wifi', {cache: 'no-store'}).then(function (response) {
      if (!response.ok) throw new Error('Wi-Fi status unavailable');
      return response.json();
    }).then(function (snapshot) {
      if (requestedGeneration !== generation) return;
      status = snapshot; online = true;
      if (waiting && !snapshot.applying) {
        var matches = snapshot.ssid === waiting.ssid && snapshot.password === waiting.password;
        if (matches) {
          if (sameFields(waiting)) dirty = false;
          waiting = null; uncertain = false; error = '';
        } else if (errors[snapshot.result] && ['save_failed', 'ap_failed', 'storage_failed'].indexOf(snapshot.result) >= 0) {
          waiting = null; uncertain = false; error = errors[snapshot.result];
        } else if (uncertain) {
          waiting = null; uncertain = false; error = 'Save was not confirmed. Review the details before trying again.';
        } else {
          waiting = null; error = 'The saved settings changed in another request. Your draft is kept; review it before saving again.';
        }
      }
      if (!dirty && !sending && !waiting) { el.ssid.value = snapshot.ssid; el.password.value = snapshot.password; }
      render();
    }).catch(function () {
      if (requestedGeneration !== generation) return;
      online = false; render();
    }).then(function () { fetching = false; schedule(); });
  }
  function edited() { dirty = true; error = ''; render(); }
  el.ssid.addEventListener('input', edited);
  el.password.addEventListener('input', edited);
  el.form.addEventListener('submit', function (event) {
    event.preventDefault();
    if (!online || !status || !status.can_save || !status.csrf_token || sending || waiting || status.applying) return;
    var pair = {ssid: el.ssid.value, password: el.password.value};
    var ssidBytes = new TextEncoder().encode(pair.ssid).length;
    var passwordBytes = new TextEncoder().encode(pair.password).length;
    if (!ssidBytes || ssidBytes > 32 || pair.ssid.indexOf('\0') >= 0) {
      error = 'Network name must contain 1–32 UTF-8 bytes.'; render(); return;
    }
    if ((passwordBytes !== 0 && (passwordBytes < 8 || passwordBytes > 63)) || pair.password.indexOf('\0') >= 0) {
      error = 'Password must be empty for an open network, or 8–63 UTF-8 bytes.'; render(); return;
    }
    ++generation; sending = true; waiting = pair; uncertain = false; error = ''; render();
    fetch('/api/wifi', {method: 'PUT', headers: {'Content-Type': 'application/json', 'X-P1S-CSRF': status.csrf_token},
      body: JSON.stringify(pair)}).then(function (response) {
      if (!response.ok) { var rejected = new Error('rejected'); rejected.status = response.status; throw rejected; }
    }).catch(function (failure) {
      if (failure.status) {
        waiting = null;
        error = failure.status === 409 ? 'Another operation is busy. Wait for it to finish and try again.'
          : 'Wi-Fi settings were not accepted. Check the details and try again.';
      } else uncertain = true;
    }).then(function () { sending = false; render(); return refresh(); });
  });
  document.addEventListener('visibilitychange', function () {
    if (document.hidden) { if (timer !== null) clearTimeout(timer); timer = null; }
    else refresh();
  });
  refresh();
})();
