const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const webpack = require('webpack');

function installLocalStorage() {
  const values = new Map();
  global.localStorage = {
    getItem: key => values.has(key) ? values.get(key) : null,
    setItem: (key, value) => values.set(key, String(value)),
    removeItem: key => values.delete(key),
    clear: () => values.clear(),
    key: index => [...values.keys()][index] ?? null,
    get length() { return values.size; },
  };
  return values;
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
  classifiers: [],
  rules: [{
    id: 'short',
    routeTo: 'fast',
    condition: { id: 'max', kind: 'leaf', type: 'max_chars', numberValue: 500 },
    outputsText: '',
  }],
};

(async () => {
  const storage = installLocalStorage();
  const { outputPath, modulePath } = await bundleStore();
  try {
    const store = require(modulePath);
    const first = store.upsertRouterRecord('account-a', validDraft);
    assert.equal(first.model_name, 'user.Scoped-router');
    assert.equal(store.loadRouterRecords('account-a').length, 1);
    assert.equal(store.loadRouterRecords('account-b').length, 0, 'router definitions must be account scoped');
    assert.ok([...storage.keys()].some(key => key.includes('account-a') && key.endsWith('router_collections')));

    const updated = store.upsertRouterRecord('account-a', { ...validDraft, name: 'Scoped router renamed' });
    assert.equal(store.loadRouterRecords('account-a').length, 2, 'a changed generated model id creates a separate router');
    assert.notEqual(updated.model_name, first.model_name);

    const replacement = store.upsertRouterRecord('account-a', { ...validDraft, modelName: first.model_name, name: 'Better display name' });
    const records = store.loadRouterRecords('account-a');
    assert.equal(records.length, 2);
    assert.equal(records.find(item => item.model_name === first.model_name).display_name, 'Better display name');
    assert.equal(records.find(item => item.model_name === first.model_name).createdAt, first.createdAt, 'upsert preserves creation time');

    const modelInfo = store.routerRecordToModelInfo(replacement);
    assert.equal(modelInfo.recipe, 'collection.router');
    assert.equal(modelInfo.custom, true);
    assert.equal(modelInfo.routing.default_model, 'smart');

    const badImport = store.importRouterRecords('account-a', {
      version: '1', model_name: 'user.bad', recipe: 'collection.router', components: ['fast'],
      routing: { candidates: ['fast'], default_model: 'fast', router: { type: 'llm', model: 'fast', prompt: 'route' } },
    });
    assert.equal(badImport.imported, 0);
    assert.match(badImport.errors[0], /not supported/i);

    store.deleteRouterRecord('account-a', first.model_name);
    assert.equal(store.loadRouterRecords('account-a').some(item => item.model_name === first.model_name), false);

    console.log('GUI3 router store scope and persistence checks passed.');
  } finally {
    fs.rmSync(outputPath, { recursive: true, force: true });
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
