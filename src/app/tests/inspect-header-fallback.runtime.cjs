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

async function scenario(responses, expectCalls, shouldDisable, method = 'POST', signal) {
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
  let error;
  let result;
  try {
    result = await client._fetch('/api/v1/test', {
      method,
      ...(['GET', 'HEAD', 'OPTIONS'].includes(method) ? {} : { body: { payload: true } }),
      includeSessionHeaders: true,
      signal,
    });
  } catch (caught) {
    error = caught;
  }
  assert.equal(attempts, expectCalls, `${method}: unexpected request replay`);
  assert.equal(client.sessionHeadersEnabled, !shouldDisable);
  assert.equal(notificationCount, shouldDisable ? 1 : 0);
  assert.ok(requests[0]['X-Client-Session-Id']);
  if (expectCalls > 1) assert.ok(!requests[1]['X-Client-Session-Id']);
  return { error, result };
}
const response = (status, body) => new Response(body, { status });
(async () => {
  for (const code of [400, 401, 403, 405]) {
    const { error } = await scenario([response(code, 'Normal validation/authentication failure')], 1, false);
    assert.equal(error?.status, code);
  }
  // An explicit header rejection remains retryable even for POST.
  const accepted = await scenario([response(400, 'X-Client-Session-Id header not supported'), response(200, 'OK')], 2, true);
  assert.equal(accepted.error, undefined);
  assert.equal(accepted.result?.status, 200);
  const rejected = await scenario([response(403, 'X-Client-Session-Id rejected'), response(403, 'Forbidden')], 2, false);
  assert.equal(rejected.error?.status, 403);

  // A network failure does not prove the first request was not processed.
  for (const method of ['POST', 'PUT', 'PATCH', 'DELETE']) {
    const { error } = await scenario([new Error('Connection lost'), response(200, 'Unexpected replay')], 1, false, method);
    assert.ok(error, `${method}: network failure must reach caller`);
    assert.equal(error.endpoint, '/api/v1/test');
    assert.match(error.message, /could not be reached/);
  }
  // Safe methods keep the proxy/header-compatibility fallback.
  for (const method of ['GET', 'HEAD', 'OPTIONS']) {
    const { error, result } = await scenario([new Error('CORS failure'), response(200, 'OK')], 2, true, method);
    assert.equal(error, undefined);
    assert.equal(result?.status, 200);
  }
  const failedGet = await scenario([new Error('Transient network error'), response(400, 'Invalid input')], 2, false, 'GET');
  assert.equal(failedGet.error?.status, 400);
  const disconnectedGet = await scenario([new Error('Disconnected'), new Error('Disconnected')], 2, false, 'GET');
  assert.ok(disconnectedGet.error);

  const controller = new AbortController();
  controller.abort();
  const abortError = new Error('Cancelled');
  const abortedGet = await scenario([abortError, response(200, 'Unexpected replay')], 1, false, 'GET', controller.signal);
  assert.equal(abortedGet.error, abortError, 'aborted requests must not be retried or wrapped');
  console.log('Inspect session header fallback regression checks passed.');
})().catch(err => { console.error(err); process.exitCode = 1; });
