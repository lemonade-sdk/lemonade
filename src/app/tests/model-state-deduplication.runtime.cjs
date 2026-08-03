const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const apiPath = path.join(root, 'src/api.ts');
const appPath = path.join(root, 'src/App.tsx');
const managerPath = path.join(root, 'src/components/ModelManager.tsx');
const hookPath = path.join(root, 'src/features/models/modelState.ts');

for (const file of [apiPath, appPath, managerPath, hookPath]) {
  const source = fs.readFileSync(file, 'utf8');
  const compiled = ts.transpileModule(source, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.ESNext,
      jsx: ts.JsxEmit.ReactJSX,
    },
    fileName: file,
    reportDiagnostics: true,
  });
  const errors = (compiled.diagnostics || []).filter(
    diagnostic => diagnostic.category === ts.DiagnosticCategory.Error,
  );
  assert.equal(
    errors.length,
    0,
    errors.map(diagnostic => ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n')).join('\n'),
  );
}

const apiSource = fs.readFileSync(apiPath, 'utf8');
const appSource = fs.readFileSync(appPath, 'utf8');
const managerSource = fs.readFileSync(managerPath, 'utf8');
const hookSource = fs.readFileSync(hookPath, 'utf8');

assert.match(apiSource, /private _modelsRequestInFlight = new Map<boolean, Promise<ModelsData>>\(\)/);
assert.match(apiSource, /private _modelsFetchRevision = 0/);
assert.match(apiSource, /private _healthDataSignature = ''/);
assert.match(apiSource, /private _modelsDataSignature = ''/);
assert.match(apiSource, /private _refreshInFlight: Promise<\{ health: HealthData; models: ModelsData \} \| null> \| null/);
assert.match(apiSource, /if \(this\._connectInFlight\) return this\._connectInFlight/);
assert.match(apiSource, /const existing = this\._modelsRequestInFlight\.get\(showAll\)/);
assert.match(apiSource, /get modelStateSnapshot\(\): ModelStateSnapshot/);
assert.match(apiSource, /onModelStateChange\(fn: ModelStateListener\)/);
assert.match(apiSource, /if \(this\.isConnected\) void this\.refresh\(\)/);
assert.match(apiSource, /async ensureModelState\(\)/);

