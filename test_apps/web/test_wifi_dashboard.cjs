const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../../web/wifi.js'), 'utf8');
const html = fs.readFileSync(path.join(__dirname, '../../web/index.html'), 'utf8');
const flush = async () => { for (let i = 0; i < 6; i++) await new Promise(setImmediate); };

function browser(initial = {}) {
  const nodes = new Map(), timers = new Map(), calls = [], busy = [];
  let sequence = 0, offline = false, loseReply = false, reply = 202;
  let putWait = null, releasePut = null, getWait = null, releaseGet = null;
  let wifi = {ssid: 'saved network', password: 'visible password', configured: true, connected: true,
    controller_name: 'left', requested_name: 'left', hostname: 'p1s-left', name_pending: false,
    applying: false, can_save: true, ap_active: false, ap_ssid: 'P1S-Heat-Setup',
    ap_ip: '192.168.4.1', ip_addr: '10.0.0.31', result: 'connected', csrf_token: 'token-one', ...initial};
  const element = id => {
    if (!nodes.has(id)) nodes.set(id, {value: '', textContent: '', disabled: false, hidden: false,
      listeners: {}, setAttribute(k, v) {this[k] = v;}, removeAttribute(k) {delete this[k];},
      addEventListener(k, fn) {this.listeners[k] = fn;}});
    return nodes.get(id);
  };
  const context = {console, TextEncoder, document: {hidden: false, getElementById: element, addEventListener() {}},
    CustomEvent: class {constructor(type, options) {this.type = type; this.detail = options.detail;}},
    dispatchEvent(event) {busy.push(event.detail);},
    setTimeout(fn, delay) {const id = ++sequence; timers.set(id, {fn, delay}); return id;},
    clearTimeout(id) {timers.delete(id);},
    fetch: async (url, options = {}) => {
      calls.push({url, options});
      if (options.method === 'PUT') {
        if (putWait) await putWait;
        if (loseReply) throw Error('response lost');
        if (reply === 202) wifi = {...wifi, applying: true, can_save: false};
        return {ok: reply === 202, status: reply};
      }
      if (offline) throw Error('offline');
      const snapshot = {...wifi};
      if (getWait) await getWait;
      return {ok: true, json: async () => snapshot};
    }};
  context.window = context;
  vm.runInNewContext(source, context, {filename: 'wifi.js'});
  return {element, calls, busy, setWifi(v) {wifi = {...wifi, ...v};},
    holdPut() {putWait = new Promise(resolve => {releasePut = resolve;});},
    releasePut() {putWait = null; releasePut();},
    holdGet() {getWait = new Promise(resolve => {releaseGet = resolve;});},
    releaseGet() {getWait = null; releaseGet();},
    offline(v) {offline = v;}, loseReply(v) {loseReply = v;}, reply(v) {reply = v;},
    edit(ssid, password, name) {
      element('wifi-ssid').value = ssid; element('wifi-password').value = password;
      if (name !== undefined) element('wifi-controller-name').value = name;
      element('wifi-ssid').listeners.input();
    },
    submit() {element('wifi-form').listeners.submit({preventDefault() {}});},
    async poll() {
      const entry = [...timers.entries()].at(-1); assert.ok(entry, 'poll scheduled');
      timers.delete(entry[0]); entry[1].fn(); await flush();
    }};
}

