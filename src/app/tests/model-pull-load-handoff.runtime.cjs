const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require('typescript');

const manager = fs.readFileSync(path.resolve(__dirname, '../src/components/ModelManager.tsx'), 'utf8');
const start = manager.indexOf('  const handleCancelPull = async (');
const end = manager.indexOf('  const handleRemotePull = async (', start);
assert.ok(start >= 0 && end > start, 'Expected the existing Get & Load handlers');

const source = `
function build(env: any) {
  const {
    api, pulling, setPulling, pullAbortRef, downloadStore, modelName,
    refresh, runLoadJob, activeLoadRef, loadingModelRef, setLoadNotice,
    showLoadError, setLoadError, friendlyErrorMessage, t, activeDownloadForModel,
  } = env;
  ${manager.slice(start, end)}
  return { handlePullAndLoad, handleCancelPull };
}
module.exports = build;
`;
const js = ts.transpileModule(source, {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
}).outputText;
const moduleStub = { exports: {} };
vm.runInNewContext(js, { module: moduleStub, exports: moduleStub.exports, AbortController, window: { setTimeout } });

async function scenario(cancelDuringRefresh) {
  const name = 'handoff-check';
  let releaseRefresh;
  let signalRefreshStarted;
  const refreshGate = new Promise(resolve => { releaseRefresh = resolve; });
  const refreshStarted = new Promise(resolve => { signalRefreshStarted = resolve; });
  const pullAbortRef = { current: {} };
  let pulling = {};
  const loads = [];
  let pullCalls = 0;
  const env = {
    pulling,
    setPulling(updater) { pulling = updater(pulling); },
    pullAbortRef,
    api: {
      async pullModel(_name, callbacks) {
        pullCalls++;
        callbacks.onComplete({ status: 'completed' }); // The API intentionally does NOT await this callback.
      },
      async controlDownload() {},
    },
    downloadStore: { wake: () => ({ percent: 100 }), snapshot: () => [], refresh: async () => {} },
    modelName: model => model.model_name,
    refresh: async () => { signalRefreshStarted(); await refreshGate; },
    runLoadJob: async model => { loads.push(model.model_name); },
    activeLoadRef: { current: null },
    loadingModelRef: { current: null },
    setLoadNotice() {},
    setLoadError() {},
    showLoadError: err => { throw err; },
    friendlyErrorMessage: err => String(err),
    activeDownloadForModel: () => null,
    t: key => key,
  };

  const handlers = moduleStub.exports(env);
  let settled = false;
  const task = handlers.handlePullAndLoad({ model_name: name }).then(() => { settled = true; });
  await refreshStarted;
  await new Promise(setImmediate);
  assert.equal(settled, false, 'Get & Load must wait for its asynchronous completion');
  assert.ok(pullAbortRef.current[name], 'Cancel must remain available while refresh is pending');
  assert.equal(pulling[name], 0, 'Cancel button must remain visible until handoff');
  if (cancelDuringRefresh) await handlers.handleCancelPull(name);
  releaseRefresh();
  await task;
  assert.deepEqual(loads, cancelDuringRefresh ? [] : [name]);
  assert.equal(pullCalls, 1);
  assert.equal(pullAbortRef.current[name], undefined, 'The controller must be cleaned up');
  assert.equal(pulling[name], undefined, 'The download indicator must be cleaned up');
}

(async () => {
  await scenario(true);
  await scenario(false);
  console.log('Get & Load completion/cancellation handoff checks passed.');
})().catch(err => { console.error(err); process.exitCode = 1; });
