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
const samplerPath = path.join(root, 'src/samplerArgs.ts');
const managerPath = path.join(root, 'src/components/ModelManager.tsx');
const globalSettingsPath = path.join(root, 'src/features/modelSettings/globalModelSettings.ts');
const storagePath = path.join(root, 'src/storage.ts');
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
  [samplerPath, fs.readFileSync(samplerPath, 'utf8')],
  [managerPath, fs.readFileSync(managerPath, 'utf8')],
  [globalSettingsPath, fs.readFileSync(globalSettingsPath, 'utf8')],
  [storagePath, fs.readFileSync(storagePath, 'utf8')],
]);
const styles = fs.readFileSync(stylesPath, 'utf8');
const samplerSource = sources.get(samplerPath);

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
const managerSource = sources.get(managerPath);
const globalSettingsSource = sources.get(globalSettingsPath);
const storageSource = sources.get(storagePath);

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
// A control's value is regular weight everywhere in this panel; semibold is
// what marks the field headings, and a semibold select outweighed its own.
assert.match(styles, /\.detail-configuration__select\s*\{[^}]*margin-top:\s*0;[^}]*font-weight:\s*var\(--weight-regular\);/s);

assert.match(detailSource, /const loadedCtxSize = positiveCtxValue\(loadedModel\?\.recipe_options\?\.ctx_size\)/);
assert.match(detailSource, /const baseCtxSize = positiveCtxValue\(resolvedCtxSize\)/,
  'the context field must start from the size lemond resolved for the next load');
assert.match(detailSource, /const stepContextSize = \(direction: -1 \| 1\) =>/);
assert.match(detailSource, /className="detail-configuration__context-number"/);
assert.match(detailSource, /className="detail-configuration__context-stepper"/);
assert.match(detailSource, /aria-label=\{`Increase context size by \$\{ctxStep\} tokens`\}/);
assert.match(detailSource, /aria-label=\{`Decrease context size by \$\{ctxStep\} tokens`\}/);
// The number box reads as the slider's current value, so it sits after it on
// the same row rather than stacked above it.
const autoTunePosition = detailSource.indexOf('className="detail-configuration__autotune"');
const contextRowPosition = detailSource.indexOf('className="detail-configuration__context-row"');
const contextNumberPosition = detailSource.indexOf('className="detail-configuration__context-number"');
const sliderRowPosition = detailSource.indexOf('className="detail-configuration__slider-row"');
assert.ok(autoTunePosition >= 0 && autoTunePosition < contextRowPosition
  && contextRowPosition < sliderRowPosition && sliderRowPosition < contextNumberPosition,
  'context size controls must render in checkbox, slider, number field order');
assert.match(styles, /\.detail-configuration__context-row\s*\{[^}]*display:\s*flex;[^}]*align-items:\s*center;/s);

// Load must apply the configuration on screen, saved or not.
// Only once the saved options are in hand: an empty draft would otherwise read
// as an explicit "ignore everything saved".
assert.match(detailSource, /loadOptionsRef\.current = serverOptionsLoaded \? buildLoadOptions : null;/);
assert.match(detailSource, /onClick=\{\(\) => loadWithShownConfiguration\(onLoad, model\)\}/);
assert.match(detailSource, /onClick=\{\(\) => loadWithShownConfiguration\(onPullAndLoad, model\)\}/);
// A load states every option the panel shows, sending null where a field is
// empty so lemond skips the saved value instead of falling back to it.
assert.match(detailSource, /options\[key\] = Object\.prototype\.hasOwnProperty\.call\(configured, key\) \? configured\[key\] : null;/);
assert.match(detailSource, /if \(supportsContextSize\) options\.ctx_size = configured\.ctx_size \?\? -1;/);
// The form already carries the merged args, so lemond must take them verbatim
// rather than merging the defaults in a second time underneath them.
assert.match(detailSource, /if \(sendsArgs\) options\.merge_args = false;/);
assert.match(detailSource, /await onReloadModel\(loadedModel, buildLoadOptions\(\)\)/,
  'reload must apply the settings on screen');

// Save is the only writer: loading, reloading and resetting never persist.
assert.doesNotMatch(detailSource, /saveConfig\(false\)/, 'reload must not save the draft first');
assert.doesNotMatch(detailSource, /api\.resetModelOptions/,
  'reset must restore lemond defaults into the draft, not write them');
assert.match(detailSource, /api\.saveModelOptions\(name, patch\)/, 'save must send only what changed');

// Nothing offers to un-apply lemond's defaults, because nothing can: an unset
// args key resolves back to them, so the control could only ever mislead.
assert.match(detailSource, /const sendsArgs = argsKeys\.length > 0;/);
assert.doesNotMatch(detailSource, /mergeArgs/,
  'the merge_args toggle is gone: the form shows the resolved args outright');
assert.doesNotMatch(styles, /detail-configuration__merge-args/);
assert.doesNotMatch(detailSource, /previewModelOptions/,
  'the form is the preview: there is no separate resolved card to fetch for');
assert.doesNotMatch(styles, /detail-configuration__merge-preview/);

