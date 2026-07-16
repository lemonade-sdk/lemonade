const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const webpack = require('webpack');

async function bundlePresetStore() {
  const outputPath = fs.mkdtempSync(path.join(os.tmpdir(), 'lemonade-autoopt-glue-'));
  const config = {
    mode: 'development',
    target: 'node',
    entry: path.resolve(__dirname, '../src/presetStore.ts'),
    output: {
      path: outputPath,
      filename: 'presetStore.cjs',
      library: { type: 'commonjs2' },
    },
    resolve: { extensions: ['.ts', '.tsx', '.js'] },
    module: {
      rules: [{ test: /\.tsx?$/, use: 'ts-loader', exclude: /node_modules/ }],
    },
    optimization: { minimize: false },
  };

  await new Promise((resolve, reject) => {
    webpack(config, (error, stats) => {
      if (error) return reject(error);
      if (stats?.hasErrors()) return reject(new Error(stats.toString({ all: false, errors: true })));
      resolve();
    });
  });

  return { outputPath, modulePath: path.join(outputPath, 'presetStore.cjs') };
}

function installBrowserStorageShim() {
  const storage = new Map();
  global.localStorage = {
    getItem: key => storage.has(key) ? storage.get(key) : null,
    setItem: (key, value) => storage.set(key, String(value)),
    removeItem: key => storage.delete(key),
    clear: () => storage.clear(),
  };
  global.CustomEvent = class CustomEvent {
    constructor(type) { this.type = type; }
  };
  global.window = { dispatchEvent() {} };
  return storage;
}

