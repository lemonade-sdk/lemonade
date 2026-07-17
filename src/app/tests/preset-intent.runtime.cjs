const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const webpack = require('webpack');

async function bundlePresetStore() {
  const outputPath = fs.mkdtempSync(path.join(os.tmpdir(), 'lemonade-preset-intent-'));
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

    assert.deepEqual(presets.TEMPERATURE_HINT_VALUES, {
      precise: 0.4,
      balanced: 0.7,
      exploratory: 0.9,
      creative: 1.1,
    });
    assert.equal(presets.temperatureHintFromValue(0.54), 'precise');
    assert.equal(presets.temperatureHintFromValue(0.55), 'balanced');
    assert.equal(presets.temperatureHintFromValue(0.80), 'exploratory');
    assert.equal(presets.temperatureHintFromValue(1.00), 'creative');

    const maximum = 200000;
    for (const hint of ['small', 'medium', 'large', 'max']) {
      const value = presets.contextSizeForHint(hint, maximum);
      assert.ok(Number.isInteger(value), `${hint} context must be an integer`);
      assert.ok(value <= maximum, `${hint} context must not exceed the model maximum`);
    }
    assert.equal(presets.contextSizeForHint('small', maximum), 4096);
    assert.equal(presets.contextSizeForHint('max', maximum), maximum);

    const codePreset = presets.STARTERS.find(preset => preset.id === 's-code');
    const creativePreset = presets.STARTERS.find(preset => preset.id === 's-creative');
    assert.ok(codePreset && creativePreset);

    // Preset MCP migration is deterministic and bounded.
    const basePreset = {
      id: 'mcp-migration',
      name: 'MCP migration',
      description: '',
      applies_to: ['chat'],
      recipe_options: {},
      sampling: {},
      engine_hint: 'auto',
      starter: false,
      system_prompt_id: 'none',
      system_prompts: [],
    };
    const legacyToolsOn = presets.sanitizePreset({ ...basePreset, tools_enabled: true });
    const legacyToolsOff = presets.sanitizePreset({ ...basePreset, id: 'mcp-off', tools_enabled: false });
    const explicitMcp = presets.sanitizePreset({
      ...basePreset,
      id: 'mcp-explicit',
      tools_enabled: false,
      mcp_server_ids: ['lemonade', 'filesystem', 'filesystem', 'git', 'browser', 'fifth', '../invalid'],
    });
    assert.deepEqual(legacyToolsOn.mcp_server_ids, ['lemonade']);
    assert.equal(legacyToolsOn.tools_enabled, true);
    assert.deepEqual(legacyToolsOff.mcp_server_ids, []);
    assert.equal(legacyToolsOff.tools_enabled, false);
    assert.deepEqual(explicitMcp.mcp_server_ids, ['lemonade', 'filesystem', 'git', 'browser']);
    assert.equal(explicitMcp.tools_enabled, true, 'canonical MCP selection wins over the legacy boolean');
    assert.equal(presets.presetMcpDisplayText(explicitMcp), '4 MCPs');
    assert.deepEqual(presets.presetMcpServerIds({ id: 'legacy-only', tools_enabled: true }), ['lemonade']);

    const model = {
      id: 'qwen-coder',
      name: 'qwen-coder',
      labels: ['llm', 'coding'],
      recipe: 'llamacpp',
      ctx_size: 4096,
      max_context_window: maximum,
    };
    assert.equal(presets.modelContextSize(model), maximum);
    assert.equal(presets.modelDefaultContextSize(model), 4096);
    assert.equal(presets.modelDefaultRecipeOptions(model).ctx_size, 4096);

    let resolved = presets.resolvedModelTuningForPreset('qwen-coder', model, codePreset);
    assert.equal(resolved.tuning.sampling.temperature, 0.4);
    assert.equal(resolved.sources.sampling.temperature, 'generic');
    assert.equal(resolved.tuning.recipe_options.ctx_size, presets.contextSizeForHint('large', maximum));
    assert.equal(resolved.sources.recipe_options.ctx_size, 'generic');
    assert.deepEqual(resolved.intent_values.temperature, {
      precise: 0.4,
      balanced: 0.7,
      exploratory: 0.9,
      creative: 1.1,
    });
    assert.deepEqual(resolved.intent_values.context, {
      small: presets.contextSizeForHint('small', maximum),
      medium: presets.contextSizeForHint('medium', maximum),
      large: presets.contextSizeForHint('large', maximum),
      max: maximum,
    });

    const modelWithBuiltInTuning = {
      ...model,
      preset_tunings: {
        's-code': {
          intent_values: {
            temperature: { precise: 0.25, balanced: 0.65, exploratory: 0.85, creative: 1.05 },
            context: { small: 8192, medium: 65536, large: 131072 },
          },
        },
      },
    };
    resolved = presets.resolvedModelTuningForPreset('qwen-coder', modelWithBuiltInTuning, codePreset);
    assert.equal(resolved.tuning.sampling.temperature, 0.25);
    assert.equal(resolved.tuning.recipe_options.ctx_size, 131072);
    assert.equal(resolved.sources.sampling.temperature, 'built-in');
    assert.equal(resolved.sources.recipe_options.ctx_size, 'built-in');
    assert.equal(resolved.intent_values.temperature.creative, 1.05);
    assert.equal(resolved.intent_values.context.medium, 65536);
    assert.equal(resolved.intent_sources.temperature.creative, 'built-in');
    assert.equal(resolved.intent_sources.context.medium, 'built-in');
    assert.equal(resolved.intent_values.context.max, maximum);
    assert.equal(resolved.intent_sources.context.max, 'generic');

    presets.saveModelTuning('qwen-coder', {
      intent_values: {
        temperature: { precise: 0.15, balanced: 0.55, exploratory: 0.75, creative: 0.95 },
        context: { small: 4096, medium: 49152, large: 98304 },
      },
      sampling: { top_p: 0.8 },
    }, codePreset.id);
    presets.saveModelTuning('qwen-coder', {
      intent_values: {
        temperature: { creative: 1.0 },
        context: { medium: 32768 },
      },
    }, creativePreset.id);

    resolved = presets.resolvedModelTuningForPreset('qwen-coder', modelWithBuiltInTuning, codePreset);
    assert.equal(resolved.tuning.sampling.temperature, 0.15);
    assert.equal(resolved.tuning.recipe_options.ctx_size, 98304);
    assert.equal(resolved.sources.sampling.temperature, 'custom');
    assert.equal(resolved.sources.recipe_options.ctx_size, 'custom');
    assert.deepEqual(resolved.intent_values.temperature, {
      precise: 0.15,
      balanced: 0.55,
      exploratory: 0.75,
      creative: 0.95,
    });
    assert.equal(resolved.intent_values.context.small, 4096);
    assert.equal(resolved.intent_values.context.medium, 49152);
    assert.equal(resolved.intent_values.context.large, 98304);
    assert.equal(resolved.intent_values.context.max, maximum, 'Max context must remain the model maximum');
    assert.equal(resolved.intent_sources.context.max, 'generic');
    assert.equal(presets.loadModelTuning('qwen-coder', creativePreset.id).intent_values.temperature.creative, 1.0);
    assert.equal(presets.loadModelTuning('qwen-coder', creativePreset.id).intent_values.context.medium, 32768);
    assert.equal(presets.loadModelTuning('qwen-coder', creativePreset.id).intent_values.temperature.precise, undefined);

    presets.resetModelTuning('qwen-coder', codePreset.id);
    resolved = presets.resolvedModelTuningForPreset('qwen-coder', modelWithBuiltInTuning, codePreset);
    assert.equal(resolved.tuning.sampling.temperature, 0.25);
    assert.equal(resolved.sources.sampling.temperature, 'built-in');
    assert.equal(resolved.intent_values.context.large, 131072);

    presets.saveModelTuning('legacy-model', {
      recipe_options: { ctx_size: 24576 },
      sampling: { temperature: 0.33 },
    }, codePreset.id);
    const migratedLegacy = presets.loadModelTuning('legacy-model', codePreset.id);
    assert.equal(migratedLegacy.intent_values.temperature.precise, 0.33);
    assert.equal(migratedLegacy.intent_values.context.large, 24576);
    assert.equal(migratedLegacy.sampling.temperature, undefined);
    assert.equal(migratedLegacy.recipe_options.ctx_size, undefined);

    storage.clear();
    assert.equal(presets.recipeOptionsForModel('qwen-coder', model), undefined);
    assert.deepEqual(presets.samplingForModel('qwen-coder', model), {});
    presets.saveApplied({ 'qwen-coder': codePreset.id });
    assert.equal(
      presets.recipeOptionsForModel('qwen-coder', model).ctx_size,
      presets.contextSizeForHint('large', maximum),
    );
    assert.equal(presets.samplingForModel('qwen-coder', model).temperature, 0.4);

    const transcriptionModel = { id: 'whisper', labels: ['transcription'], recipe: 'whispercpp' };
    assert.equal(presets.isCompatible(codePreset, transcriptionModel), false);
    assert.equal(presets.presetSupportsChatIntent(presets.STARTERS.find(preset => preset.id === 's-quality')), false);

    // Backend args are a dedicated lower-priority tuning layer. They are not
    // Presets and are resolved only for the exact concrete backend.
    presets.saveBackendTuning('llamacpp:vulkan', '--backend-args', 'user');
    let merged = presets.recipeOptionsForModel(
      'qwen-coder',
      model,
      null,
      { recipes: { llamacpp: { default_backend: 'vulkan' } } },
    );
    assert.equal(merged.llamacpp_args, '--backend-args');

    // Model tuning wins over the backend-wide args layer.
    presets.saveModelTuning('qwen-coder', {
      recipe_options: { llamacpp_args: '--model-args' },
      sampling: {},
    }, codePreset.id);
    merged = presets.recipeOptionsForModel(
      'qwen-coder',
      model,
      null,
      { recipes: { llamacpp: { default_backend: 'vulkan' } } },
    );
    assert.equal(merged.llamacpp_args, '--model-args');

    // A different backend key must not leak into this load.
    presets.resetModelTuning('qwen-coder', codePreset.id);
    merged = presets.recipeOptionsForModel(
      'qwen-coder',
      model,
      null,
      { recipes: { llamacpp: { default_backend: 'cpu' } } },
    );
    assert.notEqual(merged?.llamacpp_args, '--backend-args');

    // AutoOpt replaces the exact backend entry instead of creating or mutating
    // a Preset. A later manual edit replaces it again and changes the source.
    presets.saveBackendTuning('llamacpp:vulkan', '--autoopt-args', 'optimized', 'run-1');
    assert.deepEqual(presets.backendTuningForKey('llamacpp:vulkan'), {
      args: '--autoopt-args',
      source: 'optimized',
      auto_opt_run_id: 'run-1',
      updated_at: presets.backendTuningForKey('llamacpp:vulkan').updated_at,
    });
    presets.saveBackendTuning('llamacpp:vulkan', '--manual-replacement', 'user');
    const manualBackendTuning = presets.backendTuningForKey('llamacpp:vulkan');
    assert.equal(manualBackendTuning.args, '--manual-replacement');
    assert.equal(manualBackendTuning.source, 'user');
    assert.equal(manualBackendTuning.auto_opt_run_id, undefined);

    // Legacy backend-preset args migrate once into backend tuning, but semantic
    // intent/context/backend selection never do.
    storage.clear();
    const legacyModelPreset = {
      id: 'legacy-model-preset',
      name: 'Legacy model preset',
      description: '',
      applies_to: ['chat'],
      recipe_options: { ctx_size: 8192, llamacpp_args: '--model-wins' },
      sampling: { temperature: 0.7 },
      engine_hint: 'auto',
      starter: false,
      system_prompt_id: 'none',
      system_prompts: [],
      tools_enabled: false,
    };
    const legacyBackendPreset = {
      ...legacyModelPreset,
      id: 'legacy-backend-preset',
      name: 'Legacy backend preset',
      recipe_options: { ctx_size: 2048, llamacpp_args: '--backend-base', llamacpp_backend: 'cpu' },
    };
    presets.saveUserPresets([legacyModelPreset, legacyBackendPreset]);
    presets.saveApplied({ 'legacy-bound-model': legacyModelPreset.id });
    localStorage.setItem('lemonade:guest:shared:backend_presets', JSON.stringify({ 'llamacpp:cpu': legacyBackendPreset.id }));
    const legacyMerged = presets.recipeOptionsForModel(
      'legacy-bound-model',
      { id: 'legacy-bound-model', name: 'legacy-bound-model', labels: ['llm'], recipe: 'llamacpp', downloaded: true },
      null,
      { recipes: { llamacpp: { default_backend: 'cpu' } } },
    );
    assert.equal(legacyMerged.ctx_size, 8192);
    assert.equal(legacyMerged.llamacpp_args, '--model-wins');
    assert.equal(legacyMerged.llamacpp_backend, undefined);
    assert.equal(presets.backendTuningForKey('llamacpp:cpu').args, '--backend-base');


    console.log('Preset intent runtime tests passed.');
  } finally {
    fs.rmSync(outputPath, { recursive: true, force: true });
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