// Sampler flags get typed fields of their own, filled from the resolved args, so
// a value lemond does not set reads as an explicit "default" rather than a gap.
assert.match(detailSource, /^\s+composeSamplerArgs,$/m);
assert.match(detailSource, /\} from '\.\.\/samplerArgs';/);
assert.match(detailSource, /const split = useMemo\(\(\) => splitSamplerArgs\(specs, value\), \[specs, value\]\);/);

// Which flags a command line spells this way belongs to the recipe, not to the
// renderer: the panel asks for the schema and draws whatever comes back, so a
// backend gains typed fields without this file naming it.
assert.match(detailSource, /specs=\{samplerFieldsForRecipe\(activeRecipeForModel\(model\)\)\}/);
assert.doesNotMatch(detailSource, /=== 'llamacpp_args'/,
  'the args renderer must not single out a recipe by name');
assert.match(samplerSource, /const SAMPLER_FIELDS_BY_RECIPE: Record<string, ArgFieldSpec\[\]> = \{/);
assert.match(detailSource, /placeholder=\{owned \? 'set below' : 'default'\}/);
// One half owns a flag or the other does; both would send it twice.
assert.match(detailSource, /const owned = split\.claimedByRest\.has\(spec\.flag\);/);
// A field lemond resolves a value for cannot be left empty: emptying every one
// of them would send no args at all, which lemond answers with those same
// defaults, and "default" would stop meaning one thing across the fields.
assert.match(detailSource, /\(\) => splitSamplerArgs\(specs, fallbackArgs\)\.fields,/);
assert.match(detailSource, /if \(fallback && !\(split\.fields\[spec\.flag\] \|\| ''\)\.trim\(\)\) setSampler\(spec\.flag, fallback\);/);

// That leaves the all-default form reachable only where lemond resolves args
// no typed field covers, where it still has to say what will load.
assert.match(detailSource, /className="detail-configuration__args-fallback"/);
assert.match(styles, /\.detail-configuration__sampler-grid\s*\{/);

// A sampler people set by feel is drawn along that feeling. Which one that is
// belongs to the recipe's schema, so the renderer asks rather than names it.
assert.doesNotMatch(detailSource, /'--temp'/,
  'the axis renderer must not single out a flag by name');
assert.match(samplerSource, /axis: \{/);
assert.match(samplerSource, /advisedMin: 0\.2,/);
assert.match(samplerSource, /advisedMax: 1\.3,/);
assert.match(detailSource, /const axisSpecs = useMemo\(\(\) => axisSamplerFields\(specs\), \[specs\]\);/);
assert.match(detailSource, /const detailSpecs = useMemo\(\(\) => detailSamplerFields\(specs\), \[specs\]\);/);
assert.match(detailSource, /'--axis-advised-start': `\$\{\(\(axis\.advisedMin - min\) \/ span\) \* 100\}%`/);
// Colour alone would carry the advisory; the shoulders are hatched as well.
assert.match(styles, /\.detail-configuration__axis-slider\s*\{[^}]*repeating-linear-gradient/s);
assert.match(styles, /\.detail-configuration__axis-slider\s*\{[^}]*var\(--axis-advised-start\)[^}]*var\(--axis-advised-end\)/s);

// The rest fold away, but never silently: the closed row states the flags it
// holds, and the browser remembers an expert's choice to keep it open.
assert.match(detailSource, /aria-expanded=\{detailsOpen\}/);
assert.match(detailSource, /storageKey\(SAMPLER_DETAILS_OPEN_KEY\)/);
assert.match(detailSource, /\{foldedFlags \|\| 'All on default'\}/);
assert.match(styles, /\.detail-configuration__sampler-grid\[hidden\]\s*\{[^}]*display:\s*none;/s);

// The detail pane narrows with the splitter, not with the window, so this
// panel's breakpoints read the pane. A window-width media query fires at sizes
// where the pane is at its widest.
assert.match(styles, /\.detail-configuration\s*\{[^}]*container-type:\s*inline-size;[^}]*container-name:\s*model-configuration;/s);
assert.match(styles, /\.detail-configuration__section\s*\{[^}]*max-width:\s*var\(--max-content-width\);/s,
  'load settings must stop where the rest of the app stops, not stretch across a wide screen');
assert.match(styles, /@container model-configuration \(max-width: 520px\)/);
assert.match(styles, /@container model-configuration \(max-width: 340px\)/);
assert.doesNotMatch(styles, /@media[^{]*\{\s*\.detail-configuration__(selector-grid|sampler-grid|context-row|axis)/s,
  'load settings breakpoints must answer to the pane, not the window');

// An unbounded string of flags in the folded summary must not widen the panel:
// grid items default to a min-content floor, and `flex-basis: auto` keeps the
// summary's own max-content in that floor.
assert.match(styles, /\.detail-configuration__sampler-folded\s*\{[^}]*flex:\s*1 1 0;[^}]*min-width:\s*0;/s);
assert.match(styles, /\.detail-configuration__sampler-toggle\s*\{[^}]*min-width:\s*0;/s);
assert.match(styles, /\.detail-configuration__args-block\s*\{[^}]*min-width:\s*0;/s);

// An empty field lands on lemond's defaults at load time, not on the effective
// value the form was seeded from, so that is what its label has to name.
assert.match(detailSource, /const baseValue = defaultOptionValue\(key\);/);
assert.match(detailSource, /serverDefaultRecipeOptions\[key\] \?\? baseTuning\.recipe_options\[key\]/);
assert.doesNotMatch(detailSource, /const baseValue = serverEffectiveRecipeOptions/,
  'Reset must not label an emptied selector with the value it just cleared');

// The draft is seeded once, into the fields the recipe's option list names, so
// the options fetch waits for the system-info that carries that list.
assert.match(detailSource, /if \(!systemInfoSettled\) return;/);
assert.match(detailSource, /\}, \[name, systemInfoSettled\]\);/);
assert.match(detailSource, /\.finally\(\(\) => \{ if \(alive\) setSystemInfoSettled\(true\); \}\);/);
assert.doesNotMatch(detailSource, /seededRef/,
  'a late option list must not be reconciled into a draft that was seeded without it');

