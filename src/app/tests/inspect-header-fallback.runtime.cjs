const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require('typescript');

const file = path.resolve(__dirname, '../src/api.ts');
const parsed = ts.createSourceFile(file, fs.readFileSync(file, 'utf8'), ts.ScriptTarget.Latest, true);
const owner = parsed.statements.find(s => ts.isClassDeclaration(s) && s.name?.text === 'LemonadeAPI');
assert.ok(owner);
const methods = ['_headers', '_fetch'].map(name => {
  const found = owner.members.find(m => m.name?.getText(parsed) === name);
  assert.ok(found, `${name} must exist`);
  return found.getText(parsed);
});
const compiled = ts.transpileModule(`class Client {
  baseUrl = 'http://localhost:13305';
  apiKey = 'example';
  adminApiKey = 'admin';
  clientSessionId = 'capture-session';
  sessionHeadersEnabled = true;
  onSessionHeadersFailed = () => {};
  ${methods.join('\n')}
}
module.exports = Client;`, {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  reportDiagnostics: true,
});
assert.deepEqual((compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error), []);

async function scenario(responses, expectCalls, shouldDisable) {
  let attempts = 0;
  let notificationCount = 0;
  const requests = [];
  const scope = {
    module: { exports: {} },
    Headers, FormData, ReadableStream, Blob, URL,
    serverErrorMessage: text => text,
    originNotAllowedMessage: () => 'Origin not allowed',
    fetch: async (_url, init) => {
      requests.push(init.headers);
      const response = responses[attempts++];
      if (response instanceof Error) throw response;
      if (!response) throw new Error('Unexpected network call');
      return response;
    },
  };
  vm.runInNewContext(compiled.outputText, scope);
  const client = new scope.module.exports();
  client.onSessionHeadersFailed = () => { notificationCount++; };
  try {
    await client._fetch('/api/v1/test', {
      method: 'POST', body: { payload: true }, includeSessionHeaders: true,
    });
  } catch (e) {
    assert.ok(e, 'server errors must still be surfaced');
  }
  assert.equal(attempts, expectCalls);
  assert.equal(client.sessionHeadersEnabled, !shouldDisable);
  assert.equal(notificationCount, shouldDisable ? 1 : 0);
  assert.ok(requests[0]['X-Client-Session-Id']);
  if (expectCalls > 1) assert.ok(!requests[1]['X-Client-Session-Id']);
}
const response = (status, body) => new Response(body, { status });
(async () => {
  for (const code of [400, 401, 403, 405]) {
    await scenario([response(code, 'Normal validation/authentication failure')], 1, false);
  }
  await scenario([response(400, 'X-Client-Session-Id header not supported'), response(200, 'OK')], 2, true);
  await scenario([response(403, 'X-Client-Session-Id rejected'), response(403, 'Forbidden')], 2, false);
  await scenario([new Error('CORS failure'), response(200, 'OK')], 2, true);
  await scenario([new Error('Transient network error'), response(400, 'Invalid input')], 2, false);
  await scenario([new Error('Disconnected'), new Error('Disconnected')], 2, false);
  console.log('Inspect session header fallback regression checks passed.');
})().catch(err => { console.error(err); process.exitCode = 1; });
