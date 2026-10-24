const assert = require('node:assert/strict');
const fs = require('node:fs');
const ts = require('typescript');
require.extensions['.ts'] = (module, filename) => module._compile(ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS, esModuleInterop: true }, fileName: filename,
}).outputText, filename);
const { createNexusAccountService, verifyAccountReply, saveNexusAccountLabel, loadNexusAccountLabel } = require('../src/features/nexus/accountSession.ts');
const storage = new Map();
global.localStorage = { getItem: key => storage.get(key) || null, setItem: (key, value) => storage.set(key, value) };
const source = 'http://127.0.0.1:13305';
const calls = [];
let currentSource = source;
let local = true;
let desktop = true;
let failPasskey = false;
let registered = false;
let reply = {
  id: 'cluster', member_id: 'identity', device_node_id: 'this-device', role: 'admin',
  devices: [{node_id:'this-device',device_name:'Workstation',permissions_known:true,is_admin:true,is_online:true,permissions:['admin']},
    {node_id:'phone',device_name:'Phone',permissions_known:true,is_admin:false,is_online:false,permissions:['read']}],
};
const request = async (path, body) => {
  calls.push({path,body});
  if (path === 'status') return { available:true,enabled:true,registered,locked:true,mesh_up:false,status:'locked',members:[] };
  if (path === 'account') return structuredClone(reply);
  if (path === 'join') return {};
  if (path === 'invites') return {code:'12345678',link_token:`lnk_${'a'.repeat(64)}`,device_name:body.device_name,expires_at:Date.now()/1000+600};
  throw new Error(`Unexpected operation ${path}`);
};
const service = createNexusAccountService({
  controller: async () => ({ source: currentSource, local, request }), desktop: () => desktop,
  prepare: async (deviceName, joining, pinned, guard) => {
    calls.push({path:'verify-passkey',joining});
    guard(await pinned('status'));
    if (failPasskey) throw new Error('Security key cancelled');
  },
});
const input = { action:'create',displayName:'Geramy',deviceName:'Workstation',source };
(async () => {
  const info = await service.inspect();
  assert.equal(info.source, source);
  assert.equal(calls.some(call=>call.path==='verify-passkey'),false,'inspection never authenticates automatically');
  const account = await service.signIn(input);
  assert.equal(account.account.displayName,'Geramy');
  assert.equal(account.account.id,'cluster');
  assert.equal(account.account.role,'admin');
  assert.equal(account.devices[0].current,true);
  assert.equal(account.canInvite,true);
  assert.ok(calls.findIndex(call=>call.path==='verify-passkey')<calls.findIndex(call=>call.path==='account'),'membership is read only after explicit verification');
  saveNexusAccountLabel(account);
  assert.deepEqual(loadNexusAccountLabel(source),{id:'cluster',displayName:'Geramy'});
  assert.ok([...storage.values()].every(value=>!value.includes('identity')&&!value.includes('permissions')&&!value.includes('token')),'only account ID and display label are persisted');
  const original = [...storage.values()][0];
  failPasskey = true;
  const reads = calls.filter(call=>call.path==='account').length;
  await assert.rejects(service.signIn(input),/cancelled/);
  assert.equal(calls.filter(call=>call.path==='account').length,reads,'a cancelled passkey cannot produce a signed-in account');
  assert.equal([...storage.values()][0],original,'failed authentication preserves the saved account');
  failPasskey = false;
  await assert.rejects(service.signIn({...input,action:'join',invitation:'bad'}),/8-digit/);
  await service.signIn({...input,action:'join',invitation:'12345678'});
  const join = calls.findIndex(call=>call.path==='join');
  assert.equal(calls[join-2].path,'verify-passkey');
  assert.equal(calls[join].body.code,'12345678');
  registered = true;
  await assert.rejects(service.signIn(input),/already has a registered key/);
  await service.signIn({...input,action:'return',expectedId:'cluster'});
  currentSource = 'https://another-server.example';
  await assert.rejects(service.signIn(input),/connection changed/);
  await assert.rejects(service.refresh(account),/connection changed/);
  currentSource = source; local = false;
  await assert.rejects(service.signIn(input),/desktop app/);
  local = true; desktop = false;
  await assert.rejects(service.signIn(input),/desktop app/);
  desktop = true;
  const invite = await service.invite(account,'My phone');
  assert.equal(invite.code,'12345678');
  assert.throws(()=>verifyAccountReply({...reply,role:'superadmin'},'Name',source),/verified account/);
  assert.throws(()=>verifyAccountReply({...reply,device_node_id:'not-a-member'},'Name',source),/permissions/);
  assert.throws(()=>verifyAccountReply(reply,'Name',source,'different-cluster'),/different account/);
  reply = {...reply,member_id:'other-identity'};
  await assert.rejects(service.refresh(account),/membership changed/);
  assert.equal([...storage.values()][0],original);
  console.log('Nexus account flow: explicit verification, create/join/return, roles, server pinning, cancellation, invitations and non-secret labels passed.');
})().catch(error=>{console.error(error);process.exitCode=1;});
