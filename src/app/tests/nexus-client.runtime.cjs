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
let rateLimitedRequests = 0;
let lostReads = 0;
const json = (body, status = 200) => new Response(JSON.stringify(body), {
  status, headers: { 'Content-Type': 'application/json' },
});
global.fetch = async (url, init = {}) => {
  const target = new URL(url);
  const headers = new Headers(init.headers);
  calls.push({ url: String(url), headers });
  if (target.pathname.endsWith('/nexus/rate-limit-test')) {
    if (rateLimitedRequests-- > 0) return json({ error: 'The network is busy. Try again shortly.', retryable: true }, 429);
    return json({ ready: true });
  }
  if (target.pathname.endsWith('/nexus/lost-connection-test')) {
    // The server closed a fresh connection before answering, as a descriptor-starved
    // lemond does: a read may be sent again, a mutation must not be.
    if (init.method === 'POST' || lostReads-- > 0) throw new TypeError('Load failed');
    return json({ ready: true });
  }
  if (rejectApiKey && target.pathname.endsWith('/nexus/status')) return json({ error: 'Unauthorized' }, 401);
  if (loseMutationReply && target.pathname.endsWith('/tree/root/groups')) throw new TypeError('Connection lost after save');
  if (target.origin === staleController) throw new TypeError('Controller restarted');
  if (target.pathname.includes('/nexus/peers/')) {
    if (failedPeer && target.pathname.includes(`/peers/${failedPeer}/`)) return json({ error: 'Peer unavailable' }, 503);
    if (target.pathname.endsWith('/health')) return json({ status: 'ok', version: 'peer', all_models_loaded: [] });
    if (target.pathname.endsWith('/models')) return json({ data: [] });
  }
  if (target.pathname.endsWith('/nexus/enable')) return json({});
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
  for (const identity_source of ['user', 'handoff', 'system']) {
    assert.equal(nexus.usesNexusDeviceKey({ registered: true, unlock: 'passkey', identity_source }), false,
      'a registered passkey takes precedence over the device key storage location');
  }
  assert.equal(nexus.usesNexusDeviceKey({ registered: false, unlock: 'passkey', identity_source: 'user' }), true,
    'an unregistered user identity can still initialize with its device key');
  assert.equal(nexus.usesNexusDeviceKey({ registered: true, unlock: 'device_key', identity_source: 'user' }), true);
  assert.equal(nexus.usesNexusDeviceKey({ registered: false, unlock: 'passkey', identity_source: 'system' }), true,
    'a packaged server with no registered passkey sets itself up with its device key');
  assert.equal(nexus.usesNexusDeviceKey({ registered: true, unlock: 'device_key', identity_source: 'system' }), true);
  assert.equal(nexus.usesNexusDeviceKey({ registered: false, unlock: 'passkey' }), false, 'an unknown identity source keeps the passkey path');
  // A retryable key/unlock failure reads as two sentences whatever punctuation lemond used.
  for (const [reason, expected] of [['Set the Genesis key explicitly', 'Set the Genesis key explicitly. The network could not be reached. Try again shortly.'],
    ['No network server found for mesh.example.', 'No network server found for mesh.example. The network could not be reached. Try again shortly.']]) {
    const keyRequest = async path => {
      if (path === 'status') return { available: true, enabled: true, locked: true, mesh_up: false, status: 'locked', registered: false, unlock: 'passkey', identity_source: 'system', members: [] };
      if (path === 'key/unlock') throw new nexus.NexusRequestError(reason, 503, true);
      return {};
    };
    await assert.rejects(nexus.prepareNexus('Server', false, keyRequest), { message: expected });
  }
  await api.loadConnectionSettings();
  api.baseUrl = local;
  api.setSessionApiKey('test-local-key');

  const status = await nexus.nexusRequest('status');
  assert.equal(status.enabled, false);
  assert.equal(calls.at(-2).url, `${local}/api/v1/nexus/status`);
  assert.equal(calls.at(-1).url, `${controller}/api/v1/nexus/status`);
  assert.equal(calls.at(-1).headers.get('Authorization'), 'Bearer test-local-key');
  assert.equal(calls.at(-1).headers.get('X-Lemonade-Peer-Authorization'), null);

  rateLimitedRequests = 1;
  assert.deepEqual(await nexus.nexusRequest('rate-limit-test'), { ready: true }, 'a rate-limited read retries once');
  rateLimitedRequests = 2;
  await assert.rejects(nexus.nexusRequest('rate-limit-test', {}), error => error.status === 429);
  assert.equal(rateLimitedRequests, 1, 'a rate-limited mutation is never replayed');
  rateLimitedRequests = 0;

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
  lostReads = 1;
  const lostReadsBefore = calls.filter(call => call.url.endsWith('/nexus/lost-connection-test')).length;
  assert.deepEqual(await pinned.request('lost-connection-test'), { ready: true }, 'a pinned read is sent again after the server closed the connection');
  assert.equal(calls.filter(call => call.url.endsWith('/nexus/lost-connection-test')).length, lostReadsBefore + 2);
  assert.ok(calls.slice(-2).every(call => call.url === `${controller}/api/v1/nexus/lost-connection-test`), 'the repeated read stays on the pinned controller');
  await assert.rejects(pinned.request('lost-connection-test', {}), /Load failed/, 'a pinned mutation is not replayed');
  assert.equal(calls.filter(call => call.url.endsWith('/nexus/lost-connection-test')).length, lostReadsBefore + 3);
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

  api.baseUrl = 'http://192.168.10.20:13305';
  api.setSessionApiKey('');
  await nexus.nexusRequest('enable', {});
  assert.equal(calls.at(-1).url, 'http://192.168.10.20:13305/api/v1/nexus/enable');
  assert.equal(calls.at(-1).headers.get('Authorization'), null);

  api.baseUrl = 'http://127.0.0.1:13306';
  rejectApiKey = true;
  await assert.rejects(nexus.nexusRequest('status'), /valid API key.*Settings > Server/, 'discovery preserves API-key guidance');
  rejectApiKey = false;
  controller = 'https://untrusted-controller.example';
  const before = calls.length;
  await assert.rejects(nexus.nexusRequest('status'), /invalid local network controller/);
  assert.ok(calls.slice(before).every(call => new URL(call.url).hostname === '127.0.0.1'), 'never forward credentials to an invalid controller');

  controller = 'http://127.0.0.1:13305';
  api.baseUrl = 'http://localhost:13305';
  await nexus.nexusRequest('status');
  assert.equal(calls.at(-1).url, 'http://localhost:13305/api/v1/nexus/status', 'a controller on the public port keeps the origin the page already uses');
  controller = 'http://127.0.0.1:15556';
  api.baseUrl = local;
  await nexus.nexusRequest('status');
  assert.equal(calls.at(-1).url, 'http://127.0.0.1:15556/api/v1/nexus/status', 'a controller on another port is addressed directly');
  // A client on the server's local network is a local operator: lemond names the public port
  // as its controller and the page keeps the origin it reached. Only lemond decides: a status
  // without controller_url, or one naming a port the client cannot reach, stays remote.
  controller = 'http://127.0.0.1:13305';
  api.baseUrl = 'http://192.168.10.20:13305';
  await nexus.nexusRequest('enable', {});
  assert.deepEqual(calls.slice(-2).map(call => call.url),
    ['http://192.168.10.20:13305/api/v1/nexus/status', 'http://192.168.10.20:13305/api/v1/nexus/enable'],
    'a local-network client discovers the controller once and keeps its origin');
  const reuseBefore = calls.length;
  await nexus.nexusRequest('status');
  assert.equal(calls.length - reuseBefore, 1, 'the discovered controller is reused');
  controller = 'http://127.0.0.1:15556';
  api.baseUrl = 'http://192.168.10.21:13305';
  const remoteBefore = calls.length;
  await nexus.nexusRequest('status');
  assert.equal(calls.length - remoteBefore, 2, 'a controller on a port the local-network client cannot reach is probed once');
  assert.ok(calls.slice(remoteBefore).every(call => call.url.startsWith('http://192.168.10.21:13305/')), 'the client never addresses the ephemeral controller');
  await nexus.nexusRequest('status');
  assert.equal(calls.length - remoteBefore, 3, 'the remote decision is reused too');
  controller = 'http://127.0.0.1:15556';
  api.baseUrl = local;
  // Device pills and the mesh summary follow the server's online flag and dataplane path.
  const up = { available: true, enabled: true, locked: false, mesh_up: true, status: 'running', node_id: 'self', members: [] };
  const device = (extra) => ({ node_id: 'peer', device_name: 'Peer', tunnel_ip: '10.8.0.9/32', management_pubkey: 'k', permissions: [], can_remove: false, ...extra });
  assert.equal(nexus.nexusDeviceConnection(up, device({ is_online: true, connection_type: 'p2p' })), 'p2p');
  assert.equal(nexus.nexusDeviceConnection(up, device({ is_online: true, connection_type: 'relay' })), 'relay');
  assert.equal(nexus.nexusDeviceConnection(up, device({ is_online: true, connection_type: 'unknown' })), 'connecting', 'an online device without a path is connecting');
  assert.equal(nexus.nexusDeviceConnection(up, device({ is_online: false, connection_type: 'relay' })), 'offline');
  assert.equal(nexus.nexusDeviceConnection(up, device({ can_connect: true })), 'connecting', 'a reachable device the server has not reported yet is connecting');
  assert.equal(nexus.nexusDeviceConnection(up, device({})), undefined);
  assert.equal(nexus.nexusDeviceConnection(up, device({ node_id: 'self', is_online: true })), undefined, 'this device has no connection pill');
  assert.equal(nexus.nexusDeviceConnection({ ...up, locked: true, mesh_up: false }, device({ is_online: true })), undefined, 'a locked mesh reports no paths');
  assert.deepEqual(nexus.nexusDeviceTags(up, device({ node_id: 'self', is_inference: true, is_admin: true })), ['This device', 'Server', 'Admin']);
  assert.deepEqual(nexus.nexusDeviceTags(up, device({ is_inference: false, member: true, group_role: 'member' })), ['Client', 'Member', 'Joined network']);
  assert.deepEqual(nexus.nexusDeviceTags(up, device({})), [], 'a device without a tier reports no tier pill');
  const servers = [device({ node_id: 'gpu', is_inference: true, can_connect: true, is_online: true, connection_type: 'p2p' }), device({ node_id: 'cpu', is_inference: true, can_connect: true, is_online: true }),
    device({ node_id: 'denied', is_inference: true, can_connect: false, is_online: true }), device({ node_id: 'phone', is_inference: false, is_online: true }), device({ node_id: 'self', is_inference: true })];
  assert.deepEqual(nexus.nexusInferenceServers({ ...up, members: servers }).map(d => d.node_id), ['gpu', 'cpu'], 'chat runs only on inference servers this device may connect to');
  assert.equal(nexus.nexusServerUnavailable({ ...up, members: servers }, servers[0]), undefined, 'a server with a path can serve chat');
  assert.equal(nexus.nexusServerUnavailable({ ...up, members: servers }, servers[1]), 'connecting', 'an online server without a path yet cannot');
  assert.equal(nexus.nexusServerUnavailable(up, device({ is_online: false, connection_type: 'p2p' })), 'offline');
  assert.equal(nexus.nexusServerUnavailable(up, device({ is_inference: true })), 'offline', 'a server with no reported state counts as offline');
  assert.deepEqual(nexus.nexusMeshSummary({ ...up, members: servers }), { state: 'connected', label: '1 server online' });
  assert.deepEqual(nexus.nexusMeshSummary({ ...up, members: servers.map(d => d.node_id === 'gpu' ? { ...d, connection_type: 'relay' } : d.node_id === 'cpu' ? { ...d, connection_type: 'p2p' } : d) }), { state: 'connected', label: '{count} servers online', count: 2 });
  assert.deepEqual(nexus.nexusMeshSummary({ ...up, members: servers.slice(1) }), { state: 'connected', label: 'Connecting…' });
  assert.deepEqual(nexus.nexusMeshSummary({ ...up, members: [servers[1]].map(d => ({ ...d, is_online: false })) }), { state: 'connected', label: 'Servers offline' });
  assert.deepEqual(nexus.nexusMeshSummary(up), { state: 'connected', label: 'No servers yet' });
  assert.deepEqual(nexus.nexusMeshSummary(up, 'Cannot reach the server'), { state: 'offline', label: 'Offline' });
  assert.deepEqual(nexus.nexusMeshSummary({ ...up, locked: true, mesh_up: false }), { state: 'locked', label: 'Locked' });
  assert.deepEqual(nexus.nexusMeshSummary({ ...up, mesh_up: false, status: 'joining' }), { state: 'connecting', label: 'Connecting…' });
  assert.deepEqual(nexus.nexusMeshSummary({ ...up, enabled: false }), { state: 'off', label: 'Not set up' });
  assert.deepEqual(nexus.nexusMeshSummary(null), { state: 'off', label: 'Not set up' });
  console.log('Nexus client tests passed: discovery, restart, auth boundaries, relay, rollback, session-only credentials, pills and mesh summary.');
})().catch(error => { console.error(error); process.exitCode = 1; });
