const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { webcrypto } = require('node:crypto');
const ts = require('typescript');

// All requests execute through GUI3's actual src/api.ts; only HTTP is mocked.
const appRoot = path.resolve(__dirname, '..');
const apiPath = path.join(appRoot, 'src/api.ts');
const configurationPath = path.join(appRoot, 'src/modelConfiguration.ts');
const originalLoader = require.extensions['.ts'];
const originalFetch = global.fetch;
const originalCrypto = global.crypto;
const originalConfigCache = require.cache[configurationPath];
const originalApiCache = require.cache[apiPath];
if (!global.crypto) global.crypto = webcrypto;
require.extensions['.ts'] = (module, filename) => {
  const input = fs.readFileSync(filename, 'utf8');
  const output = ts.transpileModule(input, {
    fileName: filename,
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS,
      esModuleInterop: true,
    },
  });
  const errors = (output.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error);
  assert.equal(errors.length, 0, errors.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'));
  module._compile(output.outputText, filename);
};
// Loading a real model is not needed: isolate only option preparation, not API calls.
require.cache[configurationPath] = {
  id: configurationPath,
  filename: configurationPath,
  loaded: true,
  exports: { recipeOptionsForModel: () => ({ ctx_size: 4096 }) },
  children: [],
  paths: [],
};

let requests = [];
let rows = [{ id: 'BuiltIn-Compat', model_name: 'BuiltIn-Compat',
  recipe: 'llamacpp', checkpoint: 'org/model:Q4_K_M', downloaded: true }];
const savedOptions = new Map();
let downloads = [];
let downloadsShape = 'object';
let modelListGate = null;
let pullResponse = { status: 'completed', complete: true, running: false };
let pullContentType = 'application/json';
let pullSse = '';

function respond(data, status = 200, contentType = 'application/json') {
  return new Response(contentType.includes('event-stream') ? data : JSON.stringify(data), {
    status,
    headers: { 'content-type': contentType },
  });
}
function recorded(method, route) {
  return requests.filter(r => r.method === method && r.path === route);
}
function latest(method, route) {
  const matches = recorded(method, route);
  assert.ok(matches.length, `Expected ${method} ${route}. Received: ${requests.map(r => `${r.method} ${r.path}`).join(', ')}`);
  return matches.at(-1);
}
const endpointModel = (url, suffix = '') => {
  const prefix = '/api/v1/models/';
  assert.ok(url.pathname.startsWith(prefix), url.pathname);
  let name = url.pathname.slice(prefix.length);
  if (suffix) {
    assert.ok(name.endsWith(suffix), name);
    name = name.slice(0, -suffix.length);
  }
  return decodeURIComponent(name);
};

