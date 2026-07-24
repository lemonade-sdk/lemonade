const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const webpack = require('webpack');

function installBrowserStorageShim() {
  const storage = new Map();
  global.localStorage = {
    getItem: key => storage.has(key) ? storage.get(key) : null,
    setItem: (key, value) => storage.set(key, String(value)),
    removeItem: key => storage.delete(key),
    clear: () => storage.clear(),
  };
  global.CustomEvent = class CustomEvent { constructor(type) { this.type = type; } };
  global.window = { dispatchEvent() {} };
  return storage;
}

async function bundleActions() {
  const root = path.resolve(__dirname, '..');
  const outputPath = fs.mkdtempSync(path.join(os.tmpdir(), 'lemonade-autoopt-actions-'));
  const mockApiPath = path.join(outputPath, 'mockApi.ts');
  const entryPath = path.join(outputPath, 'entry.ts');
  const tsconfigPath = path.join(outputPath, 'tsconfig.json');
  fs.writeFileSync(mockApiPath, `
export interface ModelInfo { id: string; name?: string; checkpoint?: string; [key: string]: unknown }
export const calls: Array<{ kind: string; model: string; options: Record<string, unknown> }> = [];
const api = {
  allModels: [{ id: 'model-a', name: 'model-a', checkpoint: 'org/model-a', labels: ['llm'], recipe: 'llamacpp', max_context_window: 131072 }],
  loadedModels: [] as Array<{ model_name: string }>,
  systemInfoData: { recipes: { llamacpp: { backends: { vulkan: { state: 'installed' } } } } },
  async loadModel(model: string, options: Record<string, unknown>) { calls.push({ kind: 'load', model, options }); },
  async reloadModel(model: string, options: Record<string, unknown>) { calls.push({ kind: 'reload', model, options }); },
};
export { api };
export default api;
`);
  fs.writeFileSync(tsconfigPath, JSON.stringify({ compilerOptions: { target: 'ES2020', module: 'ESNext', moduleResolution: 'node', esModuleInterop: true, skipLibCheck: true }, files: [entryPath, mockApiPath] }));
  fs.writeFileSync(entryPath, `
export * as presets from ${JSON.stringify(path.join(root, 'src/presetStore.ts'))};
export * as actions from ${JSON.stringify(path.join(root, 'src/features/autoOpt/presetFromRun.ts'))};
export { api, calls } from './mockApi';
`);

  const config = {
    mode: 'development',
    target: 'node',
    entry: entryPath,
    output: { path: outputPath, filename: 'bundle.cjs', library: { type: 'commonjs2' } },
    resolve: {
      extensions: ['.ts', '.tsx', '.js'],
      alias: { [path.join(root, 'src/api.ts')]: mockApiPath },
    },
    module: { rules: [{ test: /\.tsx?$/, use: { loader: 'ts-loader', options: { transpileOnly: true, configFile: tsconfigPath } }, exclude: /node_modules/ }] },
    optimization: { minimize: false },
  };
  await new Promise((resolve, reject) => webpack(config, (error, stats) => {
    if (error) return reject(error);
    if (stats?.hasErrors()) return reject(new Error(stats.toString({ all: false, errors: true })));
    resolve();
  }));
  return { outputPath, modulePath: path.join(outputPath, 'bundle.cjs') };
}

