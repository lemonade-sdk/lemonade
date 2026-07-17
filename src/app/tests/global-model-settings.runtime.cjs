const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');

function installBrowserShim() {
  const values = new Map();
  global.localStorage = {
    getItem: key => values.has(key) ? values.get(key) : null,
    setItem: (key, value) => values.set(key, String(value)),
    removeItem: key => values.delete(key),
    clear: () => values.clear(),
  };
  global.CustomEvent = class CustomEvent {
    constructor(type, init) { this.type = type; this.detail = init?.detail; }
  };
  global.window = { dispatchEvent() {} };
  return values;
}

function loadSettingsModule() {
  const filename = path.join(root, 'src/features/modelSettings/globalModelSettings.ts');
  const source = fs.readFileSync(filename, 'utf8')
    .replace("import { scopedStorageKey } from '../accounts/accountStore';", "const scopedStorageKey = (scope: string, key: string) => `lemonade:${scope}:${key}`;");
  const compiled = ts.transpileModule(source, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS,
      esModuleInterop: true,
    },
    fileName: filename,
    reportDiagnostics: true,
  });
  assert.equal(
    (compiled.diagnostics || []).length,
    0,
    (compiled.diagnostics || []).map(diagnostic => ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n')).join('\n'),
  );
  const module = { exports: {} };
  Function('exports', 'require', 'module', '__filename', '__dirname', compiled.outputText)(
    module.exports,
    require,
    module,
    filename,
    path.dirname(filename),
  );
  return module.exports;
}

const storage = installBrowserShim();
const settings = loadSettingsModule();

assert.equal(settings.DEFAULT_GLOBAL_MODEL_SETTINGS.automaticModelUpdates, false, 'automatic updates must be opt-in');
assert.equal(settings.DEFAULT_GLOBAL_MODEL_SETTINGS.autoEvictOnPressure, false, 'pressure eviction must be opt-in');
assert.equal(settings.DEFAULT_GLOBAL_MODEL_SETTINGS.collapseThinkingByDefault, true);

const savedA = settings.saveGlobalModelSettings('account-a', {
  ...settings.DEFAULT_GLOBAL_MODEL_SETTINGS,
  resourceBudgetMode: 'vram',
  resourceBudgetGb: 23.456,
  loadingPolicy: 'budget-aware',
  autoEvictOnPressure: true,
});
assert.equal(savedA.resourceBudgetGb, 23.5);
assert.equal(settings.loadGlobalModelSettings('account-a').loadingPolicy, 'budget-aware');
assert.equal(settings.loadGlobalModelSettings('account-b').resourceBudgetMode, 'server', 'settings must remain account scoped');
assert.ok([...storage.keys()].some(key => key.includes('account-a') && key.endsWith('global_model_settings')));

const sanitized = settings.sanitizeGlobalModelSettings({
  resourceBudgetMode: 'invalid',
  resourceBudgetGb: -4,
  loadingPolicy: 'invalid',
  evictionPolicy: 'invalid',
  protectPinnedModels: false,
});
assert.equal(sanitized.resourceBudgetMode, 'server');
assert.equal(sanitized.resourceBudgetGb, 1);
assert.equal(sanitized.loadingPolicy, 'keep-loaded');
assert.equal(sanitized.evictionPolicy, 'lru');
assert.equal(sanitized.protectPinnedModels, false);

assert.equal(settings.isMemoryPressureError(new Error('CUDA out of memory while allocating VRAM')), true);
assert.equal(settings.isMemoryPressureError(new Error('model file missing')), false);
assert.equal(settings.automaticUpdateIsDue({ ...settings.DEFAULT_GLOBAL_MODEL_SETTINGS, automaticModelUpdates: true }), true);
assert.equal(settings.automaticUpdateIsDue({
  ...settings.DEFAULT_GLOBAL_MODEL_SETTINGS,
  automaticModelUpdates: true,
  lastAutomaticUpdateAt: new Date().toISOString(),
}), false);

const allModels = [
  { id: 'small', size: 2 },
  { id: 'large', size: 8 },
  { id: 'target', size: 6 },
];
const loaded = [
  { model_name: 'small', pid: 20, last_use: 200 },
  { model_name: 'large', pid: 10, last_use: 100 },
];
const largestPolicy = {
  ...settings.DEFAULT_GLOBAL_MODEL_SETTINGS,
  resourceBudgetMode: 'vram',
  resourceBudgetGb: 10,
  loadingPolicy: 'budget-aware',
  evictionPolicy: 'largest',
};
assert.deepEqual(settings.evictionPlanForLoad(loaded, allModels, allModels[2], [], largestPolicy), ['large']);
assert.deepEqual(settings.evictionPlanForLoad(loaded, allModels, allModels[2], ['large'], largestPolicy), ['small'], 'pinned models must be protected');
assert.deepEqual(settings.evictionPlanForLoad(loaded, allModels, allModels[2], [], { ...largestPolicy, evictionPolicy: 'manual' }), []);

const collectionModels = [...allModels, { id: 'bundle', components: ['target', 'small'] }];
assert.equal(settings.estimatedModelFootprintGb(collectionModels[3], collectionModels), 8, 'collection footprint must include concrete components');

const listSource = fs.readFileSync(path.join(root, 'src/components/ModelListPanel.tsx'), 'utf8');
const managerSource = fs.readFileSync(path.join(root, 'src/components/ModelManager.tsx'), 'utf8');
const panelSource = fs.readFileSync(path.join(root, 'src/components/GlobalModelSettingsPanel.tsx'), 'utf8');
const chatSource = fs.readFileSync(path.join(root, 'src/components/ChatView.tsx'), 'utf8');
const presetSource = fs.readFileSync(path.join(root, 'src/components/PresetManager.tsx'), 'utf8');

assert.match(listSource, /onOpenRouter[\s\S]*onOpenGlobalSettings[\s\S]*Icon name="settings"/, 'settings must sit beside the router action');
assert.match(managerSource, /showGlobalSettings \?[\s\S]*<GlobalModelSettingsPanel/);
assert.match(managerSource, /loadWithGlobalModelPolicy/);
assert.match(managerSource, /handleUpdateAllModels/);
for (const label of ['Memory budget', 'Loading and eviction', 'Pinned models', 'Collapse thinking by default', 'Default TTS model', 'Automatic model updates', 'Update all models now']) {
  assert.ok(panelSource.includes(label), `global settings panel is missing ${label}`);
}
assert.match(panelSource, /Kokoro · English/);
assert.match(panelSource, /OpenMOSS · Multilingual/);
assert.match(chatSource, /defaultThinkingOpen=\{!globalModelSettings\.collapseThinkingByDefault\}/);
assert.match(chatSource, /GLOBAL_MODEL_SETTINGS_EVENT/);
assert.match(chatSource, /loadModelWithPolicy/);
assert.match(chatSource, /loadWithGlobalModelPolicy/);
assert.match(presetSource, /const CAPABILITIES: Capability\[\] = \['chat', 'omni', 'vision', 'code', 'tts'\]/);
assert.match(presetSource, /TTS_VOICES\.map/);
assert.match(presetSource, /OPENMOSS_VOICE_PRESETS\.map/);
assert.match(presetSource, /VISIBLE_STARTERS/);

console.log('GUI3 global model settings, eviction, TTS preset and thinking-default checks passed.');
