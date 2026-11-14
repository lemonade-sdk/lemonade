// #1221 on #3930: inviting a Lemonade server found on this network, and joining it from here.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const ts = require('typescript');
require.extensions['.ts'] = (module, filename) => module._compile(ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS, esModuleInterop: true }, fileName: filename,
}).outputText, filename);
const { createNexusAccountService } = require('../src/features/nexus/accountSession.ts');
global.localStorage = { getItem: () => null, setItem: () => undefined };

const source = 'http://127.0.0.1:13305';
const account = {
  id: 'cluster', member_id: 'identity', device_node_id: 'this-device', role: 'admin',
  devices: [{ node_id: 'this-device', device_name: 'Workstation', permissions_known: true, is_admin: true, is_online: true, permissions: ['admin'] }],
};
const home = [];
const controller = async () => ({ source, local: true, request: async (path, body) => {
  home.push({ path, body });
  if (path === 'status') return { available: true, enabled: true, registered: true, unlock: 'device_key', locked: false, mesh_up: true, status: 'running', members: [] };
  if (path === 'account') return structuredClone(account);
  if (path === 'invites') return { code: '12345678', link_token: `lnk_${'a'.repeat(64)}`, device_name: body.device_name, expires_at: Date.now() / 1000 + 600 };
  throw new Error(`unexpected home request ${path}`);
} });

function target(kind) {
  const calls = [];
  const request = async (path, body) => {
    calls.push({ path, body });
    if (path === 'status') {
      if (kind === 'no-nexus') throw new Error('404');
      if (kind === 'passkey') return { available: true, registered: true, unlock: 'passkey', locked: true, mesh_up: false, status: 'locked', members: [] };
      return { available: true, registered: kind === 'own-mesh', unlock: 'device_key', locked: kind !== 'own-mesh', mesh_up: kind === 'own-mesh' || kind === 'refused', status: 'running', members: [] };
    }
    if (path === 'join') return { status: 'joined' };
    if (path === 'invites/accept') return kind === 'refused' ? { status: 'refused', error: 'expired' } : { status: 'member', group_id: 'home' };
    return {};
  };
  return { calls, request };
}

(async () => {
  const session = { account: { id: 'cluster', displayName: 'Workstation', role: 'admin' }, devices: [], memberId: 'identity', deviceNodeId: 'this-device', source, canInvite: true, avatar: { state: 'unsupported' } };
  const run = async (kind) => {
    const t = target(kind);
    const prepared = [];
    const service = createNexusAccountService({
      controller, desktop: () => true,
      prepare: async (name, joining, request, before) => { prepared.push({ name, joining }); const s = await request('status'); before?.(s); },
      requestAt: endpoint => { assert.equal(endpoint, 'http://192.168.4.24:13305'); return t.request; },
    });
    const result = await service.inviteFoundServer(session, 'http://192.168.4.24:13305', 'cesarops2');
    return { result, calls: t.calls, prepared };
  };

  // A fresh device-key server: set up as joining, then it joins with the code made here.
  let r = await run('fresh');
  assert.equal(r.result.joined, true);
  assert.equal(r.result.code, '12345678');
  assert.deepEqual(r.prepared, [{ name: 'cesarops2', joining: true }]);
  assert.deepEqual(r.calls.at(-1), { path: 'join', body: { code: '12345678' } });
  assert.deepEqual(home.find(c => c.path === 'invites').body.device_name, 'cesarops2', 'the invite carries the found server name');

  // A server already on its own mesh accepts the invite as a member, as Join by invitation does.
  r = await run('own-mesh');
  assert.equal(r.result.joined, true);
  assert.equal(r.result.membership.status, 'member');
  assert.equal(r.calls.at(-1).path, 'invites/accept');

  // No network support: nothing is changed there, and the code comes back to enter by hand.
  r = await run('no-nexus');
  assert.equal(r.result.joined, false);
  assert.equal(r.result.code, '12345678');
  assert.match(r.result.error, /without network support/);
  assert.deepEqual(r.prepared, []);

  // A passkey device has to verify its user on that device; it is not set up from here.
  r = await run('passkey');
  assert.equal(r.result.joined, false);
  assert.match(r.result.error, /passkey/);
  assert.deepEqual(r.prepared, []);

  // A refused membership is reported, with the code.
  r = await run('refused');
  assert.equal(r.result.joined, false);
  assert.equal(r.result.membership.status, 'refused');
  assert.match(r.result.error, /expired/);

  console.log('Found-server invites: fresh join, own-mesh member, no Nexus, passkey device and refused membership passed.');
})().catch(error => { console.error(error); process.exit(1); });
