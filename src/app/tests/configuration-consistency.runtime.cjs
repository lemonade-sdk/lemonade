const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const appPath = path.join(root, 'src/App.tsx');
const chatPath = path.join(root, 'src/components/ChatView.tsx');
const effectivePath = path.join(root, 'src/components/EffectiveSettingsModal.tsx');
const detailPath = path.join(root, 'src/components/ModelDetailPanel.tsx');
const backendManagerPath = path.join(root, 'src/components/BackendManager.tsx');
const modelListPath = path.join(root, 'src/components/ModelListPanel.tsx');
const modelConfigurationPath = path.join(root, 'src/modelConfiguration.ts');
const recipeMetadataPath = path.join(root, 'src/features/backends/recipeMetadata.ts');
const apiPath = path.join(root, 'src/api.ts');
const stylesPath = path.join(root, 'src/styles/styles.css');

const sources = new Map([
  [appPath, fs.readFileSync(appPath, 'utf8')],
  [chatPath, fs.readFileSync(chatPath, 'utf8')],
  [effectivePath, fs.readFileSync(effectivePath, 'utf8')],
  [detailPath, fs.readFileSync(detailPath, 'utf8')],
  [backendManagerPath, fs.readFileSync(backendManagerPath, 'utf8')],
  [modelListPath, fs.readFileSync(modelListPath, 'utf8')],
  [modelConfigurationPath, fs.readFileSync(modelConfigurationPath, 'utf8')],
  [recipeMetadataPath, fs.readFileSync(recipeMetadataPath, 'utf8')],
  [apiPath, fs.readFileSync(apiPath, 'utf8')],
]);
const styles = fs.readFileSync(stylesPath, 'utf8');

for (const [filename, source] of sources) {
  const compiled = ts.transpileModule(source, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2022,
      module: ts.ModuleKind.ESNext,
      jsx: ts.JsxEmit.ReactJSX,
    },
    fileName: filename,
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
}

const appSource = sources.get(appPath);
const chatSource = sources.get(chatPath);
const effectiveSource = sources.get(effectivePath);
const detailSource = sources.get(detailPath);
const backendManagerSource = sources.get(backendManagerPath);
const modelListSource = sources.get(modelListPath);
const modelConfigurationSource = sources.get(modelConfigurationPath);
const recipeMetadataSource = sources.get(recipeMetadataPath);
const apiSource = sources.get(apiPath);

assert.match(appSource, /<header className="titlebar" data-tauri-drag-region>/);
assert.doesNotMatch(appSource, /titlebar--chat/);
assert.doesNotMatch(styles, /\.titlebar--chat\s+\.titlebar__nav/);

assert.match(chatSource, /api\.getDefaultContextSize\(\)/);
assert.match(chatSource, /fallbackCtxSize=\{serverDefaultCtxSize\}/);
assert.match(effectiveSource, /positiveContextSize\(loadedContextSize\)/);
assert.match(effectiveSource, /positiveContextSize\(effective\?\.options\?\.ctx_size\)/);
assert.match(effectiveSource, /positiveContextSize\(resolvedContextRaw\)/);
assert.match(effectiveSource, /\{contextSetting\.value\}/);
assert.doesNotMatch(effectiveSource, /'Not loaded'/);

const runtimePosition = effectiveSource.indexOf('positiveContextSize(loadedContextSize)');
const serverPosition = effectiveSource.indexOf('positiveContextSize(effective?.options?.ctx_size)');
const localPosition = effectiveSource.indexOf('positiveContextSize(resolvedContextRaw)');
assert.ok(runtimePosition >= 0 && runtimePosition < serverPosition && serverPosition < localPosition,
  'context resolution priority must remain runtime, server-effective, then local configuration');

assert.match(styles, /\.detail-configuration__context-number\s*\{[^}]*position:\s*relative;[^}]*width:\s*9\.5rem;/s);
assert.match(styles, /\.detail-configuration__context-input\s*\{[^}]*padding-inline-end:\s*2\.75rem;[^}]*font-variant-numeric:\s*tabular-nums;/s);
assert.match(styles, /\.input\.detail-configuration__context-input\[type='number'\]\s*\{[^}]*-moz-appearance:\s*textfield\s*!important;[^}]*appearance:\s*textfield\s*!important;/s);
assert.match(styles, /\.input\.detail-configuration__context-input\[type='number'\]::\-webkit-outer-spin-button,[\s\S]*?\.input\.detail-configuration__context-input\[type='number'\]::\-webkit-inner-spin-button\s*\{[^}]*display:\s*none\s*!important;[^}]*-webkit-appearance:\s*none\s*!important;/s);
assert.match(styles, /\.detail-configuration__context-stepper\s*\{[^}]*inset-inline-end:\s*5px;[^}]*border-inline-start:/s);
assert.match(styles, /\.detail-configuration__select\s*\{[^}]*margin-top:\s*0;/s);