// Reload restarts the backend, so it is offered only where it would change it.
assert.match(detailSource, /const reloadWouldChange = recipeKeys\.some\(key => \{/);
assert.match(detailSource, /loadedModel && onReloadModel && \(reloadWouldChange \? \(/);
assert.match(detailSource, /className="detail-configuration__running-state"/);

// The context controls stay on screen while auto tuning, showing what it resolved.
assert.match(detailSource, /value=\{isAutoTuning \? String\(currentCtxSize\) : ctxSizeDraft\}/);
assert.doesNotMatch(detailSource, /\{!isAutoTuning && \(/,
  'auto tuning must disable the context controls, not remove them');

// The source chip reads as a link out to the registry.
assert.match(detailSource, /className="model-detail-panel__source"\s*\n\s*emphasis="low"\s*\n\s*icon="globe"/);
assert.match(styles, /\.model-detail-panel__source > svg\s*\{[^}]*flex:\s*0 0 auto;/s);
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
assert.match(detailSource, /serverDefaultRecipeOptions\[key\] \?\? baseTuning\.recipe_options\[key\]/,
  'server-owned model defaults must win over frontend fallback values');

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
// server-owned pin contract
assert.match(apiSource, /pinned: typeof model\.pinned === 'boolean'/, 'health normalization must preserve server pin state');
assert.match(apiSource, /'\/internal\/pin'/, 'pin mutations must use the server endpoint');
assert.match(managerSource, /loadedModels\.filter\(model => model\.pinned === true\)/, 'GUI pin state must derive from loaded server models');
assert.match(managerSource, /api\.setModelPinned\(name, loaded\.pinned !== true\)/, 'GUI pin toggles must mutate server state');
assert.match(managerSource, /onToggleFavorite=\{toggleFavoriteModel\}/, 'model list secondary action must be Favorite');
assert.match(managerSource, /onTogglePin=\{selectedDetailModelId && loadedModels\.some/, 'detail Pin handler must only be supplied for server-loaded models');
assert.doesNotMatch(managerSource, /onTogglePin=\{selectedDetailModelId && displayLoadedModels\.some/, 'virtual collection entries must not expose the server Pin action');
assert.doesNotMatch(managerSource, /if \(!loaded\) return;/, 'Pin handler must not silently no-op if loaded state races the click');
assert.match(modelListSource, /ariaKeyShortcuts=\{onToggleFavorite \? 'F'/, 'model list must expose Favorite as its row shortcut');
assert.match(modelListSource, /icon: 'star'/, 'model list secondary action must render Favorite');
assert.doesNotMatch(modelListSource, /onTogglePin\?:/, 'model list must not own runtime Pin mutation');
assert.doesNotMatch(managerSource, /loadPinnedModelNames|savePinnedModelNames/, 'ModelManager must not use browser pin persistence');
assert.doesNotMatch(globalSettingsSource, /pinnedModelsKey|loadPinnedModelNames|savePinnedModelNames|pinned_models/, 'global model settings must not own pin persistence');
assert.match(storageSource, /`\$\{STORAGE_PREFIX\}pinned_models`/, 'obsolete client pin storage must be cleaned up');

// favorite hover-only row-action contract
assert.doesNotMatch(modelListSource, /latched:\s*favorited/, 'Favorite must remain hover-only so backend info is visible at rest');
assert.match(modelListSource, /active:\s*favorited/, 'favorited row action must keep active-state styling while hovered');
assert.match(styles, /focus-within:not\(\.workspace-list-row--pointer-action\)\s+\.workspace-list-row__action/, 'pointer-only row action must not remain visible from row focus alone');
assert.match(styles, /workspace-list-row__action--latched,\s*\n\.workspace-list-row__action--active\s*\{[^}]*color:\s*var\(--accent-fg\)/s, 'active row action must use the accent color without latching visibility');

console.log('GUI3 configuration consistency contract checks passed.');
