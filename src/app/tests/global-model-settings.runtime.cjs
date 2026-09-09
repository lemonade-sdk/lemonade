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

assert.deepEqual(settings.DEFAULT_GLOBAL_MODEL_SETTINGS, { collapseThinkingByDefault: true });

storage.set('lemonade:global_model_settings', JSON.stringify({
  collapseThinkingByDefault: false,
  resourceBudgetMode: 'vram',
  resourceBudgetGb: 24,
  loadingPolicy: 'single-active',
  evictionPolicy: 'largest',
  autoEvictOnPressure: true,
  protectPinnedModels: false,
  automaticModelUpdates: true,
  lastAutomaticUpdateAt: '2026-08-01T00:00:00.000Z',
}));
assert.deepEqual(settings.loadGlobalModelSettings(), { collapseThinkingByDefault: false });
assert.deepEqual(
  JSON.parse(storage.get('lemonade:global_model_settings')),
  { collapseThinkingByDefault: false },
  'legacy lifecycle fields must be dropped instead of migrated into server settings',
);

const saved = settings.saveGlobalModelSettings({
  collapseThinkingByDefault: true,
  resourceBudgetGb: 99,
  loadingPolicy: 'budget-aware',
  automaticModelUpdates: true,
});
assert.deepEqual(saved, { collapseThinkingByDefault: true });
assert.deepEqual(JSON.parse(storage.get('lemonade:global_model_settings')), { collapseThinkingByDefault: true });

assert.deepEqual(settings.memoryRuntimeSettingsFromConfig({
  max_loaded_models: -1,
  auto_evict: true,
  auto_evict_threshold_pct: 0.875,
}), {
  maxLoadedModels: -1,
  autoEvict: true,
  autoEvictThresholdPercent: 87.5,
});
assert.deepEqual(settings.memoryRuntimeSettingsFromConfig({ max_loaded_models: 3 }), {
  maxLoadedModels: 3,
  autoEvict: false,
  autoEvictThresholdPercent: 90,
});
assert.deepEqual(settings.memoryRuntimeConfigChanges({
  maxLoadedModels: 4,
  autoEvict: true,
  autoEvictThresholdPercent: 92.5,
}), {
  max_loaded_models: 4,
  auto_evict: true,
  auto_evict_threshold_pct: 0.925,
});
assert.throws(() => settings.memoryRuntimeSettingsFromConfig({ max_loaded_models: 0 }), /positive integer/);
assert.throws(() => settings.memoryRuntimeSettingsFromConfig({ max_loaded_models: -2 }), /positive integer/);
assert.throws(() => settings.memoryRuntimeSettingsFromConfig({ max_loaded_models: 1, auto_evict_threshold_pct: 0 }), /greater than 0%/);
assert.throws(() => settings.memoryRuntimeConfigChanges({ maxLoadedModels: 1, autoEvict: true, autoEvictThresholdPercent: 101 }), /at most 100%/);
assert.equal(settings.autoCheckModelUpdatesFromConfig({ auto_check_model_updates: false }), false);
assert.equal(settings.autoCheckModelUpdatesFromConfig({ auto_check_model_updates: true }), true);
assert.equal(settings.autoCheckModelUpdatesFromConfig({}), true);

const listSource = fs.readFileSync(path.join(root, 'src/components/ModelListPanel.tsx'), 'utf8');
const managerSource = fs.readFileSync(path.join(root, 'src/components/ModelManager.tsx'), 'utf8');
const panelSource = fs.readFileSync(path.join(root, 'src/components/GlobalModelSettingsPanel.tsx'), 'utf8');
const connectSource = fs.readFileSync(path.join(root, 'src/components/ConnectView.tsx'), 'utf8');
const dashboardSource = fs.readFileSync(path.join(root, 'src/components/Dashboard.tsx'), 'utf8');
const navigationSource = fs.readFileSync(path.join(root, 'src/features/navigation/workspaceNavigation.ts'), 'utf8');
const stylesSource = fs.readFileSync(path.join(root, 'src/styles/styles.css'), 'utf8');
const chatSource = fs.readFileSync(path.join(root, 'src/components/ChatView.tsx'), 'utf8');
const settingsSource = fs.readFileSync(path.join(root, 'src/features/modelSettings/globalModelSettings.ts'), 'utf8');