test('SSID and password are visible text fields with labels', () => {
  for (const id of ['wifi-ssid', 'wifi-password']) {
    const input = html.match(new RegExp('<input[^>]*id="' + id + '"[^>]*>'));
    assert.ok(input, id + ' is present'); assert.match(input[0], /type="text"/);
    assert.match(html, new RegExp('<label[^>]*for="' + id + '"'));
  }
});
test('saved credentials are shown and polling preserves unfinished edits', async () => {
  const b = browser(); await flush();
  assert.equal(b.element('wifi-password').value, 'visible password');
  b.edit(' new network ', ' new password '); await b.poll();
  assert.equal(b.element('wifi-ssid').value, ' new network ');
  assert.equal(b.element('wifi-password').value, ' new password ');
});
test('Save sends exact text once and immediately disables duplicate submission', async () => {
  const b = browser(); await flush(); b.edit(' next ', ' password\\with spaces ');
  b.submit(); b.submit();
  assert.equal(b.element('wifi-save-button').disabled, true);
  assert.match(b.element('wifi-message').textContent, /Saving|Connecting/i);
  assert.equal(b.busy.at(-1), true);
  await flush();
  const saves = b.calls.filter(c => c.options.method === 'PUT');
  assert.equal(saves.length, 1);
  assert.deepEqual(JSON.parse(saves[0].options.body), {ssid: ' next ', password: ' password\\with spaces ', controller_name: 'left'});
  assert.equal(saves[0].options.headers['X-P1S-CSRF'], 'token-one');
  assert.equal(saves[0].url, '/api/wifi');
});
test('accepted save cannot overwrite a newer draft', async () => {
  const b = browser(); await flush(); b.edit('next', 'next password'); b.submit(); await flush();
  b.edit('later draft', 'later password');
  b.setWifi({ssid: 'next', password: 'next password', applying: false, can_save: true});
  await b.poll();
  assert.equal(b.element('wifi-ssid').value, 'later draft');
  assert.equal(b.element('wifi-password').value, 'later password');
});
test('lost reply never automatically repeats Save and preserves recovery instructions', async () => {
  const b = browser(); await flush(); b.loseReply(true); b.offline(true);
  b.edit('wrong network', 'wrong password'); b.submit(); await flush(); await b.poll(); await b.poll();
  assert.equal(b.calls.filter(c => c.options.method === 'PUT').length, 1);
  assert.match(b.element('wifi-message').textContent, /192\.168\.4\.1/);
  assert.match(b.element('wifi-message').textContent, /P1S-Heat-Setup/);
  assert.equal(b.element('wifi-ssid').value, 'wrong network');
  b.offline(false); await b.poll();
  assert.equal(b.element('wifi-save-button').disabled, false);
  assert.match(b.element('wifi-message').textContent, /not confirmed/i);
});
test('a failed persistence operation retains the draft and allows a deliberate retry', async () => {
  const b = browser(); await flush(); b.edit('next', 'next password'); b.submit(); await flush();
  b.setWifi({applying: false, can_save: true, result: 'save_failed'}); await b.poll();
  assert.equal(b.element('wifi-ssid').value, 'next');
  assert.match(b.element('wifi-message').textContent, /save|storage/i);
  assert.equal(b.element('wifi-save-button').disabled, false);
});
test('wrong credentials show a reachable correction path', async () => {
  const b = browser({connected: false, ap_active: true, result: 'auth_failed'}); await flush();
  assert.match(b.element('wifi-message').textContent, /password/i);
  assert.match(b.element('wifi-message').textContent, /192\.168\.4\.1/);
  assert.equal(b.element('wifi-save-button').disabled, false);
});
test('success exposes the station address and explains hotspot shutdown', async () => {
  const b = browser({ap_active: true}); await flush();
  assert.equal(b.element('wifi-station-link').href, 'http://10.0.0.31/');
  assert.match(b.element('wifi-message').textContent, /10 seconds/);
  b.offline(true); await b.poll();
  assert.equal(b.element('wifi-station-link').href, 'http://10.0.0.31/');
  assert.equal(b.element('wifi-station-link').hidden, false);
});
test('initial boot maintenance does not block an available Wi-Fi Save', async () => {
  const b = browser({configured: false, connected: false, ap_active: true, can_save: true, result: 'setup_required'});
  await flush(); assert.equal(b.element('wifi-save-button').disabled, false);
});
test('backend operation ownership and UTF-8 byte limits prevent a save', async () => {
  const b = browser({can_save: false}); await flush(); b.submit();
  assert.equal(b.calls.filter(c => c.options.method === 'PUT').length, 0);
  b.setWifi({can_save: true}); await b.poll();
  b.edit('é'.repeat(17), '12345678'); b.submit(); await flush();
  assert.equal(b.calls.filter(c => c.options.method === 'PUT').length, 0);
  assert.match(b.element('wifi-message').textContent, /32/);
});
test('busy rejection retains text and does not silently resubmit', async () => {
  const b = browser(); await flush(); b.reply(409); b.edit('next', 'next password'); b.submit(); await flush();
  await b.poll(); assert.equal(b.calls.filter(c => c.options.method === 'PUT').length, 1);
  assert.equal(b.element('wifi-ssid').value, 'next');
  assert.match(b.element('wifi-message').textContent, /another|busy/i);
});
test('a save completed by another browser releases busy state and keeps this draft', async () => {
  const b = browser(); await flush(); b.edit('my network', 'my password'); b.submit(); await flush();
  b.setWifi({ssid: 'other network', password: 'other password', applying: false, can_save: true});
  await b.poll(); await b.poll();
  assert.equal(b.element('wifi-save-button').disabled, false);
  assert.equal(b.busy.at(-1), false);
  assert.equal(b.element('wifi-ssid').value, 'my network');
  assert.match(b.element('wifi-message').textContent, /changed|another/i);
  assert.equal(b.calls.filter(c => c.options.method === 'PUT').length, 1);
});
test('a successful unseen network change retains a discovery address after the AP closes', async () => {
  const b = browser(); await flush(); b.edit('new network', 'new password'); b.submit();
  b.offline(true); await flush(); await b.poll();
  assert.equal(b.element('wifi-station-link').hidden, true);
  assert.equal(b.element('wifi-discovery-link').href, 'http://p1s-left.local/');
  assert.equal(b.element('wifi-discovery-link').hidden, false);
  assert.match(b.element('wifi-message').textContent, /normal Wi-Fi|new Wi-Fi/i);
  assert.match(b.element('wifi-message').textContent, /hotspot.*closed|hotspot.*closes/i);
  assert.doesNotMatch(b.element('wifi-message').textContent, /saved address below/i);
});

