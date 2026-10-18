const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const webpack = require('webpack');

function installLocalStorage() {
  const values = new Map();
  let writes = 0;
  global.localStorage = {
    get length() { return values.size; },
    key: index => [...values.keys()][index] ?? null,
    getItem: key => values.has(key) ? values.get(key) : null,
    setItem: (key, value) => { writes += 1; values.set(key, String(value)); },
    removeItem: key => values.delete(key),
    clear: () => values.clear(),
  };
  return { values, writes: () => writes };
}

async function bundleStore() {
  const outputPath = fs.mkdtempSync(path.join(os.tmpdir(), 'lemonade-router-store-'));
  const config = {
    mode: 'development',
    target: 'node',
    entry: path.resolve(__dirname, '../src/features/router/routerStore.ts'),
    output: { path: outputPath, filename: 'routerStore.cjs', library: { type: 'commonjs2' } },
    resolve: { extensions: ['.ts', '.tsx', '.js'] },
    module: { rules: [{ test: /\.tsx?$/, use: 'ts-loader', exclude: /node_modules/ }] },
    optimization: { minimize: false },
  };
  await new Promise((resolve, reject) => {
    webpack(config, (error, stats) => {
      if (error) return reject(error);
      if (stats?.hasErrors()) return reject(new Error(stats.toString({ all: false, errors: true })));
      resolve();
    });
  });
  return { outputPath, modulePath: path.join(outputPath, 'routerStore.cjs') };
}

const validDraft = {
  name: 'Scoped router',
  candidates: ['fast', 'smart'],
  defaultModel: 'smart',
  mode: 'rules',
  llmRouter: { model: '', prompt: '' },
  classifiers: [],
  rules: [{
    id: 'short', routeTo: 'fast', outputsText: '',
    condition: { id: 'max', kind: 'leaf', type: 'max_chars', numberValue: 500 },
  }],
};

(async () => {
  const storage = installLocalStorage();
  // A stale record may exist from an older GUI, but the runtime store must not expose it.
  storage.values.set('lemonade:router_collections', JSON.stringify({ version: 1, routers: [{ model_name: 'user.old' }] }));
  const writesBefore = storage.writes();
  const { outputPath, modulePath } = await bundleStore();
  try {
    const store = require(modulePath);
    assert.deepEqual(store.loadRouterRecords(), []);
    const transient = store.upsertRouterRecord(validDraft);
    assert.equal(transient.model_name, 'user.Scoped-router');
    assert.equal(store.loadRouterRecords().length, 0);
    store.deleteRouterRecord(transient.model_name);
    assert.equal(storage.writes(), writesBefore, 'router compatibility helpers must never write localStorage');
    const info = store.routerRecordToModelInfo(transient);
    assert.equal(info.recipe, 'collection.router');
    assert.equal(info.routing.default_model, 'smart');
    assert.equal(store.routerRegistrationOptions(info).display_name, 'Scoped router');
    console.log('GUI3 router store is non-persistent; lemond owns router definitions.');
  } finally {
    fs.rmSync(outputPath, { recursive: true, force: true });
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
