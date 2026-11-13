'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const read = relative => fs.readFileSync(path.join(root, relative), 'utf8');
const settings = read('src/features/adaptivePrompts/adaptivePromptSettings.ts');
const panel = read('src/components/AdaptivePromptSettingsPanel.tsx');
const chatHook = read('src/hooks/useChatStreaming.ts');
const guiSettings = read('src/components/GlobalModelSettingsPanel.tsx');
const api = read('src/api.ts');
const catalog = JSON.parse(read('../cpp/resources/adaptive_prompts.example.json'));

assert.equal(catalog.version, 1);
assert.ok(catalog.modules.length >= 200, 'ship a visible, substantive prompt library');
assert.equal(new Set(catalog.modules.map(m => m.id)).size, catalog.modules.length);
assert.ok(catalog.modules.some(m => m.id === 'reviewer' && m.type === 'role'));
assert.ok(catalog.modules.some(m => m.id === 'tribology' && m.type === 'skill'));
for (const module of catalog.modules) {
  assert.ok(module.prompt.length >= 30 && module.prompt.length <= 4096, module.id);
  assert.ok(module.keywords.length > 0, module.id);
}
const compiledCatalog = read('../cpp/server/services/adaptive_prompt_builtin.h');
const embedded = compiledCatalog.match(/R"adaptive_catalog\(\s*([\s\S]*?)\s*\)adaptive_catalog"/);
assert.ok(embedded, 'the embedded catalog must be readable');
assert.deepEqual(JSON.parse(embedded[1]), catalog, 'built-in and editable catalogs must match');
assert.equal(catalog.modules.find(m => m.id === 'concise_answer').group, 'answer-detail');
assert.equal(catalog.modules.find(m => m.id === 'detailed_explanation').group, 'answer-detail');
assert.match(settings, /export function updateManualAdaptiveIds\(/);
assert.match(settings, /picked\.excludes\?\.includes\(previousId\)/);
assert.match(panel, /updateManualAdaptiveIds\(value\.manualIds/);
assert.match(chatHook, /previous_ids: previousIds/, 'carry chat-specific IDs into next request');
assert.match(chatHook, /adaptiveSelectionsRef\.current\.set\(convoId/, 'remember only this conversation');
assert.match(settings, /enabled: false/, 'opt-in defaults off');
assert.match(settings, /if \(!settings.enabled\) return null;/, 'default sends no extension');
assert.match(chatHook, /adaptivePromptRequest\(loadAdaptivePromptSettings\(\)\)/);
assert.match(chatHook, /mode: 'manual', ids: selectedAdaptiveIds/, 'hold selection through tool calls');
assert.match(chatHook, /onAdaptiveModules: ids =>/, 'selection trace callback');
assert.match(guiSettings, /saveAdaptivePromptSettings\(adaptiveDraft\)/);
assert.match(guiSettings, /<AdaptivePromptSettingsPanel/);
assert.match(panel, /api\.listAdaptivePrompts\(\)/, 'do not duplicate the catalog in the UI');
assert.match(panel, /model\.downloaded !== true/, 'selection must offer downloaded models');
assert.match(panel, /compatibleClassifier/, 'zero-shot or systemone classifiers must be eligible');
assert.match(guiSettings, /models=\{models\}/, 'settings must pass the full model catalog');
assert.match(panel, /Show instructions/, 'user can inspect real prompts');
assert.match(api, /X-Lemonade-Adaptive-Modules/, 'server tracing must reach GUI');
console.log('Adaptive prompt GUI opt-in, catalog, tracing and settings wiring passed.');
