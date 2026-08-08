const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const storePath = path.join(root, 'src/features/downloadManager/downloadStore.ts');
const apiPath = path.join(root, 'src/api.ts');
const source = fs.readFileSync(storePath, 'utf8');

const compiled = ts.transpileModule(source, {
  compilerOptions: {
    target: ts.ScriptTarget.ES2020,
    module: ts.ModuleKind.CommonJS,
    esModuleInterop: true,
  },
  fileName: storePath,
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

assert.match(source, /private hasActiveDownloads\(\): boolean/);
assert.match(source, /if \(!this\.started \|\| !this\.hasActiveDownloads\(\)\) \{/);
assert.match(source, /this\.clearPollTimer\(\);[\s\S]*return;/);
assert.doesNotMatch(source, /finally \{[\s\S]{0,120}this\.scheduleNext\(\)/);
assert.doesNotMatch(
  source,
  /if \(!api\.isConnected\)/,
  'an authoritative /downloads refresh must not depend on the global connection flag',
);

const originalTsLoader = require.extensions['.ts'];
const originalApiCache = require.cache[apiPath];
const originalWindow = global.window;
const originalLocalStorage = global.localStorage;

require.extensions['.ts'] = function transpileTs(module, filename) {
  const input = fs.readFileSync(filename, 'utf8');
  const output = ts.transpileModule(input, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS,
      esModuleInterop: true,
    },
    fileName: filename,
  }).outputText;
  module._compile(output, filename);
};

const storage = new Map();
global.localStorage = {
  getItem(key) { return storage.has(key) ? storage.get(key) : null; },
  setItem(key, value) { storage.set(key, String(value)); },
  removeItem(key) { storage.delete(key); },
};

const windowListeners = new Map();
global.window = {
  addEventListener(type, listener) {
    const listeners = windowListeners.get(type) || [];
    listeners.push(listener);
    windowListeners.set(type, listeners);
  },
};

let downloadRequests = 0;
let serverDownloads = [];
const apiStub = {
  // The download endpoint must still be queried while the broader app
  // connection handshake is settling.
  isConnected: false,
  async downloads() {
    downloadRequests += 1;
    await new Promise(resolve => setTimeout(resolve, 5));
    return serverDownloads;
  },
};

require.cache[apiPath] = {
  id: apiPath,
  filename: apiPath,
  loaded: true,
  exports: { __esModule: true, default: apiStub, api: apiStub },
  children: [],
  paths: [],
};

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function waitFor(predicate, timeoutMs = 1500) {
  const started = Date.now();
  while (!predicate()) {
    if (Date.now() - started > timeoutMs) throw new Error('Timed out waiting for download-store state.');
    await sleep(10);
  }
}

(async () => {
  try {
    const { downloadStore } = require(storePath);
    const unsubscribe = downloadStore.subscribe(() => {});

    await waitFor(() => downloadRequests === 1);
    assert.equal(
      downloadRequests,
      1,
      'startup must query the authoritative snapshot before api.isConnected settles',
    );
    await sleep(1150);
    assert.equal(
      downloadRequests,
      1,
      'an empty authoritative startup snapshot must not start perpetual /downloads polling',
    );

    await downloadStore.refresh();
    assert.equal(downloadRequests, 2, 'an explicit idle refresh must perform exactly one request');
    await sleep(1150);
    assert.equal(downloadRequests, 2, 'an explicit idle refresh must remain one-shot');

    serverDownloads = [{
      id: 'model:test-model',
      type: 'model',
      model_name: 'test-model',
      status: 'downloading',
      running: true,
      percent: 10,
    }];
    downloadStore.markLocal('test-model', 'downloading', 'model');
    await waitFor(() => downloadRequests === 3, 1600);
    assert.ok(
      downloadStore.snapshot().some(item => item.modelName === 'test-model' && item.running === true),
      'an active download must remain represented after polling',
    );

    downloadStore.upsertFromPull('test-model', {
      id: 'model:test-model',
      status: 'completed',
      complete: true,
      running: false,
      percent: 100,
    }, 'model');
    serverDownloads = [];
    const requestsAtCompletion = downloadRequests;
    await sleep(1200);
    assert.equal(
      downloadRequests,
      requestsAtCompletion,
      'the last terminal download event must cancel the already-scheduled next poll',
    );

    const focusListeners = windowListeners.get('focus') || [];
    assert.equal(focusListeners.length, 1);
    focusListeners[0]();
    await waitFor(() => downloadRequests === requestsAtCompletion + 1);
    await sleep(1150);
    assert.equal(
      downloadRequests,
      requestsAtCompletion + 1,
      'focus may refresh idle state once but must not restart perpetual polling',
    );

    unsubscribe();
    console.log('Download polling lifecycle checks passed.');
  } finally {
    delete require.cache[storePath];
    if (originalApiCache) require.cache[apiPath] = originalApiCache;
    else delete require.cache[apiPath];
    if (originalTsLoader) require.extensions['.ts'] = originalTsLoader;
    else delete require.extensions['.ts'];
    if (originalWindow === undefined) delete global.window;
    else global.window = originalWindow;
    if (originalLocalStorage === undefined) delete global.localStorage;
    else global.localStorage = originalLocalStorage;
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
