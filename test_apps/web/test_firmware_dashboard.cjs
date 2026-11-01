const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../../web/app.js'), 'utf8');
const flush = async () => { for (let i = 0; i < 5; i++) await new Promise(setImmediate); };

function browser(initial = {}) {
  const nodes = new Map(), timers = new Map(), calls = [];
  const element = id => {
    if (!nodes.has(id)) nodes.set(id, { value: '', textContent: '', disabled: false, hidden: false,
      listeners: {}, setAttribute(k, v) { this[k] = v; }, removeAttribute(k) { delete this[k]; },
      addEventListener(k, fn) { this.listeners[k] = fn; } });
    return nodes.get(id);
  };
  let firmware = {installed: '1.0.0', candidate: '', phase: 'idle', result: 'none',
    maintenance: false, trial_pending: false, received: 0, total: 0, boot_id: 'boot-one', csrf_token: 'token-one', ...initial};
  let offline = false, losePost = false, reloads = 0, sequence = 0;
  const document = {hidden: false, getElementById: element, addEventListener() {}};
  const context = {document, console, location: {reload() { reloads++; }},
    setTimeout(fn, delay) { const id = ++sequence; timers.set(id, {fn, delay}); return id; },
    clearTimeout(id) { timers.delete(id); },
    EventSource: class {addEventListener() {}},
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
      return {ok: true, json: async () => ({state: 'idle', target_c: 60, valid: true,
        consecutive_valid: 3, enabled: false, heater_on: false, fan_on: false, fault: 'none'})};
    }};
  context.window = context;
  vm.runInNewContext(source, context, {filename: 'app.js'});
  return {element, calls, get reloads() {return reloads;},
    setFirmware(v) {firmware = {...firmware, ...v};}, offline(v) {offline = v;}, losePost(v) {losePost = v;},
    async click() {const button = element('firmware-update-button'); assert.equal(typeof button.listeners.click, 'function'); button.listeners.click(); await flush();},
    async poll() {const entry = [...timers.entries()].at(-1); assert.ok(entry, 'firmware poll is scheduled'); timers.delete(entry[0]); entry[1].fn(); await flush();}};
}

test('one click requests one installation with the current CSRF token', async () => {
  const b = browser(); await flush(); await b.click();
  const posts = b.calls.filter(c => c.url === '/api/firmware/update');
  assert.equal(posts.length, 1); assert.equal(posts[0].options.method, 'POST');
  assert.equal(posts[0].options.headers['X-P1S-CSRF'], 'token-one');
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
