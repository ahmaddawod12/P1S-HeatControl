const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../../web/app.js'), 'utf8');
const flush = async () => { for (let i = 0; i < 5; i++) await new Promise(setImmediate); };

function browser(initial = {}, initialStatus = {}) {
  const nodes = new Map(), timers = new Map(), calls = [], events = {}, stream = {};
  const element = id => {
    if (!nodes.has(id)) nodes.set(id, { value: '', textContent: '', disabled: false, hidden: false,
      listeners: {}, setAttribute(k, v) { this[k] = v; }, removeAttribute(k) { delete this[k]; },
      addEventListener(k, fn) { this.listeners[k] = fn; } });
    return nodes.get(id);
  };
  let firmware = {installed: '1.0.0', candidate: '', phase: 'idle', result: 'none',
    maintenance: false, trial_pending: false, received: 0, total: 0, boot_id: 'boot-one', csrf_token: 'token-one', ...initial};
  let offline = false, losePost = false, reloads = 0, sequence = 0, commandFailure = 0;
  const document = {hidden: false, getElementById: element, addEventListener() {}};
  const context = {document, console, location: {reload() { reloads++; }},
    setTimeout(fn, delay) { const id = ++sequence; timers.set(id, {fn, delay}); return id; },
    clearTimeout(id) { timers.delete(id); },
    EventSource: class {addEventListener(name, fn) {stream[name] = fn;}},
    fetch: async (url, options = {}) => {
      calls.push({url, options});
      if (url === '/api/firmware') {
        if (offline) throw Error('offline');
        return {ok: true, json: async () => ({...firmware})};
      }
      if (url === '/api/firmware/update') {
        if (losePost) throw Error('response lost');
        return {ok: true, status: 202, json: async () => ({})};
      }
      if (options.method && commandFailure) return {ok: false, status: commandFailure,
        json: async () => ({message: 'Command rejected.'})};
      if (url === '/api/calibration' && options.method === 'PUT') {
        initialStatus.calibration_offset_c = Math.fround(JSON.parse(options.body).calibration_offset_c);
      }
      return {ok: true, json: async () => ({state: 'idle', target_c: 60, valid: true,
        calibration_offset_c: 0, consecutive_valid: 3, enabled: false, heater_on: false, fan_on: false, fault: 'none', ...initialStatus})};
    }};
  context.window = context;
  context.addEventListener = (name,fn) => {events[name]=fn;};
  vm.runInNewContext(source, context, {filename: 'app.js'});
  return {element, calls, printer(detail) {events['p1s-printer-control']({detail});}, get reloads() {return reloads;},
    setFirmware(v) {firmware = {...firmware, ...v};}, offline(v) {offline = v;}, losePost(v) {losePost = v;},
    rejectCommands(status) {commandFailure=status;},
    editCalibration(value) {element('calibration-offset').value = String(value); element('calibration-offset').listeners.input();},
    status(detail) {stream.status({data: JSON.stringify({state: 'idle', target_c: 60, valid: true,
      consecutive_valid: 3, enabled: false, fault: 'none', ...initialStatus, ...detail})});},
    async click() {const button = element('firmware-update-button'); assert.equal(typeof button.listeners.click, 'function'); button.listeners.click(); await flush();},
    async command(id) {element(id).listeners.click(); await flush();},
    async target(value) {element('target-temperature').value = String(value); element('target-temperature').listeners.change(); await flush();},
    async poll() {const entry = [...timers.entries()].at(-1); assert.ok(entry, 'firmware poll is scheduled'); timers.delete(entry[0]); entry[1].fn(); await flush();}};
}

test('sensor calibration defaults to zero and loads a saved signed offset', async () => {
  const fresh = browser(); await flush();
  assert.equal(fresh.element('calibration-offset').value, '0');
  const saved = browser({}, {calibration_offset_c: -1.5}); await flush();
  assert.equal(saved.element('calibration-offset').value, '-1.5');
  saved.status({calibration_offset_c: 2.5});
  assert.equal(saved.element('calibration-offset').value, '2.5');
});

