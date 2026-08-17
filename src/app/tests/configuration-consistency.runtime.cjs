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
