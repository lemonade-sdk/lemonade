// Headless setup: a system-identity server proves ownership with its setup code instead of a passkey.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const ts = require('typescript');
require.extensions['.ts'] = (module, filename) => module._compile(ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS, esModuleInterop: true }, fileName: filename,
}).outputText, filename);
global.localStorage = { getItem: () => null, setItem: () => undefined };
const { usesNexusDeviceKey, prepareNexus } = require('../src/features/nexus/nexus.ts');
const { createNexusAccountService } = require('../src/features/nexus/accountSession.ts');

(async () => {
  const headless = { available: true, enabled: true, registered: false, unlock: 'passkey', identity_source: 'system',
    locked: true, mesh_up: false, status: 'locked', members: [], setup_code_required: true, setup_code_file: '/var/lib/lemonade/.config/lemonade/nexus/setup-code' };
  assert.equal(usesNexusDeviceKey(headless), true, 'a server asking for its setup code unlocks with its device key');
  assert.equal(usesNexusDeviceKey({ ...headless, setup_code_required: undefined }), false, 'without the setup code flag a system identity still uses a passkey');
  assert.equal(usesNexusDeviceKey({ ...headless, registered: true, unlock: 'passkey', setup_code_required: true }), false, 'a registered passkey device never switches to its key');

  // prepareNexus sends the code with the key unlock, trimmed, and only when one is given.
  const calls = [];
  const request = async (path, body) => { calls.push({ path, body }); return path === 'status' ? headless : {}; };
  await prepareNexus('rack-server', true, request, undefined, undefined, '  ABCD-EFGH-JKMN ');
  assert.deepEqual(calls.find(c => c.path === 'key/unlock').body, { defer_join: true, setup_code: 'ABCD-EFGH-JKMN' });
  calls.length = 0;
  await prepareNexus('desk', false, request);
  assert.deepEqual(calls.find(c => c.path === 'key/unlock').body, { defer_join: false }, 'no code is sent when none was entered');

  // signIn passes the code from the Enable Mesh dialog to prepare.
  const prepared = [];
  const account = { id: 'cluster', member_id: 'identity', device_node_id: 'self', role: 'admin',
    devices: [{ node_id: 'self', device_name: 'rack-server', permissions_known: true, is_admin: true, is_online: true, permissions: ['admin'] }] };
  const service = createNexusAccountService({
    controller: async () => ({ source: 'http://192.168.4.24:13305', local: true, request: async path => path === 'account' ? account : headless }),
    desktop: () => false,
    prepare: async (name, joining, req, before, setupCode) => { prepared.push({ name, joining, setupCode }); },
  });
  await service.signIn({ action: 'create', deviceName: 'rack-server', source: 'http://192.168.4.24:13305', setupCode: 'ABCD-EFGH-JKMN' });
  assert.deepEqual(prepared, [{ name: 'rack-server', joining: false, setupCode: 'ABCD-EFGH-JKMN' }]);
  console.log('Headless setup: setup-code servers use the device key, the code reaches key/unlock, sign-in passes it.');
})().catch(error => { console.error(error); process.exit(1); });
