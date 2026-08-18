const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const appPath = path.join(root, 'src/App.tsx');
const chatPath = path.join(root, 'src/components/ChatView.tsx');
const effectivePath = path.join(root, 'src/components/EffectiveSettingsModal.tsx');
const detailPath = path.join(root, 'src/components/ModelDetailPanel.tsx');
const apiPath = path.join(root, 'src/api.ts');
const stylesPath = path.join(root, 'src/styles/styles.css');

const sources = new Map([
  [appPath, fs.readFileSync(appPath, 'utf8')],
  [chatPath, fs.readFileSync(chatPath, 'utf8')],
  [effectivePath, fs.readFileSync(effectivePath, 'utf8')],
  [detailPath, fs.readFileSync(detailPath, 'utf8')],
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
assert.match(detailSource, /import \{ SAMPLER_ARG_FIELDS, composeSamplerArgs, splitSamplerArgs \} from '\.\.\/samplerArgs';/);
assert.match(detailSource, /const split = useMemo\(\(\) => splitSamplerArgs\(value\), \[value\]\);/);
assert.match(detailSource, /placeholder=\{owned \? 'set below' : 'default'\}/);
// One half owns a flag or the other does; both would send it twice.
assert.match(detailSource, /const owned = split\.claimedByRest\.has\(spec\.flag\);/);
// A field lemond resolves a value for cannot be left empty: emptying every one
// of them would send no args at all, which lemond answers with those same
// defaults, and "default" would stop meaning one thing across the fields.
assert.match(detailSource, /showSamplers \? splitSamplerArgs\(fallbackArgs\)\.fields : \{\}/);
assert.match(detailSource, /if \(fallback && !\(split\.fields\[spec\.flag\] \|\| ''\)\.trim\(\)\) setSampler\(spec\.flag, fallback\);/);

// That leaves the all-default form reachable only where lemond resolves args
// no typed field covers, where it still has to say what will load.
assert.match(detailSource, /className="detail-configuration__args-fallback"/);
assert.match(styles, /\.detail-configuration__sampler-grid\s*\{/);

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

console.log('GUI3 configuration consistency contract checks passed.');