assert.doesNotMatch(listSource, /onOpenGlobalSettings|Open global model settings|Global model settings/);
assert.match(listSource, /onUpdateAllModels && \([\s\S]*?icon="rotate-ccw"[\s\S]*?Update all downloaded models/);
assert.doesNotMatch(managerSource, /showGlobalSettings|<GlobalModelSettingsPanel|loadWithGlobalModelPolicy|automaticUpdateIsDue|lastAutomaticUpdateAt/);
assert.match(managerSource, /await loadModelRuntime\(model\)/);
assert.match(managerSource, /handleUpdateAllModels/);

for (const label of [
  'Chat history',
  'Memory management',
  'Loading and eviction',
  'Automatic eviction',
  'Eviction threshold',
  'Maximum loaded models per type',
  'Collapse thinking by default',
  'Default TTS model',
  'Check for model updates on server startup',
]) {
  assert.ok(panelSource.includes(label), `settings panel is missing ${label}`);
}
for (const removedLabel of [
  'Memory budget',
  'Budget source',
  'Auto-evict on memory or VRAM pressure',
  'Loading policy',
  'Keep loaded models',
  'Single active model',
  'Stay within budget',
  'Largest first',
  'Oldest process first',
  'Manual only',
  'Protect pinned models from automatic eviction',
  'Automatic model updates',
]) {
  assert.ok(!panelSource.includes(removedLabel), `removed setting is still present: ${removedLabel}`);
}
assert.match(panelSource, /api\.getRuntimeConfig\(\)/);
assert.match(panelSource, /api\.setRuntimeConfig\(memoryRuntimeConfigChanges\(memoryDraft\)\)/);
assert.match(panelSource, /api\.setRuntimeConfig\(\{ auto_check_model_updates: autoCheckModelUpdates \}\)/);
assert.match(panelSource, /Failed to save settings/);
assert.match(panelSource, /await refetchServerDraft\(\)/);
assert.match(panelSource, /Discard changes/);
assert.match(panelSource, /memoryDraft\?\.autoEvict && \(/);
assert.match(panelSource, /detail-configuration__context-stepper/);
assert.match(panelSource, /autoEvictThresholdPercent \+ direction \* 5/);
assert.match(panelSource, /step=\{5\}/);
assert.match(panelSource, /current < 1 \? 1 : current \+ 1/);
assert.match(panelSource, /current <= 1 \? -1 : current - 1/);
assert.match(panelSource, /next === -1 \|\| next > 0/);
assert.match(panelSource, /Kokoro · English/);
assert.match(panelSource, /OpenMOSS · Multilingual/);
assert.match(panelSource, /section === 'chat'/);
assert.match(panelSource, /section === 'memory'/);
assert.match(panelSource, /section === 'updates'/);
assert.match(connectSource, /<GlobalModelSettingsPanel section="chat"[\s\S]*?connected=\{status === 'connected'\}/);
assert.match(connectSource, /<GlobalModelSettingsPanel section="memory"[\s\S]*?connected=\{status === 'connected'\}/);
assert.match(connectSource, /<GlobalModelSettingsPanel section="updates"[\s\S]*?connected=\{status === 'connected'\}/);
assert.match(navigationSource, /defineSection\('chat',[\s\S]*?defineSection\('memory'/);
assert.match(stylesSource, /\.connect__section > \.global-model-settings__body\s*\{[\s\S]*?width:\s*min\(var\(--content-form-width\), 100%\);/);
assert.match(chatSource, /defaultThinkingOpen=\{!globalModelSettings\.collapseThinkingByDefault\}/);
assert.match(chatSource, /GLOBAL_MODEL_SETTINGS_EVENT/);
assert.doesNotMatch(chatSource, /loadWithGlobalModelPolicy|isMemoryPressureError|evictionPlanForLoad/);
assert.match(chatSource, /return api\.loadModel\(modelName, recipeOptions, target\)/);
assert.match(dashboardSource, /evictionRuntimeSettingsFromConfig/);
assert.match(dashboardSource, /eviction\.autoEvict \? eviction\.autoEvictThresholdPercent : null/);
assert.match(dashboardSource, /dash2-gauge__threshold/);
assert.match(dashboardSource, /thresholdPercent=\{memoryTopology\.hostTotalGb != null \? evictionThresholdPercent : null\}/);
assert.match(dashboardSource, /thresholdPercent=\{memoryTopology\.gpuTotalGb != null \? evictionThresholdPercent : null\}/);
assert.match(dashboardSource, /memoryTopology\.hostTotalGb == null \? evictionThresholdText : null/);
assert.match(dashboardSource, /memoryTopology\.gpuTotalGb == null \? evictionThresholdText : null/);
assert.match(dashboardSource, /label="RAM \/ VRAM"/);
assert.match(stylesSource, /\.dash2-gauge__threshold line/);

const memoryTopologyFilename = path.join(root, 'src/features/dashboard/memoryTopology.ts');
const memoryTopologyCompiled = ts.transpileModule(fs.readFileSync(memoryTopologyFilename, 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
  fileName: memoryTopologyFilename,
  reportDiagnostics: true,
});
assert.equal((memoryTopologyCompiled.diagnostics || []).length, 0);
const memoryTopologyModule = { exports: {} };
Function('exports', 'require', 'module', '__filename', '__dirname', memoryTopologyCompiled.outputText)(
  memoryTopologyModule.exports,
  require,
  memoryTopologyModule,
  memoryTopologyFilename,
  path.dirname(memoryTopologyFilename),
);
assert.deepEqual(memoryTopologyModule.exports.dashboardMemoryTopology({
  'Physical Memory': '32 GB',
  devices: {
    amd_igpu: { available: true, integrated: true, vram_gb: 4, virtual_mem_gb: 32 },
  },
}), { unified: true, hostTotalGb: 32, gpuTotalGb: null });

for (const removedField of [
  'resourceBudgetMode',
  'resourceBudgetGb',
  'loadingPolicy',
  'evictionPolicy',
  'autoEvictOnPressure',
  'protectPinnedModels',
  'automaticModelUpdates',
  'lastAutomaticUpdateAt',
]) {
  assert.ok(!settingsSource.includes(removedField), `server-owned field remains in local settings module: ${removedField}`);
}

console.log('Global model settings use server lifecycle config and keep only client-owned local preferences.');
