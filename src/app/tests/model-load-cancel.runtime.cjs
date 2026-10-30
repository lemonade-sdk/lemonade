const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const manager = fs.readFileSync(path.join(root, 'src/components/ModelManager.tsx'), 'utf8');
const detail = fs.readFileSync(path.join(root, 'src/components/ModelDetailPanel.tsx'), 'utf8');
const api = fs.readFileSync(path.join(root, 'src/api.ts'), 'utf8');
const css = fs.readFileSync(path.join(root, 'src/styles/styles.css'), 'utf8');

for (const filename of ['src/components/ModelManager.tsx', 'src/components/ModelDetailPanel.tsx', 'src/api.ts']) {
  const source = fs.readFileSync(path.join(root, filename), 'utf8');
  const result = ts.transpileModule(source, {
    fileName: filename,
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS, jsx: ts.JsxEmit.React },
    reportDiagnostics: true,
  });
  const errors = (result.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error);
  assert.equal(errors.length, 0, errors.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'));
}
assert.match(api, /async createModelLoadJob\(/);
assert.match(api, /_modelLoadRequestBody\(target\.name, target\.recipeOptions, target\.info\)/);
assert.match(api, /this\._assertModelNotDownloading\(target\.name\)/);
assert.match(api, /this\.createJob\(`Load \$\{modelName\}`, steps\)/);
assert.match(manager, /await loadModelRuntime\(componentInfo \|\| componentName, targets, visited\)/,
  'Omni children must become steps of the same cancellable load job');
assert.match(manager, /await api\.createModelLoadJob\(name, targets\)/);
assert.match(manager, /await api\.getJob\(id\)/);
assert.match(manager, /await api\.interruptJob\(active\.id\)/);
assert.match(manager, /if \(active\.cancelRequested\) await requestLoadInterrupt\(active\)/,
  'Cancel clicked during job creation must still cancel its created job');
assert.match(manager, /if \(job\.status === 'interrupted'\)/);
assert.match(manager, /if \(terminal && active\.id\) void api\.deleteJob\(active\.id\)/);
assert.match(manager, /if \(ac\.signal\.aborted\) return;/,
  'Cancelled Get & Load downloads must not start loading afterwards');
assert.match(manager, /if \(activeLoadRef\.current \|\| loadingModelRef\.current\) \{[\s\S]*?showLoadError\(name, new Error\(/,
  'Get & Load must explain if another operation prevents loading');
assert.match(manager, /if \(job\.status === 'failed'\) \{\s*terminal = true;\s*await refresh\(\)\.catch/,
  'Failed jobs must refresh the model list without hiding the load error');
assert.match(detail, /const displayLabel = estimate === null && progress\.stepStartedAt !== null/,
  'Loads without a size estimate must show elapsed time');
assert.match(detail, /activeLoad \? \(/);
assert.match(detail, /onClick=\{\(\) => onCancelLoad\(name\)\}/);
assert.match(detail, /role="progressbar"/);
assert.doesNotMatch(css, /@keyframes model-load-progress/);
assert.match(detail, /estimatedModelLoadPercent\(progress\.modelSizeGiB, elapsedMs\)/,
  'Models use the same estimated-load formula as Chat');
assert.match(detail, /<ModelLoadMeter name=\{name\} progress=\{activeLoad\} \/>/);
assert.match(manager, /activeStep: activeStep >= 0 \? activeStep : null/);
assert.match(manager, /stepStartedAt: activeStep < 0 \? null/);

// Exercise the actual TS load handlers without a browser or backend process.
const vm = require('node:vm');
const loaderStart = manager.indexOf('  const loadModelRuntime = async (');
const loaderEnd = manager.indexOf('  const handleUnload = async', loaderStart);
assert.ok(loaderStart >= 0 && loaderEnd > loaderStart);
const harnessSource = `
function build(env: any) {
  const {
    api, activeLoadRef, loadingModelRef, setLoadingModel, setLoadJob, setLoadError,
    downloadStore, modelName, isRouterModelInfo, findCurrentModel,
    isCollectionModel, getCollectionComponents, ensureServerRouterReady,
    customLoadOptions, activeDownloadForModel, friendlyErrorMessage,
    refresh, onModelSelect,
  } = env;
  ${manager.slice(loaderStart, loaderEnd)}
  return { handleLoad, handleCancelLoad };
}
module.exports = build;
`;
const harnessJs = ts.transpileModule(harnessSource, {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
}).outputText;
const harnessModule = { exports: {} };
vm.runInNewContext(harnessJs, {
  module: harnessModule,
  exports: harnessModule.exports,
  console,
  window: { setTimeout(fn) { setImmediate(fn); } },
});

async function scenario(cancelEarly, failedLoad = false) {
  let createResolve;
  const creation = new Promise(resolve => { createResolve = resolve; });
  let created = null;
  let interrupted = false;
  const calls = { interrupted: [], deleted: [], selected: [], refreshes: 0 };
  let loadJobState = null;
  let loadError = null;
  let polling = 0;
  const models = [
    { model_name: 'Omni', recipe: 'collection.omni', components: ['chat', 'image'] },
    { model_name: 'chat', recipe: 'llamacpp', downloaded: true },
    { model_name: 'image', recipe: 'sdcpp', downloaded: true },
  ];
  const apiStub = {
    async createModelLoadJob(name, targets) {
      created = { name, targets };
      if (cancelEarly) return creation;
      return { id: 'job-test' };
    },
    async interruptJob(id) { calls.interrupted.push(id); interrupted = true; },
    async getJob() {
      if (++polling > 12) throw new Error('load polling did not terminate');
      return {
        status: interrupted ? 'interrupted' : failedLoad ? 'failed' : 'completed',
        steps: created.targets.map((item, i) => ({
          status: interrupted ? 'pending' : failedLoad && i === 0 ? 'failed' : 'completed',
          error: failedLoad && i === 0 ? 'simulated load failure' : undefined,
        })),
      };
    },
    async deleteJob(id) { calls.deleted.push(id); },
    async systemInfo() { return {}; },
  };
  const env = {
    api: apiStub,
    activeLoadRef: { current: null },
    loadingModelRef: { current: null },
    setLoadingModel() {},
    setLoadJob(updater) { loadJobState = typeof updater === 'function' ? updater(loadJobState) : updater; },
    setLoadError(updater) { loadError = typeof updater === 'function' ? updater(loadError) : updater; },
    downloadStore: { snapshot: () => [], watchForModelLoad: () => () => {} },
    modelName: model => model?.model_name || '',
    isRouterModelInfo: model => model?.recipe === 'collection.router',
    findCurrentModel: name => models.find(model => model.model_name === name),
    isCollectionModel: model => model?.recipe === 'collection.omni',
    getCollectionComponents: model => model.components,
    ensureServerRouterReady: async () => {},
    customLoadOptions: () => undefined,
    activeDownloadForModel: () => null,
    friendlyErrorMessage: error => String(error),
    refresh: async () => { calls.refreshes++; },
    onModelSelect: name => calls.selected.push(name),
  };
  const handlers = harnessModule.exports(env);
  env.loadingModelRef.current = 'unloading';
  await handlers.handleLoad(models[0]);
  assert.equal(created, null, 'an active unload must block a new load');
  env.loadingModelRef.current = null;
  const load = handlers.handleLoad(models[0]);
  if (cancelEarly) {
    for (let tries = 0; !created && tries < 20; tries++) await new Promise(setImmediate);
    assert.ok(created, 'the Omni load must create a server job');
    assert.equal(loadJobState?.total, 2);
    handlers.handleCancelLoad('Omni');
    createResolve({ id: 'job-test' });
  }
  await load;
  if (!failedLoad) assert.equal(loadError, null);
  assert.deepEqual(Array.from(created.targets, target => target.name), ['chat', 'image']);
  return calls;
}

(async () => {
  const cancelled = await scenario(true);
  assert.deepEqual(cancelled.interrupted, ['job-test']);
  assert.equal(cancelled.selected.length, 0, 'a cancelled Omni job must not select a partially loaded collection');
  assert.deepEqual(cancelled.deleted, ['job-test']);
  const completed = await scenario(false);
  assert.equal(completed.interrupted.length, 0);
  assert.deepEqual(completed.selected, ['Omni']);
  assert.deepEqual(completed.deleted, ['job-test']);
  const failed = await scenario(false, true);
  assert.equal(failed.refreshes, 1, 'a failed job must refresh loaded-model state immediately');
  assert.equal(failed.selected.length, 0, 'a failed job must not select the model');
  console.log('Cancellable GUI load regression checks passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
