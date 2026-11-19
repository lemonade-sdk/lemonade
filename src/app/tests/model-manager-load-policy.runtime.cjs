const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const filename = path.join(root, 'src/components/ModelManager.tsx');
const source = ts.createSourceFile(filename, fs.readFileSync(filename, 'utf8'),
  ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
const initializers = new Map();
function collect(node) {
  if (ts.isVariableDeclaration(node)) initializers.set(node.name.getText(source), node.initializer);
  ts.forEachChild(node, collect);
}
collect(source);
function instantiate(name, bindings) {
  const initializer = initializers.get(name);
  assert.ok(initializer, `ModelManager must provide ${name}`);
  const code = ts.transpileModule(`const ${name} = ${initializer.getText(source)}; return ${name};`, {
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
  }).outputText;
  return Function(...Object.keys(bindings), code)(...Object.values(bindings));
}

const oldLoader = require.extensions['.ts'];
require.extensions['.ts'] = (module, file) => {
  const code = ts.transpileModule(fs.readFileSync(file, 'utf8'), {
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS, esModuleInterop: true },
    fileName: file,
  }).outputText;
  module._compile(code, file);
};

(async () => {
  try {
    const { customLoadOptions } = require(path.join(root, 'src/features/customModels/customModelStore.ts'));
    const custom = {
      id: 'user.Custom', model_name: 'user.Custom', recipe: 'llamacpp', custom: true,
      recipe_options: { ctx_size: 8192, llamacpp_backend: 'metal', llamacpp_args: '--threads 3', merge_args: false },
    };
    const original = JSON.parse(JSON.stringify(custom));
    const collection = { id: 'user.Bundle', model_name: 'user.Bundle', recipe: 'collection.omni', components: [custom.model_name] };
    const loadCalls = [];
    const reloadCalls = [];
    const api = {
      loadModel: async (...args) => { loadCalls.push(args); },
      reloadModel: async (...args) => { reloadCalls.push(args); },
      pullModel: async (_name, callbacks) => { await callbacks.onComplete({ status: 'success' }); },
    };
    const modelName = model => model.model_name;
    const noop = () => {};
    const loadModelRuntime = instantiate('loadModelRuntime', {
      api, modelName, customLoadOptions,
      findCurrentModel: name => name === collection.model_name ? collection : custom,
      isRouterModelInfo: () => false,
      ensureServerRouterReady: async () => {},
      isCollectionModel: info => info.recipe === 'collection.omni',
      getCollectionComponents: info => info.components || [],
    });

    await loadModelRuntime(custom, new Set(), { ctx_size: 4096 });
    assert.deepEqual(loadCalls.at(-1), [custom.model_name,
      { save_options: false, ctx_size: 4096, llamacpp_backend: 'metal' }, custom, 'server-config'],
    'a panel load must keep nonargument custom defaults without replaying hidden custom arguments');
    for (const args of ['--threads 4', '', null]) {
      await loadModelRuntime(custom, new Set(), { llamacpp_args: args, merge_args: false });
      assert.deepEqual(loadCalls.at(-1)[1], {
        save_options: false, ctx_size: 8192, llamacpp_backend: 'metal', llamacpp_args: args, merge_args: false,
      }, 'actual panel argument edits and clears must remain explicit');
    }
    await loadModelRuntime(custom, new Set(), {});
    assert.equal(loadCalls.at(-1)[3], 'server-config');
    assert.equal(Object.hasOwn(loadCalls.at(-1)[1], 'llamacpp_args'), false,
      'an explicit empty panel payload must not replay custom arguments');
    await loadModelRuntime(custom);
    assert.deepEqual(loadCalls.at(-1), [custom.model_name,
      { save_options: false, ...custom.recipe_options }, custom, 'local-overrides'],
    'ordinary nonpanel loads must retain intentional custom configuration');
    assert.deepEqual(custom, original, 'filtering a panel load must not mutate the custom model record');

    await loadModelRuntime(collection, new Set(), {});
    assert.deepEqual(loadCalls.at(-1), [custom.model_name,
      { save_options: false, ctx_size: 8192, llamacpp_backend: 'metal' }, custom, 'server-config'],
    'an ordinary panel collection load must not replay hidden component arguments');
    await loadModelRuntime(collection);
    assert.equal(loadCalls.at(-1)[3], 'local-overrides',
      'nonpanel collection loads must preserve intentional local component arguments');
    assert.equal(loadCalls.at(-1)[1].llamacpp_args, '--threads 3');

    const handleReloadModel = instantiate('handleReloadModel', {
      api, modelName, allModels: [custom], markBusy: noop, clearBusy: noop, refresh: async () => {},
    });
    await handleReloadModel(custom, { ctx_size: 4096 });
    assert.deepEqual(reloadCalls.at(-1), [custom.model_name, { ctx_size: 4096 }, custom, 'server-config'],
      'panel reload must request server-owned argument inheritance');
    await handleReloadModel(custom);
    assert.equal(reloadCalls.at(-1)[3], 'local-overrides',
      'reload without a panel payload must preserve the old argument policy');

    let pulling = {};
    const selected = [];
    const handlePullAndLoad = instantiate('handlePullAndLoad', {
      api, modelName, loadModelRuntime,
      pulling, activeDownloadForModel: () => false,
      downloadStore: { snapshot: () => ({}), wake: () => ({ percent: 100 }) },
      pullAbortRef: { current: {} }, latestLoadRequestRef: { current: '' },
      setPulling: update => { pulling = update(pulling); },
      markBusy: noop, clearBusy: noop, refresh: async () => {},
      onModelSelect: name => selected.push(name),
      showModelError: (_name, error) => { throw new Error(error); },
      friendlyErrorMessage: error => String(error),
    });
    await handlePullAndLoad(custom, { ctx_size: 4096 });
    assert.deepEqual(loadCalls.at(-1), [custom.model_name,
      { save_options: false, ctx_size: 4096, llamacpp_backend: 'metal' }, custom, 'server-config'],
    'Get and load must preserve the panel policy after the asynchronous download completes');
    assert.deepEqual(selected, [custom.model_name]);
    assert.deepEqual(pulling, {});
    console.log('Model manager load policy behavior checks passed.');
  } finally {
    if (oldLoader) require.extensions['.ts'] = oldLoader;
    else delete require.extensions['.ts'];
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
