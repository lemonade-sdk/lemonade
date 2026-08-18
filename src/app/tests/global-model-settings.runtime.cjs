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
    .replace("import { storageKey } from '../../storage';", "const storageKey = (key: string) => `lemonade:${key}`;");
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

const saved = settings.saveGlobalModelSettings({
  ...settings.DEFAULT_GLOBAL_MODEL_SETTINGS,
  resourceBudgetMode: 'vram',
  resourceBudgetGb: 23.456,
  loadingPolicy: 'budget-aware',
  autoEvictOnPressure: true,
});
assert.equal(saved.resourceBudgetGb, 23.5);
assert.equal(settings.loadGlobalModelSettings().loadingPolicy, 'budget-aware');
assert.ok(storage.has('lemonade:global_model_settings'));
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
const connectSource = fs.readFileSync(path.join(root, 'src/components/ConnectView.tsx'), 'utf8');
const navigationSource = fs.readFileSync(path.join(root, 'src/features/navigation/workspaceNavigation.ts'), 'utf8');
const stylesSource = fs.readFileSync(path.join(root, 'src/styles/styles.css'), 'utf8');
const chatSource = fs.readFileSync(path.join(root, 'src/components/ChatView.tsx'), 'utf8');

assert.doesNotMatch(listSource, /onOpenGlobalSettings|Open global model settings|Global model settings/);
assert.match(listSource, /onUpdateAllModels && \([\s\S]*?icon="rotate-ccw"[\s\S]*?Update all downloaded models/);
assert.doesNotMatch(managerSource, /showGlobalSettings|<GlobalModelSettingsPanel/);
assert.match(managerSource, /loadWithGlobalModelPolicy/);
assert.match(managerSource, /handleUpdateAllModels/);
for (const label of ['Chat history', 'Memory budget', 'Loading and eviction', 'Collapse thinking by default', 'Default TTS model', 'Automatic model updates']) {
  assert.ok(panelSource.includes(label), `settings panel is missing ${label}`);
}
assert.match(panelSource, /Kokoro · English/);
assert.match(panelSource, /OpenMOSS · Multilingual/);
assert.match(panelSource, /section === 'chat'/);
assert.match(panelSource, /section === 'memory'/);
assert.match(panelSource, /section === 'updates'/);
assert.match(connectSource, /activeSection === 'chat'[\s\S]*?<GlobalModelSettingsPanel section="chat"/);
assert.match(connectSource, /activeSection === 'memory'[\s\S]*?<GlobalModelSettingsPanel section="memory"/);
assert.match(connectSource, /activeSection === 'model-storage'[\s\S]*?<GlobalModelSettingsPanel section="updates"/);
assert.match(navigationSource, /defineSection\('chat',[\s\S]*?defineSection\('memory'/);
assert.match(stylesSource, /\.connect__section > \.global-model-settings__body\s*\{[\s\S]*?width:\s*min\(var\(--content-form-width\), 100%\);/);
assert.match(chatSource, /defaultThinkingOpen=\{!globalModelSettings\.collapseThinkingByDefault\}/);
assert.match(chatSource, /GLOBAL_MODEL_SETTINGS_EVENT/);
assert.match(chatSource, /loadModelWithPolicy/);
assert.match(chatSource, /loadWithGlobalModelPolicy/);