assert.match(detailSource, /const loadedCtxSize = positiveCtxValue\(loadedModel\?\.recipe_options\?\.ctx_size\)/);
assert.match(detailSource, /const baseCtxSize = loadedCtxSize/);
assert.match(detailSource, /const stepContextSize = \(direction: -1 \| 1\) =>/);
assert.match(detailSource, /className="detail-configuration__context-number"/);
assert.match(detailSource, /className="detail-configuration__context-stepper"/);
assert.match(detailSource, /aria-label=\{`Increase context size by \$\{ctxStep\} tokens`\}/);
assert.match(detailSource, /aria-label=\{`Decrease context size by \$\{ctxStep\} tokens`\}/);
assert.match(detailSource, /import \{ TTS_VOICES \}/);
assert.match(detailSource, /knownVoiceOptionsForModel/);
assert.match(detailSource, /<option value=\{customVoiceSentinel\}>Custom voice…<\/option>/);
assert.match(detailSource, /placeholder="Enter custom voice ID"/);

assert.doesNotMatch(apiSource, /save_options:\s*true/, 'ordinary GUI loads must never persist model options');
assert.match(apiSource, /\.\.\.recipeOptions,[\s\S]*save_options:\s*false/, 'save_options=false must come after caller options');
const callerOptionsPosition = apiSource.indexOf('...recipeOptions');
const noPersistPosition = apiSource.indexOf('save_options: false', callerOptionsPosition);
assert.ok(callerOptionsPosition >= 0 && noPersistPosition > callerOptionsPosition,
  'ordinary GUI loads must force non-persistence after caller-provided recipe options');

assert.match(effectiveSource, /api\.getModelOptions\(modelName\)/, 'effective settings must read server-owned model options');
assert.match(effectiveSource, /serverModelOptions\?\.effective\?\.ctx_size/, 'effective settings must resolve context from the server');
assert.doesNotMatch(effectiveSource, /loadModelTuning\(modelName\)\?\.recipe_options\?\.ctx_size/, 'effective settings must not read recipe options from browser storage');

assert.match(chatSource, /api\.getModelOptions\(currentModel\)/, 'chat generation defaults must use server-owned model options');
assert.match(chatSource, /api\.getModelOptions\(modelName\)/, 'pinned TTS must use server-owned model options');
assert.match(chatSource, /api\.getModelOptions\(model\.name\)/, 'capability TTS must use server-owned model options');
assert.doesNotMatch(chatSource, /loadModelTuning\s*\([^)]*\)[^;\n]*recipe_options/, 'chat must not read recipe options from browser model tuning state');




// GUI3 server-defined recipe metadata contract v3.
assert.doesNotMatch(backendManagerSource, /RECIPE_CAPABILITY/, 'backend sections must not enumerate recipes');
assert.doesNotMatch(backendManagerSource, /backendSupportsArgs/, 'backend Args availability must not use a recipe allowlist');
assert.match(backendManagerSource, /llamacpp:\s+'llama\.cpp'/,
  'functional recipe metadata refactor must preserve existing backend presentation labels');
assert.match(backendManagerSource, /const label = `\$\{RECIPE_LABELS\[recipe\] \|\| recipe\} · \$\{backend \|\| 'default'\}`;/,
  'backend Args dialog must preserve the established accessible recipe label');
assert.match(backendManagerSource, /const engineName = RECIPE_LABELS\[recipe\] \|\| recipe;/,
  'backend Args trigger must preserve the established accessible recipe label');
assert.match(backendManagerSource, /const canEditArgs = backendArgsTarget\(runtimeConfig, cellKey\) !== null;/,
  'backend Args availability must require the concrete writable runtime-config target');
assert.match(backendManagerSource,
  /const hasPerBackendArgs = Object\.keys\(section\)\.some\(key => key\.endsWith\('_args'\)\);[\s\S]*else if \(!hasPerBackendArgs && Object\.prototype\.hasOwnProperty\.call\(section, 'args'\)\)/,
  'final #3183 per-backend args safety rule must remain intact');