(async () => {
  const storage = installBrowserStorageShim();
  const { outputPath, modulePath } = await bundleActions();
  try {
    const { presets, actions, api, calls } = require(modulePath);
    const intent = {
      id: 'u-intent', name: 'My intent', description: '', applies_to: ['chat'],
      temperature_hint: 'balanced', context_hint: 'medium', thinking_mode: 'normal',
      recipe_options: {}, sampling: {}, engine_hint: 'auto', starter: false,
      system_prompt_id: 'none', system_prompts: [], tools_enabled: true,
    };
    presets.saveUserPresets([intent]);
    presets.saveApplied({ 'model-a': intent.id });
    presets.saveBackendTuning('llamacpp:vulkan', '--old-manual', 'user');

    const rec = {
      label: 'Fast', llamacpp_backend: 'vulkan', ctx_size: 32768,
      llamacpp_args: '--threads 8 --batch-size 512', rationale: [],
    };
    const run = {
      id: 'run-42', model: 'model-a', checkpoint: 'org/model-a', budget: 'quick',
      answers: { parallel: { mode: 'single' }, kv_cache_quant: 'none', ram_headroom: 'normal', allow_network: false },
      allow_unload: false, status: 'completed', created_at: new Date().toISOString(), stages: [],
      measurements: { fit: [], bench: [] },
      result: { primary: rec, alternatives: [], sampling_defaults: { temperature: 0.5, min_p: 0.02, source: 'model' } },
    };

    const presetsBefore = presets.loadUserPresets();
    const appliedBefore = presets.loadApplied();
    const contextBeforeAutoOpt = presets.resolvedModelTuningForPreset('model-a', api.allModels[0], intent)
      .tuning.recipe_options.ctx_size;
    const backendResult = actions.applyRunToBackend(run, rec);
    assert.equal(backendResult.key, 'llamacpp:vulkan');
    assert.deepEqual(presets.loadUserPresets(), presetsBefore, 'backend apply must not create a Preset');
    assert.deepEqual(presets.loadApplied(), appliedBefore, 'backend apply must not re-link model intent');
    const backendTuning = presets.backendTuningForKey('llamacpp:vulkan');
    assert.equal(backendTuning.args, rec.llamacpp_args, 'AutoOpt replaces the exact backend args');
    assert.equal(backendTuning.source, 'optimized');
    assert.equal(backendTuning.auto_opt_run_id, run.id);

    const modelResult = actions.saveRunToModelTuning(run, rec, api.allModels[0]);
    assert.deepEqual(modelResult, { presetId: intent.id, presetName: intent.name });
    assert.deepEqual(presets.loadUserPresets(), presetsBefore, 'model tuning save must not create a Preset');
    const modelTuning = presets.loadModelTuning('model-a', intent.id);
    assert.equal(modelTuning.source, 'optimized');
    assert.equal(modelTuning.auto_opt_run_id, run.id);
    assert.equal(modelTuning.intent_values.context, undefined, 'AutoOpt must not override Context intent');
    assert.equal(modelTuning.recipe_options.ctx_size, undefined, 'AutoOpt must not persist ctx_size as a load option');
    assert.equal(modelTuning.recipe_options.llamacpp_backend, 'vulkan');
    assert.equal(modelTuning.recipe_options.llamacpp_args, rec.llamacpp_args);
    assert.equal(modelTuning.intent_values.temperature.balanced, 0.5);
    assert.equal(modelTuning.sampling.min_p, 0.02);
    const resolved = presets.resolvedModelTuningForPreset('model-a', api.allModels[0], intent);
    assert.equal(resolved.tuning.recipe_options.ctx_size, contextBeforeAutoOpt,
      'AutoOpt must leave the Preset-derived context unchanged');
    assert.equal(resolved.tuning.sampling.temperature, 0.5);

    const storageBeforeTry = new Map(storage);
    await actions.applyRunNow(run, rec);
    assert.equal(calls.length, 1);
    assert.deepEqual(calls[0], {
      kind: 'load', model: 'model-a',
      options: { llamacpp_backend: 'vulkan', llamacpp_args: rec.llamacpp_args, save_options: false },
    });
    assert.deepEqual(new Map(storage), storageBeforeTry, 'Try now must not persist any setting');

    api.loadedModels.push({ model_name: 'model-a' });
    await actions.applyRunNow(run, rec);
    assert.equal(calls[1].kind, 'reload');

    console.log('AutoOpt action contract tests passed.');
  } finally {
    fs.rmSync(outputPath, { recursive: true, force: true });
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
