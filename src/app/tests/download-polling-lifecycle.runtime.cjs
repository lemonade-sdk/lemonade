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
assert.doesNotMatch(
  source,
  /lemonade_download_manager_items_v1/,
  'download rows must never be persisted client-side',
);
assert.match(source, /visibleSnapshot\(\): DownloadListItem\[\]/);
assert.match(source, /isDismissedTerminal\(download, dismissed\)/);
assert.match(source, /wake\(modelName\?: string, type: DownloadType = 'model'\)/);
assert.doesNotMatch(source, /upsertFromPull|markLocal|TERMINAL_TTL_MS/);

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
    downloadStore.wake('test-model', 'model');
    await waitFor(
      () => downloadRequests >= 3 && downloadStore.snapshot().some(item => item.modelName === 'test-model' && item.running === true),
      1600,
    );
    assert.ok(
      downloadStore.snapshot().some(item => item.modelName === 'test-model' && item.running === true),
      'an active download must remain represented after polling',
    );

    // Pull callbacks are wake-up signals only. The server still says active,
    // so a callback may not manufacture a terminal row in canonical state.
    const requestsBeforeCallbackWake = downloadRequests;
    downloadStore.wake('test-model', 'model');
    await downloadStore.refresh();
    assert.equal(downloadRequests, requestsBeforeCallbackWake + 1);
    assert.ok(
      downloadStore.snapshot().some(item => item.modelName === 'test-model' && item.running === true),
      'callback data must not override the authoritative /downloads snapshot',
    );

    serverDownloads = [];
    const requestsBeforeEmptySnapshot = downloadRequests;
    await downloadStore.refresh();
    assert.equal(downloadRequests, requestsBeforeEmptySnapshot + 1);
    assert.equal(downloadStore.snapshot().length, 0, 'an empty server snapshot must remove the active row immediately');
    const requestsAtCompletion = downloadRequests;
    await sleep(1200);
    assert.equal(
      downloadRequests,
      requestsAtCompletion,
      'an idle authoritative snapshot must stop polling',
    );

    const requestsBeforeWakeBurst = downloadRequests;
    for (let i = 0; i < 25; i += 1) downloadStore.wake('burst-model', 'model');
    await sleep(650);
    assert.equal(
      downloadRequests,
      requestsBeforeWakeBurst + 1,
      'many SSE-style wake signals must coalesce into one /downloads request',
    );

    // Dismissed state is presentation-only: canonical state remains intact, and
    // non-terminal rows can never be hidden by a client-owned dismissed id.
    serverDownloads = [{
      id: 'model:completed-model',
      type: 'model',
      model_name: 'completed-model',
      status: 'completed',
      complete: true,
      running: false,
      percent: 100,
      created_at: Date.now() - 10_000,
    }];
    const requestsBeforeCompletedSnapshot = downloadRequests;
    await downloadStore.refresh();
    assert.equal(downloadRequests, requestsBeforeCompletedSnapshot + 1);
    downloadStore.dismissMany(['model:completed-model']);
    assert.equal(downloadStore.snapshot().length, 1, 'dismiss must not mutate canonical server state');
    assert.equal(downloadStore.visibleSnapshot().length, 0, 'dismiss may hide terminal history visually');

    const firstAttemptCreatedAt = serverDownloads[0].created_at;
    await downloadStore.refresh();
    assert.equal(downloadStore.visibleSnapshot().length, 0, 'the same dismissed server attempt stays hidden');

    serverDownloads = [{
      ...serverDownloads[0],
      status: 'error',
      complete: false,
      error: 'new attempt failed',
      created_at: firstAttemptCreatedAt + 5_000,
    }];
    await downloadStore.refresh();
    assert.equal(
      downloadStore.visibleSnapshot().length,
      1,
      'a new terminal attempt reusing the same stable id must not inherit an old dismissal',
    );

    serverDownloads = [{
      id: 'model:paused-model',
      type: 'model',
      model_name: 'paused-model',
      status: 'paused',
      running: false,
      percent: 25,
    }];
    const requestsBeforePausedSnapshot = downloadRequests;
    await downloadStore.refresh();
    assert.equal(downloadRequests, requestsBeforePausedSnapshot + 1);
    downloadStore.dismissMany(['model:paused-model']);
    assert.equal(downloadStore.snapshot().length, 1);
    assert.equal(downloadStore.visibleSnapshot().length, 1, 'paused server state must remain visible and blocking');

    const focusListeners = windowListeners.get('focus') || [];
    assert.equal(focusListeners.length, 1);
    const requestsBeforeFocus = downloadRequests;
    focusListeners[0]();
    await waitFor(() => downloadRequests === requestsBeforeFocus + 1);
    await sleep(1150);
    assert.equal(
      downloadRequests,
      requestsBeforeFocus + 1,
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
