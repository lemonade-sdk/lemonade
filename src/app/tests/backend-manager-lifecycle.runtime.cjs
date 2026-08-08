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
assert.match(installBlock, /onComplete: \(\) => \{[\s\S]*?downloadStore\.refresh\(\)/);
const onCompleteBlock = installBlock.match(
  /onComplete: \(\) => \{[\s\S]*?\n\s*\},\n\s*onError:/,
)?.[0] || '';
assert.doesNotMatch(onCompleteBlock, /status: 'completed'|still needs update/);

// An API error must become a truly terminal error and can never fall through to success.
assert.match(installBlock, /onError: \(err\) => \{[\s\S]*?status: 'error',[\s\S]*?running: false/);
assert.match(installBlock, /terminalDownload\.status === 'error'[\s\S]*?throw new Error/);

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
    declarationText('BackendDownloadMissingError', 'class'),
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

async function runBehaviorChecks() {
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