test('saved controller name is editable and polling preserves its draft', async () => {
  const b = browser(); await flush();
  assert.equal(b.element('wifi-controller-name').value, 'left');
  b.edit('saved network', 'visible password', 'right'); await b.poll();
  assert.equal(b.element('wifi-controller-name').value, 'right');
  assert.match(b.element('wifi-name-preview').textContent, /http:\/\/p1s-right\.local\//);
  assert.equal(b.element('wifi-discovery-link').href, 'http://p1s-left.local/');
});

test('blank name previews the first numbered address', async () => {
  const b = browser(); await flush(); b.edit('saved network', 'visible password', '');
  assert.match(b.element('wifi-name-preview').textContent, /p1s-1\.local/);
});

test('name-only saves normalize the name and retain the IP recovery link', async () => {
  const b = browser(); await flush(); b.edit('saved network', 'visible password', ' Right-2 ');
  b.submit(); await flush();
  const saves = b.calls.filter(c => c.options.method === 'PUT');
  assert.equal(saves.length, 1);
  assert.deepEqual(JSON.parse(saves[0].options.body), {
    ssid: 'saved network', password: 'visible password', controller_name: 'right-2'});
  assert.equal(b.element('wifi-station-link').hidden, false);
  assert.equal(b.element('wifi-station-link').href, 'http://10.0.0.31/');
});

test('invalid names have an inline error and cannot be saved', async () => {
  for (const name of ['two words', '-left', 'left-', 'left.local', 'a/b', 'é', 'x'.repeat(23)]) {
    const b = browser(); await flush(); b.edit('saved network', 'visible password', name);
    b.submit(); await flush();
    assert.equal(b.calls.filter(c => c.options.method === 'PUT').length, 0);
    assert.equal(b.element('wifi-name-error').hidden, false);
    assert.equal(b.element('wifi-controller-name')['aria-invalid'], 'true');
  }
});

test('name confirmation keeps Save busy until the final numbered address is returned', async () => {
  const b = browser(); await flush(); b.submit(); await flush();
  b.setWifi({applying: false, can_save: true, controller_name: '1', hostname: 'p1s-1', name_pending: true});
  await b.poll();
  assert.equal(b.element('wifi-save-button').disabled, true);
  assert.match(b.element('wifi-message').textContent, /checking|confirming/i);
  b.setWifi({controller_name: '2', hostname: 'p1s-2', name_pending: false}); await b.poll();
  assert.equal(b.element('wifi-save-button').disabled, false);
  assert.equal(b.element('wifi-controller-name').value, '2');
  assert.equal(b.element('wifi-discovery-link').href, 'http://p1s-2.local/');
  assert.equal(b.element('wifi-discovery-link').textContent, 'p1s-2.local');
});

test('a newer name draft survives confirmation of an older save', async () => {
  const b = browser(); await flush(); b.submit(); await flush();
  b.edit('saved network', 'visible password', 'new-draft');
  b.setWifi({applying: false, can_save: true, controller_name: '1', hostname: 'p1s-1', name_pending: false});
  await b.poll();
  assert.equal(b.element('wifi-controller-name').value, 'new-draft');
  assert.equal(b.element('wifi-discovery-link').href, 'http://p1s-1.local/');
});

test('lost rename reply retains the proposed address and unchanged setup hotspot', async () => {
  const b = browser(); await flush(); b.loseReply(true); b.offline(true);
  b.edit('saved network', 'visible password', 'right'); b.submit(); await flush(); await b.poll();
  assert.equal(b.element('wifi-discovery-link').href, 'http://p1s-right.local/');
  assert.match(b.element('wifi-message').textContent, /P1S-Heat-Setup/);
  assert.match(b.element('wifi-message').textContent, /192\.168\.4\.1/);
  assert.equal(b.calls.filter(c => c.options.method === 'PUT').length, 1);
});

test('discovery failure leaves Wi-Fi settings and IP recovery usable', async () => {
  const b = browser(); await flush(); b.submit(); await flush();
  b.setWifi({applying: false, can_save: true, connected: true, name_pending: true, result: 'driver_failed'});
  await b.poll();
  assert.equal(b.element('wifi-save-button').disabled, false);
  assert.equal(b.element('wifi-station-link').hidden, false);
  assert.match(b.element('wifi-message').textContent, /address.*retry|retry.*address/i);
});

test('a poll during Save cannot discard the requested hostname before a lost reply', async () => {
  const b = browser({ap_active: true}); await flush();
  b.holdPut(); b.edit('new network', 'new password', 'right'); b.submit(); await flush();
  await b.poll();
  b.setWifi({ssid: 'new network', password: 'new password', controller_name: 'right',
    requested_name: 'right', hostname: 'p1s-right'});
  b.offline(true); b.loseReply(true); b.releasePut(); await flush(); await b.poll();
  assert.equal(b.element('wifi-discovery-link').href, 'http://p1s-right.local/');
  assert.match(b.element('wifi-message').textContent, /p1s-right\.local/);
  assert.equal(b.element('wifi-controller-name').value, 'right');
  assert.equal(b.calls.filter(c => c.options.method === 'PUT').length, 1);
});

test('a GET begun during Save cannot confirm it after the PUT reply arrives', async () => {
  const b = browser({controller_name: '2', requested_name: 'left', hostname: 'p1s-2'}); await flush();
  b.holdPut(); b.edit('saved network', 'visible password', 'left'); b.submit(); await flush();
  b.holdGet(); await b.poll();
  b.releasePut(); await flush();
  b.releaseGet(); await flush();
  assert.equal(b.element('wifi-controller-name').value, 'left');
  assert.equal(b.element('wifi-save-button').disabled, true);
  b.setWifi({controller_name: 'left', requested_name: 'left', hostname: 'p1s-left',
    applying: false, can_save: true, name_pending: false});
  await b.poll();
  assert.equal(b.element('wifi-controller-name').value, 'left');
  assert.equal(b.element('wifi-discovery-link').href, 'http://p1s-left.local/');
  assert.equal(b.element('wifi-save-button').disabled, false);
});

test('an old matching status cannot erase a rename draft before Save rejection', async () => {
  const b = browser({controller_name: '2', requested_name: 'left', hostname: 'p1s-2'}); await flush();
  b.holdPut(); b.edit('saved network', 'visible password', 'left'); b.submit(); await flush();
  await b.poll();
  b.reply(503); b.releasePut(); await flush();
  assert.equal(b.element('wifi-controller-name').value, 'left');
  assert.equal(b.element('wifi-save-button').disabled, false);
  assert.match(b.element('wifi-message').textContent, /not accepted/i);
  assert.equal(b.calls.filter(c => c.options.method === 'PUT').length, 1);
});
