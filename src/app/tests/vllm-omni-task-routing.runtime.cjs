const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');
const vm = require('node:vm');

const root = path.resolve(__dirname, '..');
const read = p => fs.readFileSync(path.join(root, p), 'utf8');
const chat = read('src/components/ChatView.tsx');
const detail = read('src/components/ModelDetailPanel.tsx');
const server = fs.readFileSync(path.resolve(root, '../cpp/server/backends/vllm/vllm_server.cpp'), 'utf8');
const models = JSON.parse(fs.readFileSync(path.resolve(root, '../cpp/resources/server_models.json'), 'utf8'));

function functionsFrom(source, filename, names, dependencies) {
  const file = ts.createSourceFile(filename, source, ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
  const selected = [];
  for (const stmt of file.statements) {
    if (ts.isFunctionDeclaration(stmt) && names.includes(stmt.name?.text)) selected.push(stmt.getText(file));
  }
  assert.equal(selected.length, names.length, `Expected functions: ${names.join(', ')}`);
  const js = ts.transpileModule(`${selected.join('\n')}\nmodule.exports = { ${names.join(', ')} };`, {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
    fileName: filename,
    reportDiagnostics: true,
  });
  const diagnostics = (js.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error);
  assert.equal(diagnostics.length, 0);
  const module = { exports: {} };
  vm.runInNewContext(js.outputText, { module, exports: module.exports, ...dependencies }, { filename });
  return module.exports;
}

const modeHelpers = functionsFrom(chat, 'ChatView.tsx', ['modelModeBadge', 'modelModeDisplayLabel'], {
  modelStructure: recipe => recipe === 'collection.omni' ? 'omni' : 'single',
  identityFor: (cap, recipe) => recipe === 'collection.omni' ? 'omni' : cap,
  capabilityBadge: cap => cap,
  capabilityLabel: cap => cap,
  modelModeLabel: (cap, audio) => audio && cap === 'chat' ? 'Chat + Audio' : cap,
});
assert.equal(modeHelpers.modelModeBadge('chat', 'vllm', true), 'chat');
assert.equal(modeHelpers.modelModeDisplayLabel('chat', true, 'vllm', true), 'Chat + Audio');
assert.equal(modeHelpers.modelModeDisplayLabel('tts', false, 'vllm', true), 'tts');
assert.equal(modeHelpers.modelModeBadge('chat', 'collection.omni', true), 'omni');
assert.equal(modeHelpers.modelModeDisplayLabel('chat', true, 'collection.omni', true), 'Omni');
assert.match(chat, /const modeSupportsMcp = modeSupportsChatCompletions && !currentIsNativeOmni/);
assert.match(chat, /capability=\{capability\}/);
assert.match(chat, /glyphs=\{option.audioInput && option.capability === 'chat'/);

const backendHelpers = functionsFrom(detail, 'ModelDetailPanel.tsx',
  ['backendIsSelectable', 'backendOptionsForKey', 'activeBackendValue'], {
    backendState: value => value?.state ?? '',
    activeRecipeForModel: model => model.recipe,
    backendMapForRecipe: (info, recipe) => info.recipes?.[recipe]?.backends || null,
    backendMatchesDetectedHardware: () => true,
    optionalDisplayValue: value => String(value || '').trim(),
    recipeDefaultBackend: (info, recipe) => info.recipes?.[recipe]?.default_backend || '',
  });
const info = { recipes: { vllm: { default_backend: 'omni-rocm', backends: {
  rocm: { state: 'installable' }, 'omni-rocm': { state: 'installed' },
} } } };
assert.equal(backendHelpers.backendIsSelectable('vllm', 'rocm', info.recipes.vllm.backends.rocm), true);
assert.equal(backendHelpers.backendIsSelectable('vllm', 'omni-rocm', info.recipes.vllm.backends['omni-rocm']), false);
assert.deepEqual(Array.from(backendHelpers.backendOptionsForKey('vllm_backend', 'omni-rocm', { recipe: 'vllm' }, info)), ['auto', 'rocm']);
assert.equal(backendHelpers.activeBackendValue('vllm_backend', undefined, { recipe: 'vllm' }, info), 'rocm');
assert.equal(backendHelpers.activeBackendValue('vllm_backend', 'omni-rocm', { recipe: 'vllm' }, info), 'rocm');

for (const name of ['Qwen3-TTS-1.7B-VoiceDesign-vLLM', 'Fish-Speech-S2-Pro-vLLM']) {
  assert.equal(models[name]?.omni_preserve_stage_context, true, name);
}
assert.equal(models['Qwen2.5-Omni-3B-vLLM']?.omni_preserve_stage_context, undefined);
assert.equal(models['Voxtral-4B-TTS-2603-vLLM'], undefined);
assert.match(server, /ctx_size > 0 && !model_info\.extra<bool>\("omni_preserve_stage_context", false\)/);
assert.match(server, /ProcessOutput\{true, true\}/);
assert.match(server, /const OmniUmaBudget memory = detect_omni_uma_budget\(\)/);
console.log('Native Omni task routing and vLLM variant selector regressions passed.');
