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
assert.match(styles, /\.detail-configuration__select\s*\{[^}]*margin-top:\s*0;/s);

assert.match(detailSource, /const loadedCtxSize = positiveCtxValue\(loadedModel\?\.recipe_options\?\.ctx_size\)/);
assert.match(detailSource, /const baseCtxSize = loadedCtxSize/);
assert.match(detailSource, /const stepContextSize = \(direction: -1 \| 1\) =>/);
assert.match(detailSource, /className="detail-configuration__context-number"/);
assert.match(detailSource, /className="detail-configuration__context-stepper"/);
assert.match(detailSource, /aria-label=\{`Increase context size by \$\{ctxStep\} tokens`\}/);
assert.match(detailSource, /aria-label=\{`Decrease context size by \$\{ctxStep\} tokens`\}/);
// Everything auto tune hides sits below the checkbox, so the heading above it
// cannot shift when the checkbox is toggled.
const autoTunePosition = detailSource.indexOf('className="detail-configuration__autotune"');
const contextNumberPosition = detailSource.indexOf('className="detail-configuration__context-number"');
const sliderRowPosition = detailSource.indexOf('className="detail-configuration__slider-row"');
assert.ok(autoTunePosition >= 0 && autoTunePosition < contextNumberPosition && contextNumberPosition < sliderRowPosition,
  'context size controls must render in checkbox, number field, slider order');

// Load must apply the configuration on screen, saved or not.
assert.match(detailSource, /loadOptionsRef\.current = buildLoadOptions;/);
assert.match(detailSource, /onClick=\{\(\) => loadWithShownConfiguration\(onLoad, model\)\}/);
assert.match(detailSource, /onClick=\{\(\) => loadWithShownConfiguration\(onPullAndLoad, model\)\}/);
assert.match(detailSource, /import \{ TTS_VOICES \}/);
assert.match(detailSource, /knownVoiceOptionsForModel/);
assert.match(detailSource, /<option value=\{customVoiceSentinel\}>Custom voice…<\/option>/);
assert.match(detailSource, /placeholder="Enter custom voice ID"/);

assert.match(apiSource, /const body: Record<string, unknown> = \{[\s\S]*model_name: modelName,[\s\S]*save_options: true,[\s\S]*\.\.\.recipeOptions,[\s\S]*\};/);
const saveDefaultPosition = apiSource.indexOf('save_options: true');
const callerOverridePosition = apiSource.indexOf('...recipeOptions', saveDefaultPosition);
assert.ok(saveDefaultPosition >= 0 && callerOverridePosition > saveDefaultPosition,
  'GUI3 loads must persist recipe options by default while preserving explicit save_options: false overrides');

console.log('GUI3 configuration consistency contract checks passed.');