test('calibration saves signed values once with the current command token', async () => {
  const b = browser(); await flush();
  b.setFirmware({csrf_token: 'calibration-token'});
  b.editCalibration(-1.5); await b.command('calibration-save-button');
  const sent = b.calls.filter(c => c.url === '/api/calibration');
  assert.equal(sent.length, 1);
  assert.equal(sent[0].options.method, 'PUT');
  assert.equal(sent[0].options.headers['X-P1S-CSRF'], 'calibration-token');
  assert.deepEqual(JSON.parse(sent[0].options.body), {calibration_offset_c: -1.5});
  assert.match(b.element('status-message').textContent, /-1\.5/);
});

test('calibration rejects blank, nonnumeric and out-of-range drafts without sending', async () => {
  const b = browser(); await flush();
  for (const value of ['', ' ', 'not a number', '1oops', '-10.1', '10.1', 'Infinity']) {
    b.editCalibration(value); await b.command('calibration-save-button');
    assert.equal(b.calls.filter(c => c.url === '/api/calibration').length, 0);
    assert.match(b.element('status-message').textContent, /between -10 and 10/i);
  }
});

test('decimal calibration stays readable and later status changes update a saved field', async () => {
  const b = browser({}, {calibration_offset_c: Math.fround(1.1)}); await flush();
  assert.equal(b.element('calibration-offset').value, '1.1');
  b.editCalibration('2.1'); await b.command('calibration-save-button');
  assert.equal(b.element('calibration-offset').value, '2.1');
  b.status({calibration_offset_c: 3.5});
  assert.equal(b.element('calibration-offset').value, '3.5');
});

test('status updates and failed saves preserve calibration drafts', async () => {
  const b = browser(); await flush();
  b.editCalibration(''); b.status({calibration_offset_c: 1});
  assert.equal(b.element('calibration-offset').value, '');
  b.editCalibration('-2.5'); b.status({calibration_offset_c: 1});
  assert.equal(b.element('calibration-offset').value, '-2.5');
  b.rejectCommands(500); await b.command('calibration-save-button');
  assert.equal(b.element('calibration-offset').value, '-2.5');
  assert.equal(b.element('status-message').textContent, 'Command rejected.');
  assert.equal(b.calls.filter(c => c.url === '/api/calibration').length, 1);
});

test('printer automation and firmware maintenance block calibration saves', async () => {
  const b = browser(); await flush();
  b.printer({active: true});
  assert.equal(b.element('calibration-offset').disabled, true);
  assert.equal(b.element('calibration-save-button').disabled, true);
  await b.command('calibration-save-button');
  assert.equal(b.calls.filter(c => c.url === '/api/calibration').length, 0);
  b.printer({active: false});
  b.setFirmware({maintenance: true}); await b.poll();
  assert.equal(b.element('calibration-offset').disabled, true);
  await b.command('calibration-save-button');
  assert.equal(b.calls.filter(c => c.url === '/api/calibration').length, 0);
});

test('every chamber mutation sends a freshly read per-boot CSRF token once', async () => {
  const b = browser(); await flush();
  const commands = [
    ['enable-button', '/api/control/enable'], ['disable-button', '/api/control/disable'],
    ['fan-toggle-button', '/api/control/fan/enable'], ['acknowledge-button', '/api/fault/acknowledge']
  ];
  for (let i = 0; i < commands.length; i++) {
    b.setFirmware({csrf_token: 'boot-token-' + i});
    await b.command(commands[i][0]);
    const sent = b.calls.filter(c => c.url === commands[i][1]);
    assert.equal(sent.length, 1);
    assert.equal(sent[0].options.headers?.['X-P1S-CSRF'], 'boot-token-' + i);
  }
  b.setFirmware({csrf_token: 'target-token'}); await b.target(60);
  const sent = b.calls.filter(c => c.url === '/api/target'); assert.equal(sent.length, 1);
  assert.equal(sent[0].options.headers['X-P1S-CSRF'], 'target-token');
  assert.equal(sent[0].options.headers['Content-Type'], 'application/json');
});

