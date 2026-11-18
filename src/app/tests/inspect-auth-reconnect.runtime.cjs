const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require('typescript');

const file = path.resolve(__dirname, '../src/inspectStore.ts');
const src = fs.readFileSync(file, 'utf8');
const parsed = ts.createSourceFile(file, src, ts.ScriptTarget.Latest, true);
const clazz = parsed.statements.find(item => ts.isClassDeclaration(item) && item.name?.text === 'InspectStoreClass');
assert.ok(clazz);
const methods = ['hasActiveSocket', 'connect', 'disconnect'].map(name => {
  const found = clazz.members.find(member => member.name?.getText(parsed) === name);
  assert.ok(found, `${name} missing`);
  return found.getText(parsed);
});
const program = `class Store {
  state = { capturing: true, captureReady: 'connecting' };
  ws = null;
  reconnectTimeout = null;
  currentWsUrl = '';
  currentWsApiKey = '';
  connectionGeneration = 0;
  toasts = [];
  setState(next) { Object.assign(this.state, next); }
  showToast(message) { this.toasts.push(message); }
  addTrace() {}
  ${methods.join('\n')}
}
module.exports = Store;`;
const compiled = ts.transpileModule(program, {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  reportDiagnostics: true,
});
assert.deepEqual((compiled.diagnostics || []).filter(e => e.category === ts.DiagnosticCategory.Error), []);

const connections = [];
const scheduled = [];
class FakeSocket {
  static CONNECTING = 0;
  static OPEN = 1;
  static CLOSED = 3;
  readyState = FakeSocket.CONNECTING;
  sent = [];
  constructor(url) { this.url = url; connections.push(this); }
  open() { this.readyState = FakeSocket.OPEN; this.onopen?.(); }
  send(value) { this.sent.push(JSON.parse(value)); }
  receive(value) { this.onmessage?.({ data: JSON.stringify(value) }); }
  close() { this.readyState = FakeSocket.CLOSED; this.onclose?.(); }
}
const api = { baseUrl: 'http://localhost:13305', apiKey: '', clientSessionId: 'capture-1', sessionHeadersEnabled: false };
const scope = {
  module: { exports: {} }, WebSocket: FakeSocket, URLSearchParams,
  api, parseOtelSpan: () => null, console,
  setTimeout: cb => { scheduled.push(cb); return scheduled.length; },
  clearTimeout: () => {},
};
vm.runInNewContext(compiled.outputText, scope);
const store = new scope.module.exports();

store.connect();
assert.equal(connections.length, 1);
connections[0].open();
assert.equal(connections[0].sent[0].token, '');
connections[0].receive({ type: 'error', error: { message: 'Invalid API key' } });
assert.equal(store.state.captureReady, 'disconnected');
assert.equal(store.toasts.length, 0, 'no stale-key toast on browser page reload');
assert.equal(store.ws, null);
assert.equal(scheduled.length, 0, 'invalid auth must not cause an automatic retry loop');
assert.equal(api.sessionHeadersEnabled, false);

api.apiKey = 'wrong-key';
store.connect();
connections[1].open();
connections[1].receive({ type: 'error', error: { message: 'Invalid API key' } });
assert.equal(store.toasts.length, 1, 'a genuinely wrong key produces one visible error');
assert.equal(store.state.captureReady, 'disconnected');
assert.equal(scheduled.length, 0);

api.apiKey = 'correct-key';
store.connect();
assert.equal(connections.length, 3);
connections[2].open();
assert.equal(connections[2].sent[0].token, 'correct-key');
connections[2].receive({ type: 'auth.ok' });
assert.equal(store.state.captureReady, 'ready');
assert.equal(api.sessionHeadersEnabled, true);
store.connect();
assert.equal(connections.length, 3, 'unchanged key reuses the socket');

api.apiKey = 'rotated-key';
store.connect();
assert.equal(connections.length, 4, 'key change must reopen the websocket');
assert.equal(store.state.captureReady, 'connecting', 'new credentials are not yet authenticated');
assert.equal(api.sessionHeadersEnabled, false, 'old capture headers must stop immediately');
connections[3].open();
assert.equal(connections[3].sent[0].token, 'rotated-key');
connections[3].receive({ type: 'auth.ok' });
assert.equal(store.state.captureReady, 'ready');
connections[3].close();
assert.equal(store.state.captureReady, 'connecting', 'transient websocket close remains retryable');
assert.equal(scheduled.length, 1, 'transient disconnect should schedule one reconnect');
store.disconnect();
assert.equal(store.state.captureReady, 'disconnected');
assert.equal(api.sessionHeadersEnabled, false);
console.log('Inspect auth reconnect runtime checks passed.');