global.fetch = async (uri, init = {}) => {
  const url = new URL(String(uri));
  const method = (init.method || 'GET').toUpperCase();
  const body = typeof init.body === 'string' ? JSON.parse(init.body) : null;
  const call = { method, path: url.pathname, search: url.searchParams,
    rawUrl: url.href, body, headers: init.headers || {}, init };
  requests.push(call);

  if (url.pathname === '/api/v1/health' && method === 'GET') {
    return respond({ status: 'ok', version: 'compat', all_models_loaded: [], max_models: {} });
  }
  if (url.pathname === '/api/v1/models' && method === 'GET') {
    const snapshot = rows.map(row => ({ ...row }));
    if (modelListGate) {
      const gate = modelListGate;
      modelListGate = null;
      gate.started();
      await gate.wait;
    }
    return respond({ data: snapshot });
  }
  if (url.pathname === '/api/v1/downloads' && method === 'GET') {
    return respond(downloadsShape === 'array' ? downloads : { downloads });
  }
  if (url.pathname === '/api/v1/models/register' && method === 'POST') {
    rows = rows.concat([{ id: body.model_name.replace(/^user\./, ''),
      model_name: body.model_name, checkpoint: body.checkpoint || '',
      recipe: body.recipe || '', downloaded: false }]);
    return respond({ status: 'success', model_name: body.model_name.replace(/^user\./, ''),
      canonical_model_name: body.model_name });
  }
  if (url.pathname === '/api/v1/delete' && method === 'POST') {
    rows = rows.filter(row => row.model_name !== body.model_name && row.id !== body.model_name);
    return respond({ status: 'success' });
  }
  if (url.pathname === '/api/v1/unload' && method === 'POST') return respond({ status: 'unloaded' });
  if (url.pathname === '/api/v1/load' && method === 'POST') return respond({ status: 'loaded' });
  if (url.pathname === '/internal/pin' && method === 'POST') {
    return respond({ status: 'success', model_name: body.model_name, pinned: body.pinned });
  }
  if (url.pathname === '/api/v1/downloads/control' && method === 'POST') return respond({ ok: true });
  if (url.pathname === '/api/v1/registry/search' && method === 'GET') return respond({ results: [] });
  if (url.pathname === '/api/v1/pull/variants' && method === 'GET') return respond({ variants: [], mmproj_files: [] });
  if (url.pathname === '/api/v1/pull' && method === 'POST') {
    if (pullContentType === 'text/event-stream') return respond(pullSse, 200, pullContentType);
    if (pullResponse.error) return respond({ error: pullResponse.error }, 404);
    return respond(pullResponse);
  }
  if (url.pathname.startsWith('/api/v1/models/') && url.pathname.endsWith('/options')) {
    const name = endpointModel(url, '/options');
    if (method === 'POST') savedOptions.set(name, { ...savedOptions.get(name), ...body });
    if (method === 'DELETE') savedOptions.delete(name);
    return respond({ model_name: name, recipe: 'llamacpp', saved: savedOptions.get(name) || {},
      effective: { ctx_size: 4096, ...(savedOptions.get(name) || {}), model_name: name },
      defaults: { ctx_size: 4096 }, resolved_ctx_size: 4096, load_command: '' });
  }
  if (url.pathname.startsWith('/api/v1/models/') && url.pathname.endsWith('/files')) {
    const name = endpointModel(url, '/files');
    if (name === 'missing') return respond({ error: 'not found' }, 404);
    return respond({ model_id: name, files: [{ name: 'main.gguf', role: 'main', size_bytes: 100, exists: true }] });
  }
  if (url.pathname.startsWith('/api/v1/models/') && method === 'GET') {
    const name = endpointModel(url);
    if (name === 'missing') return respond({ error: 'not found' }, 404);
    return respond(rows.find(row => row.model_name === name || row.id === name) ||
      { id: name, model_name: name, recipe: 'llamacpp' });
  }
  throw new Error(`Unmocked request: ${method} ${url.href}`);
};

let passed = 0;
async function check(name, fn) {
  try {
    await fn();
    passed += 1;
    process.stdout.write(`PASS ${name}\n`);
  } catch (error) {
    error.message = `${name}: ${error.message}`;
    throw error;
  }
}