assert.match(hookSource, /useSyncExternalStore\(subscribe, getSnapshot, getSnapshot\)/);
assert.match(appSource, /const serverModelState = useServerModelState\(\)/);
assert.match(appSource, /void api\.connect\(\)/);
assert.match(appSource, /const rawLoadedModels = serverModelState\.health\?\.all_models_loaded/);
assert.doesNotMatch(appSource, /useState<LoadedModel\[]>/);
assert.doesNotMatch(appSource, /api\.connect\(\)\.then\([\s\S]*api\.refresh\(/);
assert.doesNotMatch(appSource, /api\.onModelsChanged\(/);

assert.match(managerSource, /const serverModelState = useServerModelState\(\)/);
assert.match(managerSource, /const models = serverModelState\.models\?\.data/);
assert.match(managerSource, /const loadedModels = serverModelState\.health\?\.all_models_loaded/);
assert.doesNotMatch(managerSource, /useState<ModelInfo\[]>\(\[\]\)/);
assert.doesNotMatch(managerSource, /useState<LoadedModel\[]>\(\[\]\)/);
assert.doesNotMatch(managerSource, /api\.onStatusChange\(/);
assert.doesNotMatch(managerSource, /api\.onModelsChanged\(/);

const originalTsLoader = require.extensions['.ts'];
require.extensions['.ts'] = function transpileTs(module, filename) {
  const source = fs.readFileSync(filename, 'utf8');
  const output = ts.transpileModule(source, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS,
      esModuleInterop: true,
    },
    fileName: filename,
  }).outputText;
  module._compile(output, filename);
};

const originalFetch = global.fetch;
let healthRequests = 0;
let modelRequests = 0;
let deleteRequests = 0;

global.fetch = async (url, init = {}) => {
  const target = String(url);
  await new Promise(resolve => setTimeout(resolve, 15));

  if (target.includes('/api/v1/health')) {
    healthRequests += 1;
    return new Response(JSON.stringify({
      status: 'ok',
      version: 'test',
      all_models_loaded: [],
      max_models: {},
    }), { status: 200, headers: { 'content-type': 'application/json' } });
  }

  if (target.includes('/api/v1/models')) {
    modelRequests += 1;
    return new Response(JSON.stringify({
      data: [{ id: 'test-model', model_name: 'test-model', downloaded: true }],
    }), { status: 200, headers: { 'content-type': 'application/json' } });
  }

  if (target.endsWith('/api/v1/delete') && String(init.method || 'GET').toUpperCase() === 'POST') {
    deleteRequests += 1;
    return new Response(JSON.stringify({ ok: true }), {
      status: 200,
      headers: { 'content-type': 'application/json' },
    });
  }

  throw new Error(`Unexpected request: ${String(init.method || 'GET')} ${target}`);
};

async function waitFor(predicate, timeoutMs = 1500) {
  const started = Date.now();
  while (!predicate()) {
    if (Date.now() - started > timeoutMs) throw new Error('Timed out waiting for model state refresh.');
    await new Promise(resolve => setTimeout(resolve, 10));
  }
}

(async () => {
  try {
    const { api } = require(apiPath);
    let notifications = 0;
    const unsubscribe = api.onModelStateChange(() => { notifications += 1; });

    const startupResults = await Promise.all([
      api.connect(),
      api.connect(),
      api.refresh(),
      api.refresh(),
      api.models(true),
    ]);

    assert.deepEqual(startupResults.map(Boolean), [true, true, true, true, true]);
    assert.equal(healthRequests, 1, 'concurrent startup callers must share one health request');
    assert.equal(modelRequests, 1, 'concurrent startup callers must share one /models request');
    assert.equal(api.modelStateSnapshot.status, 'connected');
    assert.equal(api.modelStateSnapshot.refreshing, false);
    assert.equal(api.modelStateSnapshot.models.data.length, 1);

    const stableNotificationCount = notifications;
    await api.health();
    assert.equal(healthRequests, 2, 'an explicit stable health read still performs one request');
    assert.equal(notifications, stableNotificationCount, 'unchanged health data must retain snapshot identity');

    await Promise.all([api.models(true), api.models(true), api.models(true)]);
    assert.equal(modelRequests, 2, 'direct concurrent model reads must be coalesced');
    assert.equal(notifications, stableNotificationCount, 'unchanged model data must not republish global state');

    await api.deleteModel('test-model');
    assert.equal(deleteRequests, 1);
    await waitFor(() => modelRequests === 3 && api.modelStateSnapshot.refreshing === false);
    assert.equal(healthRequests, 3, 'a mutation must trigger one global state refresh');
    assert.equal(modelRequests, 3, 'a mutation must trigger one registry refresh');

    const backgroundRefresh = api.refresh();
    await new Promise(resolve => setTimeout(resolve, 5));
    await api.deleteModel('test-model');
    const joinedMutationRefresh = api.refresh();
    await Promise.all([backgroundRefresh, joinedMutationRefresh]);
    await waitFor(() => modelRequests === 5 && api.modelStateSnapshot.refreshing === false);
    assert.equal(deleteRequests, 2);
    assert.equal(healthRequests, 5, 'a mutation during refresh must queue exactly one follow-up health request');
    assert.equal(modelRequests, 5, 'a mutation during refresh must queue exactly one follow-up registry request');
    assert.ok(notifications > 0, 'subscribers must receive canonical model-state updates');

    unsubscribe();
    console.log('Model state deduplication checks passed.');
  } finally {
    global.fetch = originalFetch;
    if (originalTsLoader) require.extensions['.ts'] = originalTsLoader;
    else delete require.extensions['.ts'];
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