assert.doesNotMatch(detailSource, /IMAGE_RECIPE_KEYS|recipeKeysForRecipe\(|fallbackBackendsForRecipe\(/,
  'Model Configuration must not map an unknown recipe onto a frontend recipe table');
assert.match(detailSource, /recipeOptionNames\(info, recipe\)/,
  'Model Configuration fields must come from recipes[].options[]');
assert.match(detailSource, /recipeBackendOptionName\(systemInfo, activeRecipe\)/,
  'device options must resolve their owning backend field from recipe metadata');
assert.match(detailSource, /api\.getModelOptions\(name\)/,
  'Model Configuration must keep reading model-specific defaults from lemond');
assert.match(detailSource, /const baseValue = serverEffectiveRecipeOptions\[key\] \?\? baseTuning\.recipe_options\[key\];/,
  'server-effective model defaults must win over frontend fallback values');

assert.doesNotMatch(modelListSource, /BACKEND_MANAGED_RECIPES|BACKEND_OPTION_FIELD/,
  'model readiness must not enumerate recipe ids or backend option names');
assert.match(modelListSource, /recipeBackendOptionName\(systemInfo, recipe\)/,
  'model readiness must discover the backend field from system-info');
assert.match(modelConfigurationSource, /export type RecipeName = string;/,
  'recipe ids must be an open server-owned set');
assert.doesNotMatch(modelConfigurationSource, /BACKEND_ARGS_FIELD_BY_RECIPE|BACKEND_FIELD_BY_RECIPE/,
  'backend option resolution must not keep per-recipe compatibility maps');
assert.match(modelConfigurationSource, /backendArgsFieldForRecipe\(backendTuning\.recipe, systemInfo\)/,
  'model backend args must resolve their option name from system-info');

const metadataCompiled = ts.transpileModule(recipeMetadataSource, {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  fileName: recipeMetadataPath,
  reportDiagnostics: true,
});
const metadataErrors = (metadataCompiled.diagnostics || []).filter(
  diagnostic => diagnostic.category === ts.DiagnosticCategory.Error,
);
assert.equal(metadataErrors.length, 0,
  metadataErrors.map(diagnostic => ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n')).join('\n'));
const metadataModule = { exports: {} };
new Function('module', 'exports', metadataCompiled.outputText)(metadataModule, metadataModule.exports);
const metadata = metadataModule.exports;

const recipeFixture = {
  recipes: {
    thenoise: {
      modality: 'Image generation',
      options: [
        { name: 'thenoise_backend', type_name: 'BACKEND', help: 'TheNoise backend to use' },
        { name: 'steps', type_name: 'SIZE', help: 'Number of denoising steps' },
        { name: 'cfg_scale', type_name: 'SIZE', help: 'CFG scale' },
        { name: 'width', type_name: 'SIZE', help: 'Output image width' },
        { name: 'height', type_name: 'SIZE', help: 'Output image height' },
        { name: 'sampler', type_name: 'ARGS', help: 'Denoising solver' },
        { name: 'negative_prompt', type_name: 'ARGS', help: 'Negative prompt' },
        { name: 'qwen_vae_enhance', type_name: 'BOOL', help: 'Nyquist notch post-filter' },
        { name: 'film_grain', type_name: 'SIZE', help: 'Film grain strength' },
        { name: 'sharpening', type_name: 'SIZE', help: 'RCAS sharpening strength' },
        { name: 'lora_specs', type_name: 'ARGS', help: 'Comma-separated LoRA specs' },
      ],
    },
    futurellm: {
      modality: 'Text generation',
      options: [
        { name: 'futurellm_backend', type_name: 'BACKEND' },
        { name: 'futurellm_args', type_name: 'ARGS' },
      ],
    },
  },
};
assert.equal(metadata.recipeCapability(recipeFixture, 'thenoise'), 'Image');
assert.deepEqual(metadata.recipeOptionNames(recipeFixture, 'thenoise'),
  ['thenoise_backend', 'steps', 'cfg_scale', 'width', 'height', 'sampler', 'negative_prompt',
    'qwen_vae_enhance', 'film_grain', 'sharpening', 'lora_specs']);
assert.equal(metadata.recipeOptionIsBackend(recipeFixture, 'thenoise', 'thenoise_backend'), true);
assert.equal(metadata.recipeOptionIsBoolean(recipeFixture, 'thenoise', 'qwen_vae_enhance'), true);
assert.equal(metadata.recipeOptionIsNumeric(recipeFixture, 'thenoise', 'film_grain'), true);
assert.equal(metadata.recipeOptionIsNumeric(recipeFixture, 'thenoise', 'sharpening'), true);
assert.equal(metadata.recipeOptionIsArgs(recipeFixture, 'thenoise', 'lora_specs'), false,
  'generic ARGS-valued recipe options are not backend argv fields');
assert.equal(metadata.recipeOptionIsArgs(recipeFixture, 'futurellm', 'futurellm_args'), true,
  'a future backend *_args field is discovered without a frontend recipe entry');
assert.equal(metadata.recipeCapability({ recipes: { llamacpp: { backends: {} } } }, 'llamacpp'), 'Other',
  'missing descriptor modality must not be hidden by a frontend recipe fallback');
assert.deepEqual(metadata.recipeOptionNames({ recipes: { llamacpp: { backends: {} } } }, 'llamacpp'), [],
  'missing descriptor options must not be hidden by a frontend recipe fallback');
assert.equal(metadata.recipeCapability({ recipes: { strange: { modality: 'New modality' } } }, 'strange'), 'Other',
  'unknown server modalities must not silently fall back to LLM');


// GUI3 server-defined recipe metadata contract v4.
assert.doesNotMatch(backendManagerSource, /RECIPE_CAPABILITY/, 'backend sections must not enumerate recipes');
assert.doesNotMatch(backendManagerSource, /backendSupportsArgs/, 'backend Args availability must not use a recipe allowlist');
assert.match(backendManagerSource, /llamacpp:\s+'llama\.cpp'/,
  'functional recipe metadata refactor must preserve existing backend presentation labels');
assert.match(backendManagerSource, /const label = `\$\{RECIPE_LABELS\[recipe\] \|\| recipe\} · \$\{backend \|\| 'default'\}`;/,
  'backend Args dialog must preserve the established accessible recipe label');
assert.match(backendManagerSource, /const engineName = RECIPE_LABELS\[recipe\] \|\| recipe;/,
  'backend Args trigger must preserve the established accessible recipe label');
assert.match(backendManagerSource, /const canEditArgs = backendArgsTarget\(runtimeConfig, cellKey\) !== null;/,
  'backend Args availability must require the concrete writable runtime-config target');
assert.match(backendManagerSource,
  /const hasPerBackendArgs = Object\.keys\(section\)\.some\(key => key\.endsWith\('_args'\)\);[\s\S]*else if \(!hasPerBackendArgs && Object\.prototype\.hasOwnProperty\.call\(section, 'args'\)\)/,
  'final #3183 per-backend args safety rule must remain intact');
assert.doesNotMatch(detailSource, /IMAGE_RECIPE_KEYS|recipeKeysForRecipe\(|fallbackBackendsForRecipe\(/,
  'Model Configuration must not map an unknown recipe onto a frontend recipe table');
assert.match(detailSource, /recipeOptionNames\(info, recipe\)/,
  'Model Configuration fields must come from recipes[].options[]');
assert.match(detailSource, /recipeBackendOptionName\(systemInfo, activeRecipe\)/,
  'device options must resolve their owning backend field from recipe metadata');
assert.match(detailSource, /const TUNING_FIELD_LABELS:/,
  'known field labels remain presentation overrides, not functional recipe discovery');
assert.match(detailSource, /const TUNING_FIELD_HINTS:/,
  'known field help remains presentation-only while new fields use server metadata');
assert.match(detailSource, /TUNING_FIELD_LABELS\[key\] \|\| recipeOptionLabel\(systemInfo, activeRecipe, String\(key\)\)/,
  'known labels must win while unknown fields still render from server metadata');
assert.match(detailSource, /api\.getModelOptions\(name\)/,
  'Model Configuration must keep reading model-specific defaults from lemond');
assert.match(detailSource, /const baseValue = serverEffectiveRecipeOptions\[key\] \?\? baseTuning\.recipe_options\[key\];/,
  'server-effective model defaults must win over frontend fallback values');

assert.doesNotMatch(modelListSource, /BACKEND_MANAGED_RECIPES|BACKEND_OPTION_FIELD/,
  'model readiness must not enumerate recipe ids or backend option names');
assert.match(modelListSource, /recipeBackendOptionName\(systemInfo, recipe\)/,
  'model readiness must discover the backend field from system-info');
assert.match(modelConfigurationSource, /export type RecipeName = string;/,
  'recipe ids must be an open server-owned set');
assert.doesNotMatch(modelConfigurationSource, /BACKEND_ARGS_FIELD_BY_RECIPE|BACKEND_FIELD_BY_RECIPE/,
  'backend option resolution must not keep per-recipe compatibility maps');
assert.match(modelConfigurationSource, /backendArgsFieldForRecipe\(backendTuning\.recipe, systemInfo\)/,
  'model backend args must resolve their option name from system-info');
assert.match(modelConfigurationSource, /selectable_backend === true/,
  'backend field discovery may derive <recipe>_backend only from the server selectable_backend contract');
assert.doesNotMatch(modelConfigurationSource, /capability === 'image' \|\| recipe === 'sd-cpp'/,
  'an unknown image recipe must never inherit sd-cpp-only fields');
assert.doesNotMatch(modelConfigurationSource, /capability === 'audio' \|\| recipe === 'whispercpp'/,
  'an unknown audio recipe must never inherit whispercpp-only fields');

const metadataCompiled = ts.transpileModule(recipeMetadataSource, {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  fileName: recipeMetadataPath,
  reportDiagnostics: true,
});
const metadataErrors = (metadataCompiled.diagnostics || []).filter(
  diagnostic => diagnostic.category === ts.DiagnosticCategory.Error,
);
assert.equal(metadataErrors.length, 0,
  metadataErrors.map(diagnostic => ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n')).join('\n'));
const metadataModule = { exports: {} };
new Function('module', 'exports', metadataCompiled.outputText)(metadataModule, metadataModule.exports);
const metadata = metadataModule.exports;

const recipeFixture = {
  recipes: {
    thenoise: {
      modality: 'Image generation',
      options: [
        { name: 'thenoise_backend', type_name: 'BACKEND', help: 'TheNoise backend to use' },
        { name: 'steps', type_name: 'SIZE', help: 'Number of denoising steps' },
        { name: 'cfg_scale', type_name: 'SIZE', help: 'CFG scale' },
        { name: 'width', type_name: 'SIZE', help: 'Output image width' },
        { name: 'height', type_name: 'SIZE', help: 'Output image height' },
        { name: 'sampler', type_name: 'ARGS', help: 'Denoising solver' },
        { name: 'negative_prompt', type_name: 'ARGS', help: 'Negative prompt' },
        { name: 'qwen_vae_enhance', type_name: 'BOOL', help: 'Nyquist notch post-filter' },
        { name: 'film_grain', type_name: 'SIZE', help: 'Film grain strength' },
        { name: 'sharpening', type_name: 'SIZE', help: 'RCAS sharpening strength' },
        { name: 'lora_specs', type_name: 'ARGS', help: 'Comma-separated LoRA specs' },
      ],
    },
    futurellm: {
      modality: 'Text generation',
      options: [
        { name: 'futurellm_backend', type_name: 'BACKEND' },
        { name: 'futurellm_args', type_name: 'ARGS' },
      ],
    },
    derivedbackend: {
      modality: 'Text generation',
      selectable_backend: true,
      options: [],
    },
  },
};
assert.equal(metadata.recipeCapability(recipeFixture, 'thenoise'), 'Image');
assert.deepEqual(metadata.recipeOptionNames(recipeFixture, 'thenoise'),
  ['thenoise_backend', 'steps', 'cfg_scale', 'width', 'height', 'sampler', 'negative_prompt',
    'qwen_vae_enhance', 'film_grain', 'sharpening', 'lora_specs']);
assert.equal(metadata.recipeOptionIsBackend(recipeFixture, 'thenoise', 'thenoise_backend'), true);
assert.equal(metadata.recipeOptionIsBoolean(recipeFixture, 'thenoise', 'qwen_vae_enhance'), true);
assert.equal(metadata.recipeOptionIsNumeric(recipeFixture, 'thenoise', 'film_grain'), true);
assert.equal(metadata.recipeOptionIsNumeric(recipeFixture, 'thenoise', 'sharpening'), true);
assert.equal(metadata.recipeOptionIsArgs(recipeFixture, 'thenoise', 'lora_specs'), false,
  'generic ARGS-valued recipe options are not backend argv fields');
assert.equal(metadata.recipeOptionIsArgs(recipeFixture, 'futurellm', 'futurellm_args'), true,
  'a future backend *_args field is discovered without a frontend recipe entry');
assert.equal(metadata.recipeBackendOptionName(recipeFixture, 'derivedbackend'), 'derivedbackend_backend',
  'selectable_backend derives the conventional backend option without a recipe table');
assert.deepEqual(metadata.recipeOptionNames(recipeFixture, 'derivedbackend'), ['derivedbackend_backend']);
assert.equal(metadata.recipeCapability({ recipes: { llamacpp: { backends: {} } } }, 'llamacpp'), 'Other',
  'missing descriptor modality must not be hidden by a frontend recipe fallback');
assert.deepEqual(metadata.recipeOptionNames({ recipes: { llamacpp: { backends: {} } } }, 'llamacpp'), [],
  'missing descriptor options must not be hidden by a frontend recipe fallback');
assert.equal(metadata.recipeCapability({ recipes: { strange: { modality: 'New modality' } } }, 'strange'), 'Other',
  'unknown server modalities must not silently fall back to LLM');

console.log('GUI3 configuration consistency contract checks passed.');
