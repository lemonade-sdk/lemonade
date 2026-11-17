const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require('typescript');

const filename = path.resolve(__dirname, '../src/components/BackendManager.tsx');
const source = fs.readFileSync(filename, 'utf8');
const compiled = ts.transpileModule(source, {
  compilerOptions: {
    target: ts.ScriptTarget.ES2022,
    module: ts.ModuleKind.ESNext,
    jsx: ts.JsxEmit.ReactJSX,
  },
  fileName: filename,
  reportDiagnostics: true,
});
const errors = (compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error);
assert.equal(errors.length, 0, errors.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'));

const installBlock = source.match(
  /const handleInstall = useCallback[\s\S]*?\n  const handleUninstall = useCallback/,
)?.[0];
assert.ok(installBlock, 'BackendManager handleInstall block not found');

// Job acceptance is not completion. Completion must come from the download store.
assert.match(
  installBlock,
  /const terminalDownloadPromise = waitForBackendDownloadTerminal[\s\S]*?await api\.installBackend\([\s\S]*?const terminalDownload = await terminalDownloadPromise/,
);
assert.match(installBlock, /void terminalDownloadPromise\.catch\(\(\) => undefined\)/);
assert.match(installBlock, /onComplete: \(\) => \{[\s\S]*?downloadStore\.wake\(downloadName, 'backend'\)/);
const onCompleteBlock = installBlock.match(
  /onComplete: \(\) => \{[\s\S]*?\n\s*\},\n\s*onError:/,
)?.[0] || '';
assert.doesNotMatch(onCompleteBlock, /status: 'completed'|still needs update/);

// installBackend reports errors through its callback; capture and rethrow them so
// a failure before the server registers a backend row cannot hang the UI.
assert.match(installBlock, /let installError: Error \| null = null/);
assert.match(installBlock, /onError: \(err\) => \{[\s\S]*?installError = err;[\s\S]*?downloadStore\.wake/);
assert.match(installBlock, /await api\.installBackend[\s\S]*?if \(installError\) throw installError/);
assert.match(installBlock, /terminalDownload\.status === 'error'[\s\S]*?throw new Error/);
assert.match(source, /BACKEND_DOWNLOAD_REGISTRATION_TIMEOUT_MS/);

// Both install and update use the same post-terminal status synchronization.
assert.match(installBlock, /const synced = await syncBackendStatus\(recipe, backend\)/);
assert.match(source, /BACKEND_STATUS_RETRY_DELAYS_MS/);
assert.match(source, /backendActionIsReflected/);

// Concurrent refreshes may not overwrite the newest system-info snapshot.
assert.match(source, /const requestId = \+\+systemInfoRequestRef\.current/);
assert.match(source, /requestId === systemInfoRequestRef\.current/);

// Regression: never tell the user to remove a binary based on a stale snapshot.
assert.doesNotMatch(source, /existing binary may need to be removed manually/);

function declarationText(name, kind) {
  const sourceFile = ts.createSourceFile(filename, source, ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
  let match;
  const visit = node => {
    const kindMatches = kind === 'function' ? ts.isFunctionDeclaration(node) : ts.isClassDeclaration(node);
    if (kindMatches && node.name?.text === name) match = node.getText(sourceFile);
    if (!match) ts.forEachChild(node, visit);
  };
  visit(sourceFile);
  assert.ok(match, `${kind} ${name} not found`);
  return match;
}

function buildRuntimeHelpers(downloadStore) {
  const helperSource = [
    'const BACKEND_DOWNLOAD_REGISTRATION_TIMEOUT_MS = 20;',
    declarationText('BackendDownloadMissingError', 'class'),
    declarationText('BackendDownloadRegistrationTimeoutError', 'class'),
    declarationText('backendActionIsReflected', 'function'),
    declarationText('waitForBackendDownloadTerminal', 'function'),
    'module.exports = { backendActionIsReflected, waitForBackendDownloadTerminal };',
  ].join('\n\n');
  const output = ts.transpileModule(helperSource, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2022,
      module: ts.ModuleKind.CommonJS,
    },
    reportDiagnostics: true,
  });
  const helperErrors = (output.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error);
  assert.equal(helperErrors.length, 0, helperErrors.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'));

  const module = { exports: {} };
  const context = {
    module,
    exports: module.exports,
    AbortController,
    Error,
    setTimeout,
    clearTimeout,
    downloadStore,
    cleanString: value => {
      const text = String(value || '').trim();
      return text && text.toLowerCase() !== 'unknown' ? text : '';
    },
    backendDownloadMatches: (download, recipe, backend) => (
      download.downloadType === 'backend'
      && (download.id === `backend:${recipe}:${backend}` || download.modelName === `${recipe}:${backend}`)
    ),
    isDownloadActive: download => download.running === true || download.status === 'downloading',
  };
  vm.runInNewContext(output.outputText, context, { filename: 'backend-manager-lifecycle-helpers.cjs' });
  return module.exports;
}

function createDownloadStore(initialItems) {
  let items = initialItems;
  const listeners = new Set();
  return {
    subscribe(listener) {
      listeners.add(listener);
      listener(items);
      return () => listeners.delete(listener);
    },
    set(nextItems) {
      items = nextItems;
      for (const listener of listeners) listener(items);
    },
  };
}

function backendItem(status, running) {
  return {
    id: 'backend:llamacpp:cpu',
    downloadType: 'backend',
    modelName: 'llamacpp:cpu',
    status,
    running,
  };
}

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((ok, fail) => { resolve = ok; reject = fail; });
  return { promise, resolve, reject };
}

function configFlow(api, loadSystemInfo = async () => ({})) {
  const parsed = ts.createSourceFile(filename, source, ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
  const callbacks = [];
  function visit(node) {
    if (ts.isVariableDeclaration(node) &&
        ['fetchInfo', 'handleFlmBuildChange'].includes(node.name.getText(parsed))) {
      callbacks.push(`const ${node.getText(parsed)};`);
    }
    ts.forEachChild(node, visit);
  }
  visit(parsed);
  const module = { exports: {} };
  let config = { flm: { runtime: 'xrt', args: '--pmode 1' } };
  const errors = [];
  vm.runInNewContext(ts.transpileModule(callbacks.join('\n') +
    '\nmodule.exports = { fetchInfo, handleFlmBuildChange };', {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  }).outputText, {
    module,
    api: { healthData: {}, ...api },
    loadSystemInfo,
    useCallback: fn => fn,
    runtimeConfigRequestRef: { current: 0 },
    setRuntimeConfig: value => { config = typeof value === 'function' ? value(config) : value; },
    backendRuntimeRecord: value => value && typeof value === 'object' ? value : null,
    setLoading: () => {},
    setError: error => { if (error) errors.push(error); },
    setSavingFlmBuild: () => {},
    friendlyErrorMessage: error => error.message,
    toast: error => errors.push(error),
  });
  return { ...module.exports, config: () => config, errors };
}

async function runConfigFlowChecks() {
  {
    const older = deferred();
    const newer = deferred();
    const responses = [older, newer];
    const flow = configFlow({ getRuntimeConfig: () => responses.shift().promise });
    const first = flow.fetchInfo(false);
    const second = flow.fetchInfo(false);
    newer.resolve({ flm: { runtime: 'hrx' } });
    await second;
    older.resolve({ flm: { runtime: 'xrt' } });
    await first;
    assert.equal(flow.config().flm.runtime, 'hrx', 'older config responses cannot replace newer ones');
  }
  {
    const oldRefresh = deferred();
    const duringSaveRefresh = deferred();
    const postSaveRefresh = deferred();
    const save = deferred();
    const responses = [oldRefresh, duringSaveRefresh, postSaveRefresh];
    const flow = configFlow({
      getRuntimeConfig: () => responses.shift().promise,
      setRuntimeConfig: () => save.promise,
    });
    const beforeSave = flow.fetchInfo(false);
    const saving = flow.handleFlmBuildChange('hrx');
    const duringSave = flow.fetchInfo(false);
    save.resolve({ updated: { flm: { runtime: 'hrx' } } });
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(flow.config().flm.runtime, 'hrx', 'the accepted runtime is applied before refresh completes');
    assert.equal(flow.config().flm.args, '--pmode 1', 'saving the runtime preserves other settings');
    oldRefresh.resolve({ flm: { runtime: 'xrt' } });
    duringSaveRefresh.resolve({ flm: { runtime: 'xrt' } });
    await Promise.all([beforeSave, duringSave]);
    assert.equal(flow.config().flm.runtime, 'hrx', 'responses from before save completion are invalidated');
    postSaveRefresh.resolve({ flm: { runtime: 'hrx', args: 'refreshed' } });
    await saving;
    assert.equal(flow.config().flm.args, 'refreshed', 'the post-save refresh can update configuration');
  }
  {
    const flow = configFlow({
      setRuntimeConfig: async () => ({}),
      getRuntimeConfig: async () => ({ flm: { runtime: 'hrx' } }),
    }, async () => { throw new Error('system-info unavailable'); });
    await flow.handleFlmBuildChange('hrx');
    assert.equal(flow.config().flm.runtime, 'hrx', 'refresh failure preserves the accepted selection');
    assert.deepEqual(flow.errors, ['system-info unavailable']);
  }
}

async function runBehaviorChecks() {
  await runConfigFlowChecks();
  {
    const store = createDownloadStore([backendItem('downloading', true)]);
    const { waitForBackendDownloadTerminal } = buildRuntimeHelpers(store);
    const controller = new AbortController();
    const terminal = waitForBackendDownloadTerminal('llamacpp', 'cpu', controller.signal);
    store.set([backendItem('completed', false)]);
    assert.equal((await terminal).status, 'completed');
  }

  {
    const store = createDownloadStore([backendItem('downloading', true)]);
    const { waitForBackendDownloadTerminal } = buildRuntimeHelpers(store);
    const controller = new AbortController();
    const terminal = waitForBackendDownloadTerminal('llamacpp', 'cpu', controller.signal);
    store.set([backendItem('error', false)]);
    assert.equal((await terminal).status, 'error');
  }

  {
    const store = createDownloadStore([backendItem('downloading', true)]);
    const { waitForBackendDownloadTerminal } = buildRuntimeHelpers(store);
    const controller = new AbortController();
    const terminal = waitForBackendDownloadTerminal('llamacpp', 'cpu', controller.signal);
    store.set([]);
    await assert.rejects(terminal, error => error?.name === 'BackendDownloadMissingError');
  }

  {
    const store = createDownloadStore([]);
    const { waitForBackendDownloadTerminal } = buildRuntimeHelpers(store);
    const controller = new AbortController();
    await assert.rejects(
      waitForBackendDownloadTerminal('llamacpp', 'cpu', controller.signal),
      error => error?.name === 'BackendDownloadRegistrationTimeoutError',
    );
  }

  {
    const store = createDownloadStore([backendItem('downloading', true)]);
    const { waitForBackendDownloadTerminal } = buildRuntimeHelpers(store);
    const controller = new AbortController();
    controller.abort(new Error('cancelled by test'));
    await assert.rejects(
      waitForBackendDownloadTerminal('llamacpp', 'cpu', controller.signal),
      /cancelled by test/,
    );
  }

  {
    const store = createDownloadStore([]);
    const { backendActionIsReflected } = buildRuntimeHelpers(store);
    assert.equal(backendActionIsReflected({ state: 'installed', version: '1.0' }, undefined), true);
    assert.equal(
      backendActionIsReflected(
        { state: 'update_available', version: '1.0' },
        { isUpdate: false, initialVersion: '' },
      ),
      true,
    );
    assert.equal(
      backendActionIsReflected(
        { state: 'update_available', version: '1.0' },
        { isUpdate: true, initialVersion: '1.0' },
      ),
      false,
    );
    assert.equal(
      backendActionIsReflected(
        { state: 'update_available', version: '1.1' },
        { isUpdate: true, initialVersion: '1.0' },
      ),
      true,
    );
  }
}

runBehaviorChecks()
  .then(() => console.log('Backend manager lifecycle regression checks passed.'))
  .catch(error => {
    console.error(error);
    process.exitCode = 1;
  });