(async () => {
  const { outputPath, modulePath } = await bundlePresetStore();
  const storage = installBrowserStorageShim();

  try {
    const presets = require(modulePath);

    assert.deepEqual(
      presets.sanitizeSamplingParams({ temperature: 0.7, top_p: 0.9, min_p: 0.03, bogus: 5 }),
      { temperature: 0.7, top_p: 0.9, min_p: 0.03 },
      'min_p must survive sampling sanitization',
    );
    assert.deepEqual(
      presets.sanitizeRecipeOptions({ mmproj_enabled: false, llamacpp_args: '-b 512' }),
      { mmproj_enabled: false, llamacpp_args: '-b 512' },
      'mmproj_enabled=false must survive recipe-option sanitization',
    );

    const codePreset = presets.STARTERS.find(preset => preset.id === 's-code');
    assert.ok(codePreset);
    const model = {
      id: 'opt-model',
      name: 'opt-model',
      labels: ['llm', 'coding'],
      recipe: 'llamacpp',
      ctx_size: 4096,
      max_context_window: 12288,
    };

    presets.saveOptimizedModelTuning('opt-model', {
      recipe_options: { ctx_size: 8192, llamacpp_backend: 'vulkan', llamacpp_args: '-b 512 -ub 256', mmproj_enabled: false },
      sampling: { temperature: 0.7, min_p: 0.05 },
    }, codePreset.id, 'run-123');

    const stored = presets.loadModelTuning('opt-model', codePreset.id);
    assert.equal(stored.source, 'optimized');
    assert.equal(stored.auto_opt_run_id, 'run-123');
    assert.equal(stored.sampling.min_p, 0.05);
    assert.equal(stored.recipe_options.mmproj_enabled, false);
    assert.equal(stored.recipe_options.llamacpp_backend, 'vulkan');

    let resolved = presets.resolvedModelTuningForPreset('opt-model', model, codePreset);
    assert.equal(resolved.tuning.source, 'optimized');
    assert.equal(resolved.tuning.auto_opt_run_id, 'run-123');
    assert.equal(resolved.tuning.recipe_options.ctx_size, 8192);
    assert.equal(resolved.tuning.recipe_options.llamacpp_args, '-b 512 -ub 256');
    assert.equal(resolved.tuning.recipe_options.mmproj_enabled, false);
    assert.equal(resolved.tuning.sampling.temperature, 0.7);
    assert.equal(resolved.tuning.sampling.min_p, 0.05);
    assert.equal(resolved.sources.recipe_options.ctx_size, 'optimized');
    assert.equal(resolved.sources.recipe_options.llamacpp_args, 'optimized');
    assert.equal(resolved.sources.recipe_options.mmproj_enabled, 'optimized');
    assert.equal(resolved.sources.sampling.temperature, 'optimized');
    assert.equal(resolved.sources.sampling.min_p, 'optimized');

    presets.saveModelTuning('opt-model', {
      recipe_options: { llamacpp_args: '-b 256' },
      sampling: { min_p: 0.02 },
    }, codePreset.id);

    const downgraded = presets.loadModelTuning('opt-model', codePreset.id);
    assert.equal(downgraded.source, 'user', 'manual save must downgrade optimized tuning to user');
    assert.equal(downgraded.auto_opt_run_id, undefined);
    assert.equal(downgraded.sampling.min_p, 0.02);

    resolved = presets.resolvedModelTuningForPreset('opt-model', model, codePreset);
    assert.equal(resolved.tuning.source, 'user');
    assert.equal(resolved.tuning.auto_opt_run_id, undefined);
    assert.equal(resolved.sources.recipe_options.llamacpp_args, 'custom');
    assert.equal(resolved.sources.sampling.min_p, 'custom');

    storage.clear();
    presets.saveOptimizedModelTuning('opt-model', {
      recipe_options: { ctx_size: 8192, llamacpp_args: '-b 512' },
      sampling: {},
    }, codePreset.id, 'run-456');
    presets.saveApplied({ 'opt-model': codePreset.id });
    const loadOptions = presets.recipeOptionsForModel('opt-model', model);
    assert.equal(loadOptions.ctx_size, 8192, 'optimized ctx_size must flow into load options');
    assert.equal(loadOptions.llamacpp_args, '-b 512');

    // createPresetFromRun path: a recommended ctx BELOW the model maximum
    // (e.g. clamped to 65536 on a 262144 model) maps to an editable hint and
    // resolves the optimizer's exact ctx with source 'optimized'.
    storage.clear();
    const bigModel = {
      id: 'big-model',
      name: 'big-model',
      labels: ['llm'],
      recipe: 'llamacpp',
      ctx_size: 4096,
      max_context_window: 262144,
      downloaded: true,
    };
    const hint = presets.contextHintFromValue(65536, presets.modelContextSize(bigModel));
    assert.equal(hint, 'medium', 'a below-max optimized ctx must map to an editable hint');
    const aoPreset = presets.sanitizePreset({
      id: 'u-autoopt-ctx',
      name: 'AutoOpt · big-model',
      description: '',
      applies_to: ['chat'],
      temperature_hint: 'balanced',
      context_hint: hint,
      thinking_mode: 'normal',
      recipe_options: {},
      sampling: {},
      engine_hint: 'llamacpp',
      starter: false,
      auto_opt_run_id: 'run-ctx',
      auto_opt_enabled: true,
    });
    presets.saveUserPresets([aoPreset]);
    presets.saveApplied({ 'big-model': aoPreset.id });
    presets.saveOptimizedModelTuning('big-model', {
      recipe_options: { ctx_size: 65536, llamacpp_backend: 'vulkan', llamacpp_args: '-b 2048 -ub 2048' },
      sampling: {},
    }, aoPreset.id, 'run-ctx');
    const resolvedBig = presets.resolvedModelTuningForPreset('big-model', bigModel, aoPreset);
    assert.equal(resolvedBig.tuning.recipe_options.ctx_size, 65536,
      'the optimizer ctx must survive intent migration exactly');
    assert.equal(resolvedBig.sources.recipe_options.ctx_size, 'optimized');
    assert.equal(resolvedBig.tuning.source, 'optimized');
    assert.equal(presets.recipeOptionsForModel('big-model', bigModel).ctx_size, 65536);

    console.log('AutoOpt glue runtime tests passed.');
  } finally {
    fs.rmSync(outputPath, { recursive: true, force: true });
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
