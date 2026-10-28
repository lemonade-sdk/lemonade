const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const apiPath = path.join(root, 'src/api.ts');
const storePath = path.join(root, 'src/features/models/modelSyncStore.ts');

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

const json = body => new Response(JSON.stringify(body), { status: 200, headers: { 'content-type': 'application/json' } });

let healthSync = { status: 'failed', sync_id: 3, completed_sync_id: 3 };
let modelsGeneration = 1;
let syncStatus = { status: 'failed', sync_id: 3, completed_sync_id: 3, failed_models: { 'old-model': 'old failure' } };
const counts = { status: 0, models: 0, start: 0 };

const originalFetch = global.fetch;
global.fetch = async (url, init = {}) => {
  const target = String(url);
  const method = String(init.method || 'GET').toUpperCase();
  if (target.includes('/api/v1/health')) {
    return json({ status: 'ok', version: 'test', all_models_loaded: [], max_models: {}, model_sync: healthSync, models_generation: modelsGeneration });
  }
  if (target.includes('/api/v1/models') && method === 'GET') {
    counts.models += 1;
    return json({ data: [{ id: 'alpha-model', downloaded: true }] });
  }
  if (target.endsWith('/internal/models/sync/status')) {
    counts.status += 1;
    return json(syncStatus);
  }
  if (target.endsWith('/internal/models/sync') && method === 'POST') {
    counts.start += 1;
    syncStatus = { status: 'in_progress', sync_id: 5, completed_sync_id: 4, progress: { current_model: 'alpha-model', percent: 10 } };
    healthSync = { status: 'in_progress', sync_id: 5, completed_sync_id: 4 };
    return json(syncStatus);
  }
  throw new Error(`Unexpected request: ${method} ${target}`);
};

async function waitFor(predicate, message, timeoutMs = 5000) {
  const started = Date.now();
  while (!predicate()) {
    if (Date.now() - started > timeoutMs) throw new Error(`Timed out: ${message}`);
    await new Promise(resolve => setTimeout(resolve, 10));
  }
}

(async () => {
  try {
    const { api } = require(apiPath);
    const { modelSyncStore, modelSyncFailureText, isModelSyncRunning } = require(storePath);
    await api.refresh();
    const unsubscribe = modelSyncStore.subscribe(() => {});

    // A failure that finished before this client connected is not news.
    await new Promise(resolve => setTimeout(resolve, 50));
    assert.equal(modelSyncStore.snapshot().result, null, 'an old result must not surface on connect');
    assert.equal(counts.status, 0, 'an idle server must not be polled for sync details');

    // A sync another client starts is picked up from health alone.
    healthSync = { status: 'in_progress', sync_id: 4, completed_sync_id: 3 };
    syncStatus = { status: 'in_progress', sync_id: 4, completed_sync_id: 3, progress: { current_model: 'beta-model', percent: 55 } };
    await api.health();
    await waitFor(() => modelSyncStore.snapshot().active?.progress?.current_model === 'beta-model', 'external sync becomes active');
    assert.equal(isModelSyncRunning(modelSyncStore.snapshot()), true);

    // Its failure is reported once it finishes.
    syncStatus = { status: 'failed', sync_id: 4, completed_sync_id: 4, failed_models: { 'beta-model': 'Hugging Face repository not found' } };
    healthSync = { status: 'failed', sync_id: 4, completed_sync_id: 4 };
    await waitFor(() => modelSyncStore.snapshot().result?.status === 'failed', 'external failure becomes the result');
    assert.match(modelSyncFailureText(modelSyncStore.snapshot().result), /beta-model: Hugging Face repository not found/);
    assert.equal(modelSyncStore.snapshot().active, null);

    // A registry change on the server makes the client read the model list again.
    const modelReads = counts.models;
    modelsGeneration += 1;
    await api.health();
    await waitFor(() => counts.models === modelReads + 1, 'models generation change refreshes models');
    await api.health();
    await new Promise(resolve => setTimeout(resolve, 50));
    assert.equal(counts.models, modelReads + 1, 'an unchanged generation must not refetch models');

    // A sync started here is followed to its end, and a success is not a failure.
    modelSyncStore.dismissResult();
    await modelSyncStore.startSync();
    assert.equal(counts.start, 1);
    assert.equal(modelSyncStore.snapshot().active?.status, 'in_progress');
    syncStatus = { status: 'success', sync_id: 5, completed_sync_id: 5, failed_models: {} };
    healthSync = { status: 'success', sync_id: 5, completed_sync_id: 5 };
    await waitFor(() => modelSyncStore.snapshot().result?.status === 'success', 'own sync ends as success');
    assert.equal(modelSyncFailureText(modelSyncStore.snapshot().result), null, 'a success is not a failure');
    assert.equal(isModelSyncRunning(modelSyncStore.snapshot()), false);

    unsubscribe();
    console.log('Model sync store checks passed.');
  } finally {
    global.fetch = originalFetch;
    if (originalTsLoader) require.extensions['.ts'] = originalTsLoader;
    else delete require.extensions['.ts'];
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
