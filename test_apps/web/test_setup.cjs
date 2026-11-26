const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const readPage = name => fs.readFileSync(path.join(__dirname, '../../web', name), 'utf8');
const wifiPage = readPage('wifi_setup.html');
const pairingPage = readPage('pairing_setup.html');

function view(html, fetch, pathname = '/') {
  const elements = {};
  const events = {};
  const pending = [];
  const redirects = [];
  const byId = id => elements[id] ??= {
    value: '', textContent: '', disabled: false,
    addEventListener: (type, handler) => { events[id + '/' + type] = handler; }
  };
  const ready = vm.runInNewContext(html.match(/<script>([\s\S]+)<\/script>/)[1], {
    document: {getElementById: byId},
    location: {pathname, replace: url => redirects.push(url)},
    setTimeout: fn => pending.push(fn),
    AbortSignal, fetch
  });
  return {
    ready, byId, redirects,
    poll: () => pending.shift()?.(),
    submit: () => events['wifi/submit']({preventDefault() {}})
  };
}

const boardA = '7C87CE01020A';
const boardB = '7C87CE01020B';
const boardHeaders = (id = boardA) => new Headers({'X-P1S-Board-ID': id});
const ok = (data, id) => ({
  ok: true, status: 200, headers: boardHeaders(id), json: async () => data, text: async () => data
});
const changed = {ok: false, status: 404};

test('Wi-Fi setup sends only credentials and reloads when pairing begins', async () => {
  assert.doesNotMatch(wifiPage, /controller name|pairing code|\/api\/pairing/i);
  const requests = [];
  let snapshot = {ssid: 'home', csrf_token: 'token', can_save: true};
  const browser = view(wifiPage, async (url, options) => {
    assert.equal(url, '/api/wifi');
    requests.push(options);
    if (options.method === 'PUT') return ok({save_id: 7});
    return ok(snapshot);
  });
  await browser.ready;
  assert.equal(browser.byId('ssid').value, 'home');
  browser.byId('password').value = 'home-password';
  await browser.submit();
  assert.deepEqual(JSON.parse(requests[1].body), {ssid: 'home', password: 'home-password'});
  assert.equal(browser.byId('connect').disabled, true);
  snapshot = {...snapshot, pairing: true, save_id: 7, save_result: 'saved'};
  await browser.poll();
  assert.deepEqual(browser.redirects, ['/']);
});

test('Wi-Fi setup preserves a newer draft and sends an empty password for an open network', async () => {
  let snapshot = {ssid: 'home', csrf_token: 'token', can_save: true};
  let sent;
  const browser = view(wifiPage, async (url, options) => {
    assert.equal(url, '/api/wifi');
    if (options.method !== 'PUT') return ok(snapshot);
    sent = JSON.parse(options.body);
    return ok({save_id: 7});
  });
  await browser.ready;
  browser.byId('ssid').value = 'open';
  await browser.submit();
  assert.deepEqual(sent, {ssid: 'open', password: ''});
  browser.byId('password').value = 'newer-draft';
  snapshot = {...snapshot, save_id: 7, save_result: 'saved', result: 'connecting'};
  await browser.poll();
  assert.equal(browser.byId('password').value, 'newer-draft');
});

test('a Wi-Fi setup tab cannot submit settings to a different board', async () => {
  let board = boardA;
  let saves = 0;
  const browser = view(wifiPage, async (_url, options) => {
    if (options.method === 'PUT') {
      saves++;
      return ok({save_id: 7}, board);
    }
    return ok({ssid: 'home', csrf_token: board, can_save: true}, board);
  });
  await browser.ready;
  assert.equal(browser.byId('board-id').textContent, 'Board ' + boardA);
  assert.equal(browser.byId('connect').disabled, false);
  browser.byId('password').value = 'home-password';
  board = boardB;
  await browser.poll();
  assert.equal(browser.byId('board-id').textContent, 'Board ' + boardA);
  assert.match(browser.byId('status').textContent, /controller changed/);
  assert.equal(browser.byId('connect').disabled, true);
  await browser.submit();
  assert.equal(saves, 0);
});

test('pairing recovery stays on the network form until the replacement connects', async () => {
  assert.match(pairingPage, /href="\/wifi">Change Wi-Fi network</);
  let snapshot = {ssid: 'isolated-network', csrf_token: 'token', can_save: true, pairing: true};
  const browser = view(wifiPage, async (url, options) => {
    assert.equal(url, '/api/wifi');
    if (options.method === 'PUT') return ok({save_id: 7});
    return ok(snapshot);
  }, '/wifi');
  await browser.ready;
  assert.deepEqual(browser.redirects, []);
  assert.equal(browser.byId('return-to-pairing').hidden, false);
  assert.equal(browser.byId('connect').disabled, false);
  browser.byId('ssid').value = 'replacement-network';
  await browser.submit();
  snapshot = {...snapshot, save_id: 7, save_result: 'saved', pairing: false, result: 'connecting'};
  await browser.poll();
  assert.deepEqual(browser.redirects, []);
  assert.equal(browser.byId('return-to-pairing').hidden, true);
  snapshot = {...snapshot, pairing: true, result: 'connected'};
  await browser.poll();
  assert.deepEqual(browser.redirects, ['/']);
});

test('pairing polls only its code and reloads after a Wi-Fi disconnect', async () => {
  assert.doesNotMatch(pairingPage, /<form|<input|\/api\/wifi/);
  let response = ok('');
  const browser = view(pairingPage, async url => {
    assert.equal(url, '/api/pairing');
    return response;
  });
  await browser.ready;
  assert.match(browser.byId('code').textContent, /Getting/);
  response = ok('test-code');
  await browser.poll();
  assert.equal(browser.byId('code').textContent, 'test-code');
  assert.equal(browser.byId('pairing-instructions').hidden, false);
  response = {ok: false, status: 409, headers: boardHeaders(), text: async () => 'USB enrollment recovery required.'};
  await browser.poll();
  assert.equal(browser.byId('code').textContent, 'USB enrollment recovery required.');
  response = {ok: false, status: 409, headers: boardHeaders(),
    text: async () => "This controller's Firebase account is disabled."};
  await browser.poll();
  assert.match(browser.byId('code').textContent, /Firebase account is disabled/);
  assert.equal(browser.byId('pairing-instructions').hidden, true);
  response = ok('fresh-code');
  await browser.poll();
  assert.equal(browser.byId('pairing-instructions').hidden, false);
  response = changed;
  await browser.poll();
  assert.deepEqual(browser.redirects, ['/']);
});

test('a pairing tab clears the code when the connected board changes', async () => {
  let response = ok('code-for-first-board', boardA);
  const browser = view(pairingPage, async () => response);
  await browser.ready;
  assert.equal(browser.byId('board-id').textContent, 'Board ' + boardA);
  assert.equal(browser.byId('code').textContent, 'code-for-first-board');
  response = ok('code-for-second-board', boardB);
  await browser.poll();
  assert.equal(browser.byId('board-id').textContent, 'Board ' + boardA);
  assert.match(browser.byId('code').textContent, /controller changed/);
  assert.equal(browser.byId('pairing-instructions').hidden, true);
});
