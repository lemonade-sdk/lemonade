const assert = require('node:assert/strict');
const fs = require('node:fs');
const ts = require('typescript');

require.extensions['.ts'] = (module, filename) => {
  module._compile(ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS, esModuleInterop: true },
    fileName: filename,
  }).outputText, filename);
};

const storage = new Map();
global.localStorage = {
  getItem: key => storage.get(key) ?? null,
  setItem: (key, value) => storage.set(key, value),
  removeItem: key => storage.delete(key),
};
const calls = [];
const local = 'http://127.0.0.1:13305';
let controller = 'http://127.0.0.1:15555';
let staleController;
let failedPeer;
let loseMutationReply = false;
let rejectApiKey = false;
const json = (body, status = 200) => new Response(JSON.stringify(body), {
  status, headers: { 'Content-Type': 'application/json' },
});
global.fetch = async (url, init = {}) => {
  const target = new URL(url);
  const headers = new Headers(init.headers);
  calls.push({ url: String(url), headers });
  if (rejectApiKey && target.pathname.endsWith('/nexus/status')) return json({ error: 'Unauthorized' }, 401);
  if (loseMutationReply && target.pathname.endsWith('/tree/root/groups')) throw new TypeError('Connection lost after save');
  if (target.origin === staleController) throw new TypeError('Controller restarted');
  if (target.pathname.includes('/nexus/peers/')) {
    if (failedPeer && target.pathname.includes(`/peers/${failedPeer}/`)) return json({ error: 'Peer unavailable' }, 503);
    if (target.pathname.endsWith('/health')) return json({ status: 'ok', version: 'peer', all_models_loaded: [] });
    if (target.pathname.endsWith('/models')) return json({ data: [] });
  }
  if (target.pathname.endsWith('/nexus/status')) return json({
    available: true, enabled: false, locked: true, mesh_up: false, status: 'disabled', members: [],
    controller_url: controller,
  });
  if (target.pathname.endsWith('/nexus/members')) return json({ error: 'Access denied' }, 403);
  if (target.pathname.endsWith('/health')) return json({ status: 'ok', version: 'local', all_models_loaded: [] });
  if (target.pathname.endsWith('/models')) return json({ data: [] });
  throw new Error(`Unexpected request: ${target.pathname}`);
};

(async () => {
  const { workspaceRouteFromPath } = require('../src/features/navigation/workspaceNavigation.ts');
  assert.deepEqual(workspaceRouteFromPath('connect/devices-and-mesh'), { workspace: 'connect', section: 'nexus' }, 'existing Nexus links keep working');
  const api = require('../src/api.ts').default;
  const nexus = require('../src/features/nexus/nexus.ts');
  await api.loadConnectionSettings();
  api.baseUrl = local;
  api.setSessionApiKey('test-local-key');

  const status = await nexus.nexusRequest('status');
  assert.equal(status.enabled, false);
  assert.equal(calls.at(-2).url, `${local}/api/v1/nexus/status`);
  assert.equal(calls.at(-1).url, `${controller}/api/v1/nexus/status`);
  assert.equal(calls.at(-1).headers.get('Authorization'), 'Bearer test-local-key');
  assert.equal(calls.at(-1).headers.get('X-Lemonade-Peer-Authorization'), null);

  rejectApiKey = true;
  await assert.rejects(nexus.nexusRequest('status'), /valid API key.*Settings > Server/, 'an expired key gives actionable guidance');
  rejectApiKey = false;
  staleController = controller;
  controller = 'http://127.0.0.1:15556';
  await nexus.nexusRequest('status');
  assert.equal(calls.at(-1).url, `${controller}/api/v1/nexus/status`, 'rediscover the management listener after restart');
  await assert.rejects(nexus.nexusRequest('members'), /Access denied/);
  loseMutationReply = true;
  const writesBefore = calls.filter(call => call.url.endsWith('/tree/root/groups')).length;
  await assert.rejects(nexus.nexusRequest('tree/root/groups', { name: 'Test group' }), /Connection lost after save/);
  assert.equal(calls.filter(call => call.url.endsWith('/tree/root/groups')).length, writesBefore + 1, 'non-idempotent group creation is never replayed after a lost response');
  loseMutationReply = false;
  const pinned = await nexus.nexusAccountController();
  api.baseUrl = 'https://different-controller.example';
  api.setSessionApiKey('test-different-key');
  await pinned.request('status');
  assert.equal(calls.at(-1).url, `${controller}/api/v1/nexus/status`, 'an in-flight account flow stays on its original controller');
  assert.equal(calls.at(-1).headers.get('Authorization'), 'Bearer test-local-key', 'a pinned flow retains its original controller credentials');
  api.baseUrl = local;
  api.setSessionApiKey('test-local-key');

  await nexus.selectNexusInferenceServer('gpu', 'test-peer-key');
  assert.equal(api.baseUrl, `${local}/api/v1/nexus/peers/gpu`);
  const relay = calls.filter(call => call.url.includes('/peers/gpu/'));
  assert.ok(relay.length >= 2);
  assert.ok(relay.every(call => call.headers.get('Authorization') === 'Bearer test-local-key'));
  assert.ok(relay.every(call => call.headers.get('X-Lemonade-Peer-Authorization') === 'Bearer test-peer-key'));
  assert.equal(storage.get('lemonade_base_url'), local, 'mesh selection stays in this client session');
  assert.ok([...storage.values()].every(value => !value.includes('test-peer-key')), 'peer credentials are never persisted');
  await nexus.nexusRequest('status');
  assert.equal(calls.at(-1).url, `${controller}/api/v1/nexus/status`, 'management stays on the home server');
  assert.equal(calls.at(-1).headers.get('X-Lemonade-Peer-Authorization'), null);

  failedPeer = 'offline';
  await assert.rejects(nexus.selectNexusInferenceServer('offline', 'test-failed-key'));
  assert.equal(api.baseUrl, `${local}/api/v1/nexus/peers/gpu`, 'failed switches restore the working peer');
  assert.equal(api.peerApiKey, 'test-peer-key');
  assert.equal(api.status, 'connected');
  await nexus.selectNexusInferenceServer('local');
  assert.equal(api.baseUrl, local);
  assert.equal(api.peerApiKey, '');
  assert.equal(calls.at(-1).headers.get('X-Lemonade-Peer-Authorization'), null);

  api.baseUrl = 'https://nexus-server.example';
  api.setSessionApiKey('test-remote-key');
  await nexus.nexusRequest('status');
  assert.equal(calls.at(-1).url, 'https://nexus-server.example/api/v1/nexus/status');
  assert.equal(calls.at(-1).headers.get('Authorization'), 'Bearer test-remote-key');

  api.baseUrl = 'http://127.0.0.1:13306';
  rejectApiKey = true;
  await assert.rejects(nexus.nexusRequest('status'), /valid API key.*Settings > Server/, 'discovery preserves API-key guidance');
  rejectApiKey = false;
  controller = 'https://untrusted-controller.example';
  const before = calls.length;
  await assert.rejects(nexus.nexusRequest('status'), /invalid local network controller/);
  assert.ok(calls.slice(before).every(call => new URL(call.url).hostname === '127.0.0.1'), 'never forward credentials to an invalid controller');
  console.log('Nexus client tests passed: discovery, restart, auth boundaries, relay, rollback, session-only credentials.');
})().catch(error => { console.error(error); process.exitCode = 1; });