(async () => {
  const { api } = require(apiPath);
  api.baseUrl = 'http://127.0.0.1:13305';
  api.setSessionApiKey('api-secret');
  api.setSessionAdminApiKey('admin-secret');
  await check('concurrent GUI3 model lists share one HTTP request', async () => {
    const before = recorded('GET', '/api/v1/models').length;
    const data = await Promise.all([api.models(true), api.models(true), api.models(true)]);
    assert.equal(recorded('GET', '/api/v1/models').length - before, 1);
    assert.equal(data[0].data[0].id, 'BuiltIn-Compat');
    assert.ok(latest('GET', '/api/v1/models').search.has('show_all'));
  });
  await check('downloaded-only listing uses distinct query', async () => {
    await api.models(false);
    assert.equal(latest('GET', '/api/v1/models').search.has('show_all'), false);
  });
  await check('GUI3 preserves model metadata', async () => {
    rows.push({ id: 'user-custom', recipe: 'llamacpp', checkpoint: 'org/r:Q4',
      downloaded: false, source: 'modelscope', labels: ['vision', 'custom'],
      registry_source: 'modelscope', checkpoint_status: [{ type: 'main', present: false }] });
    const result = await api.models(true);
    const row = result.data.find(item => item.id === 'user-custom');
    assert.equal(row.registry_source, 'modelscope');
    assert.equal(row.custom, true);
    assert.deepEqual(row.checkpoint_status, [{ type: 'main', present: false }]);
  });
  await check('GET model detail URL-encodes reserved characters', async () => {
    const name = 'group/model+ Q4';
    const result = await api.modelDetail(name);
    assert.equal(result.id, name);
    assert.match(latest('GET', '/api/v1/models/group%2Fmodel%2B%20Q4').rawUrl, /%2F/);
  });
  await check('GUI3 files tab accepts model files shape', async () => {
    const files = await api.getModelFiles('org/name');
    assert.equal(files.model_id, 'org/name');
    assert.deepEqual(files.files[0], { name: 'main.gguf', role: 'main', size_bytes: 100, exists: true });
  });
  await check('missing model files return null, not an uncaught error', async () => {
    assert.equal(await api.getModelFiles('missing'), null);
  });
  await check('options GET addresses the selected model', async () => {
    const data = await api.getModelOptions('with space');
    assert.equal(data.model_name, 'with space');
    assert.deepEqual(data.saved, {});
  });
  await check('options POST sends JSON changes without a model load', async () => {
    const before = recorded('POST', '/api/v1/load').length;
    const data = await api.saveModelOptions('BuiltIn-Compat', { ctx_size: 8192 });
    assert.equal(data.saved.ctx_size, 8192);
    assert.equal(latest('POST', '/api/v1/models/BuiltIn-Compat/options').body.ctx_size, 8192);
    assert.equal(recorded('POST', '/api/v1/load').length, before);
  });
  await check('options saved state round-trips through GET', async () => {
    assert.equal((await api.getModelOptions('BuiltIn-Compat')).saved.ctx_size, 8192);
  });
  await check('options DELETE resets stored overrides', async () => {
    assert.deepEqual((await api.resetModelOptions('BuiltIn-Compat')).saved, {});
  });
  await check('register model keeps caller ID authoritative', async () => {
    const result = await api.registerModelDefinition('user.GUI3Compat', {
      model_name: 'should-not-win', checkpoint: 'org/r:Q4', recipe: 'llamacpp', labels: ['chat'],
    });
    assert.equal(result.canonical_model_name, 'user.GUI3Compat');
    assert.equal(latest('POST', '/api/v1/models/register').body.model_name, 'user.GUI3Compat');
    assert.equal((await api.models(true)).data.some(x => x.model_name === 'user.GUI3Compat'), true);
  });
  await check('delete model POST uses the server-defined model name', async () => {
    await api.deleteModel('user.GUI3Compat');
    assert.equal(latest('POST', '/api/v1/delete').body.model_name, 'user.GUI3Compat');
  });
  await check('unload specific model uses POST and model_name', async () => {
    await api.unloadModel('BuiltIn-Compat');
    assert.equal(latest('POST', '/api/v1/unload').body.model_name, 'BuiltIn-Compat');
  });
  await check('unload all uses empty body', async () => {
    await api.unloadModel();
    assert.deepEqual(latest('POST', '/api/v1/unload').body, {});
  });
  await check('model pinning preserves requested boolean', async () => {
    assert.equal((await api.setModelPinned('BuiltIn-Compat', true)).pinned, true);
    assert.deepEqual(latest('POST', '/internal/pin').body, { model_name: 'BuiltIn-Compat', pinned: true });
  });
  await check('search builds URL-encoded provider query without HF fallback', async () => {
    await api.registrySearch('modelscope', 'Qwen vision + tools', 7);
    const route = latest('GET', '/api/v1/registry/search').search;
    assert.equal(route.get('source'), 'modelscope');
    assert.equal(route.get('query'), 'Qwen vision + tools');
    assert.equal(route.get('limit'), '7');
    assert.equal(route.get('format'), 'gguf');
  });
  await check('variant lookup preserves source and entire checkpoint', async () => {
    await api.pullVariants('org/my model:Q4_K_M', 'modelscope');
    const route = latest('GET', '/api/v1/pull/variants').search;
    assert.equal(route.get('checkpoint'), 'org/my model:Q4_K_M');
    assert.equal(route.get('source'), 'modelscope');
  });
  await check('downloads accepts wrapped server response', async () => {
    downloadsShape = 'object';
    downloads = [{ id: 'model:sample', type: 'model', status: 'downloading', running: true }];
    assert.equal((await api.downloads())[0].id, 'model:sample');
  });
  await check('downloads accepts historical array response', async () => {
    downloadsShape = 'array';
    assert.equal((await api.downloads()).length, 1);
    downloadsShape = 'object';
  });
  await check('download control retains action and stable id', async () => {
    await api.controlDownload('model:sample', 'pause');
    assert.deepEqual(latest('POST', '/api/v1/downloads/control').body,
      { id: 'model:sample', action: 'pause' });
  });
  await check('active model download blocks load instead of hanging', async () => {
    downloads = [{ id: 'model:BuiltIn-Compat', type: 'model', model_name: 'BuiltIn-Compat',
      status: 'downloading', running: true }];
    const count = recorded('POST', '/api/v1/load').length;
    await assert.rejects(api.loadModel('BuiltIn-Compat'), /still downloading/);
    assert.equal(recorded('POST', '/api/v1/load').length, count);
  });
  await check('paused model download remains blocking', async () => {
    downloads = [{ id: 'model:BuiltIn-Compat', type: 'model', status: 'paused', running: false }];
    await assert.rejects(api.loadModel('BuiltIn-Compat'), /still downloading/);
  });
  await check('backend download does not block loading an unrelated model', async () => {
    downloads = [{ id: 'backend:BuiltIn-Compat', type: 'backend', status: 'downloading', running: true }];
    await api.loadModel('BuiltIn-Compat', { ctx_size: 8192 }, rows[0]);
    const body = latest('POST', '/api/v1/load').body;
    assert.equal(body.model_name, 'BuiltIn-Compat');
    assert.equal(body.ctx_size, 8192);
    assert.equal(body.save_options, false);
  });
  await check('completed download may be loaded', async () => {
    downloads = [{ id: 'model:BuiltIn-Compat', type: 'model', status: 'completed',
      complete: true, running: false }];
    await api.loadModel('BuiltIn-Compat', undefined, rows[0]);
  });
  await check('loading a router definition remains virtual', async () => {
    const count = recorded('POST', '/api/v1/load').length;
    const data = await api.loadModel('Router', undefined, { recipe: 'collection.router' });
    assert.equal(data.virtual, true);
    assert.equal(recorded('POST', '/api/v1/load').length, count);
  });
  await check('pull uses non-subscribing server download payload', async () => {
    downloads = [];
    pullContentType = 'application/json';
    pullResponse = { status: 'completed', complete: true, running: false };
    let completed = 0;
    await api.pullModel('PullModel', { onComplete: () => { completed += 1; } },
      { source: 'modelscope', checkpoint: 'org/repo:Q4', recipe: 'llamacpp' });
    const body = latest('POST', '/api/v1/pull').body;
    assert.equal(body.stream, true);
    assert.equal(body.subscribe, false);
    assert.equal(body.source, 'modelscope');
    assert.equal(body.model_name, 'PullModel');
    assert.equal(completed, 1);
  });
  await check('pull rejects explicit HTTP errors, never reports completion', async () => {
    pullResponse = { error: 'No such model' };
    let completed = 0;
    let receivedError = null;
    await api.pullModel('NotFound', { onComplete: () => { completed += 1; },
      onError: e => { receivedError = e; } });
    assert.equal(completed, 0);
    assert.match(receivedError.message, /404|No such model/);
  });
  await check('SSE complete waits for authoritative /downloads result', async () => {
    pullResponse = {};
    pullContentType = 'text/event-stream';
    pullSse = 'event: progress\ndata: {"percent": 20}\n\n' +
      'event: complete\ndata: {"status":"completed","complete":true,"running":false}\n\n';
    downloads = [{ id: 'model:SseModel', type: 'model', model_name: 'SseModel',
      status: 'completed', complete: true, running: false }];
    let completed = 0;
    const progress = [];
    await api.pullModel('SseModel', { onProgress: x => progress.push(x),
      onComplete: () => { completed += 1; } });
    assert.equal(completed, 1);
    assert.ok(progress.length >= 2);
    assert.ok(recorded('GET', '/api/v1/downloads').length);
  });
  await check('SSE error cannot trigger a successful completion', async () => {
    pullSse = 'event: error\ndata: {"error":"Registry unreachable"}\n\n';
    let completed = 0;
    let error = null;
    await api.pullModel('SseError', { onComplete: () => { completed += 1; },
      onError: e => { error = e; } });
    assert.equal(completed, 0);
    assert.match(error.message, /Registry unreachable/);
  });
  console.log(`GUI3 actual API client: ${passed} compatibility checks passed.`);
})().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exitCode = 1;
}).finally(() => {
  delete require.cache[apiPath];
  if (originalApiCache) require.cache[apiPath] = originalApiCache;
  if (originalConfigCache) require.cache[configurationPath] = originalConfigCache;
  else delete require.cache[configurationPath];
  if (originalLoader) require.extensions['.ts'] = originalLoader;
  else delete require.extensions['.ts'];
  global.fetch = originalFetch;
  if (originalCrypto === undefined) delete global.crypto;
  else global.crypto = originalCrypto;
});