test('a failed token read never submits or retries a chamber command', async () => {
  const b = browser(); await flush(); b.offline(true); await b.command('disable-button');
  assert.equal(b.calls.filter(c => c.options.method === 'POST').length, 0);
  assert.match(b.element('status-message').textContent, /could not be reached/i);
  b.offline(false); b.setFirmware({csrf_token: 'after-reboot'}); await b.command('disable-button');
  const sent=b.calls.filter(c => c.url === '/api/control/disable'); assert.equal(sent.length,1);
  assert.equal(sent[0].options.headers['X-P1S-CSRF'],'after-reboot');
});

test('stopping a manual fan uses the same current-token request flow', async () => {
  const b=browser({}, {manual_fan_on: true}); await flush();
  b.setFirmware({csrf_token: 'fan-stop-token'}); await b.command('fan-toggle-button');
  const sent=b.calls.filter(c => c.options.method === 'POST'); assert.equal(sent.length,1);
  assert.equal(sent[0].url,'/api/control/fan/disable');
  assert.equal(sent[0].options.headers['X-P1S-CSRF'],'fan-stop-token');
});

test('an Origin or token rejection is displayed without retrying Disable', async () => {
  const b=browser(); await flush(); b.rejectCommands(403); await b.command('disable-button'); await b.poll();
  assert.equal(b.calls.filter(c=>c.url==='/api/control/disable').length,1);
  assert.equal(b.element('status-message').textContent,'Command rejected.');
});

test('one click requests one installation with the current CSRF token', async () => {
  const b = browser(); await flush(); await b.click();
  const posts = b.calls.filter(c => c.url === '/api/firmware/update');
  assert.equal(posts.length, 1); assert.equal(posts[0].options.method, 'POST');
  assert.equal(posts[0].options.headers['X-P1S-CSRF'], 'token-one');
});
test('automatic printer ownership locks target, Enable and update while fan and Disable stay available',async()=>{
  const b=browser();await flush();b.printer({active:true,known:true});
  for(const id of ['target-slider','target-temperature','enable-button','firmware-update-button'])assert.equal(b.element(id).disabled,true,id);
  assert.equal(b.element('fan-toggle-button').disabled,false);assert.equal(b.element('disable-button').disabled,false);
  b.element('enable-button').listeners.click();b.element('target-temperature').listeners.change();await b.click();
  assert.equal(b.calls.filter(c=>c.options.method).length,0);
  b.printer({active:false,known:true});assert.equal(b.element('target-slider').disabled,false);
});
test('maintenance locks heating controls and leaves Disable available', async () => {
  const b = browser({phase: 'downloading', maintenance: true}); await flush();
  assert.equal(b.element('enable-button').disabled, true);
  assert.equal(b.element('target-slider').disabled, true);
  assert.equal(b.element('disable-button').disabled, false);
});
test('a lost POST response is never automatically retried', async () => {
  const b = browser(); await flush(); b.losePost(true); await b.click();
  await b.poll(); await b.poll();
  assert.equal(b.calls.filter(c => c.url === '/api/firmware/update').length, 1);
});
test('reboot downtime stays pending and a new installed version reloads once', async () => {
  const b = browser({phase: 'rebooting', maintenance: true}); await flush();
  b.offline(true); await b.poll();
  assert.doesNotMatch(b.element('firmware-message').textContent, /success|confirmed|updated/i);
  b.offline(false); b.setFirmware({installed: '1.0.1', phase: 'succeeded', result: 'installed',
    maintenance: false, boot_id: 'boot-two', csrf_token: 'token-two'});
  await b.poll(); await b.poll(); assert.equal(b.reloads, 1);
});
test('rollback to the previous version reports failure without a reload loop', async () => {
  const b = browser(); await flush();
  b.setFirmware({phase: 'failed', result: 'rolled_back', boot_id: 'boot-two', csrf_token: 'token-two'});
  await b.poll(); assert.equal(b.reloads, 0);
  assert.match(b.element('firmware-message').textContent, /rolled back/i);
  await b.click();
  assert.equal(b.calls.filter(c => c.url === '/api/firmware/update').at(-1).options.headers['X-P1S-CSRF'], 'token-two');
});
