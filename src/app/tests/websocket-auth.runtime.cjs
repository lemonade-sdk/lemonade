const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require('typescript');

const filename = path.resolve(__dirname, '../src/api.ts');
const source = fs.readFileSync(filename, 'utf8');
const parsed = ts.createSourceFile(filename, source, ts.ScriptTarget.Latest, true);
const apiClass = parsed.statements.find(item => ts.isClassDeclaration(item) && item.name?.text === 'LemonadeAPI');
assert.ok(apiClass);
const names = ['_buildWebSocketUrl', '_openRealtimeSocket', '_openLogSocket', 'connectLogStream', 'connectRealtimeTranscription'];
const methods = apiClass.members.filter(member => names.includes(member.name?.getText(parsed)) || member.name?.getText(parsed) === '_webSocketProtocols');
assert.equal(methods.filter(member => names.includes(member.name?.getText(parsed))).length, names.length);
const compiled = ts.transpileModule(`class Client {
  baseUrl = 'https://example.test:13305/api/v1';
  apiKey = '';
  clientSessionId = 'session /?';
  _healthData = { websocket_port: 9000 };
  ${methods.map(member => member.getText(parsed)).join('\n')}
}
module.exports = Client;`, {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  reportDiagnostics: true,
});
assert.deepEqual((compiled.diagnostics || []).filter(item => item.category === ts.DiagnosticCategory.Error), []);

const connections = [];
class FakeSocket {
  static OPEN = 1;
  readyState = 0;
  listeners = {};
  sent = [];
  constructor(url, protocols) { this.url = url; this.protocols = protocols; connections.push(this); }
  addEventListener(name, callback) { this.listeners[name] = callback; }
  emit(name) { if (name === 'open') this.readyState = FakeSocket.OPEN; this.listeners[name]?.({ code: 1000 }); }
  send(value) { this.sent.push(JSON.parse(value)); }
  close() { this.readyState = 3; }
}
const scope = {
  module: { exports: {} }, URL, URLSearchParams, TextEncoder, btoa,
  WebSocket: FakeSocket,
  window: { setTimeout: () => 1, clearTimeout: () => {} },
};
vm.runInNewContext(compiled.outputText, scope);
const flush = async () => { for (let i = 0; i < 5; i++) await Promise.resolve(); };

function checkSocket(socket, client, path, port, model) {
  const url = new URL(socket.url);
  assert.ok(!url.searchParams.has('api_key'), `${path}: credentials must not appear in URL queries`);
  assert.equal(url.protocol, client.baseUrl.startsWith('https:') ? 'wss:' : 'ws:');
  assert.equal(url.pathname, path);
  assert.equal(url.port, String(port));
  assert.equal(url.searchParams.get('client_session_id'), client.clientSessionId);
  assert.equal(url.searchParams.get('model'), model ?? null);
  const protocols = Array.from(socket.protocols || []);
  if (client.apiKey) {
    const expected = `bearer.${Buffer.from(client.apiKey, 'utf8').toString('base64url')}`;
    assert.ok(protocols.length === 2 && protocols[0] === 'lemonade-realtime' && protocols[1] === expected,
      `${path}: must offer registered protocol and UTF-8 base64url credential`);
    assert.ok(/^bearer\.[A-Za-z0-9_-]+$/.test(protocols[1]), 'credential must use unpadded base64url');
  } else {
    assert.equal(socket.protocols, undefined, 'anonymous sockets must preserve the unauthenticated handshake');
  }
}

(async () => {
  for (const key of ['', 'ascii-secret', 'key+/= café 🔑']) {
    for (const scheme of ['http', 'https']) {
      const client = new scope.module.exports();
      client.apiKey = key;
      client.baseUrl = `${scheme}://example.test:13305/api/v1`;
      const logs = client.connectLogStream({}, 42);
      const mainLog = connections.at(-1);
      mainLog.emit('error');
      await flush();
      const fallbackLog = connections.at(-1);
      assert.notEqual(fallbackLog, mainLog, 'failed main log socket must fall back');
      fallbackLog.emit('open');
      await flush();
      assert.equal(fallbackLog.sent[0].after_seq, 42);
      logs.close();

      const model = 'model /? café';
      const realtime = client.connectRealtimeTranscription(model);
      const mainRealtime = connections.at(-1);
      mainRealtime.emit('error');
      await flush();
      const fallbackRealtime = connections.at(-1);
      assert.notEqual(fallbackRealtime, mainRealtime, 'failed main realtime socket must fall back');
      fallbackRealtime.emit('open');
      (await realtime).close();
      assert.equal(fallbackRealtime.sent[0].session.model, model);
      checkSocket(mainLog, client, '/logs/stream', 13305);
      checkSocket(fallbackLog, client, '/logs/stream', 9000);
      checkSocket(mainRealtime, client, '/v1/realtime', 13305, model);
      checkSocket(fallbackRealtime, client, '/realtime', 9000, model);
    }
  }
  console.log('WebSocket credential transport regression checks passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
