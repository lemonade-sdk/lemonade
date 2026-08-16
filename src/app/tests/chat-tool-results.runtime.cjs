const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const modulePath = path.join(root, 'src/hooks/useChatStreaming.ts');

function loadModule() {
  let source = fs.readFileSync(modulePath, 'utf8');
  source = source
    .replace("import { useState, useEffect, useRef, useCallback } from 'react';", `
const useState = initial => [typeof initial === 'function' ? initial() : initial, () => {}];
const useEffect = () => {};
const useRef = value => ({ current: value });
const useCallback = fn => fn;`)
    .replace("import type { ChatMessage, LiveStreamStats, ChatCompletionStats } from '../api';", `
type ChatMessage = any;
type LiveStreamStats = any;
type ChatCompletionStats = any;`)
    .replace("import type { ToolCall, ToolResult } from '../tools/lemonadeTools';", `
type ToolCall = any;
type ToolResult = any;`)
    .replace("import { useI18n, type TranslationParams } from '../i18n';", `
type TranslationParams = Record<string, string | number | boolean | null | undefined>;
const useI18n = () => ({ t: (key: string) => key });`);

  const compiled = ts.transpileModule(source, {
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS, esModuleInterop: true },
    fileName: modulePath,
    reportDiagnostics: true,
  });
  assert.equal(
    (compiled.diagnostics || []).length,
    0,
    (compiled.diagnostics || []).map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'),
  );
  const module = { exports: {} };
  Function('exports', 'require', 'module', '__filename', '__dirname', compiled.outputText)(
    module.exports, require, module, modulePath, path.dirname(modulePath),
  );
  return module.exports;
}

function t(key, params = {}) {
  const simple = {
    'toolResults.fields.status': 'Status',
    'toolResults.fields.capability': 'Capability',
    'toolResults.fields.size': 'Size',
    'toolResults.fields.context': 'Context',
    'toolResults.fields.checkpoint': 'Checkpoint',
    'toolResults.fields.recipes': 'Recipes',
    'toolResults.fields.components': 'Components',
    'toolResults.fields.options': 'Options',
    'toolResults.fields.version': 'Version',
    'toolResults.fields.os': 'OS',
    'toolResults.fields.source': 'Source',
    'toolResults.fields.variant': 'Variant',
    'toolResults.fields.recipe': 'Recipe',
    'toolResults.fields.device': 'Device',
    'toolResults.fields.type': 'Type',
    'toolResults.fields.file': 'File',
    'toolResults.fields.downloads': 'Downloads',
    'toolResults.statuses.loaded': 'loaded',
    'toolResults.statuses.downloaded': 'downloaded',
    'toolResults.statuses.registry': 'registry',
    'toolResults.capabilities.chat': 'chat',
    'toolResults.capabilities.omni': 'omni',
    'toolResults.capabilities.image': 'image',
    'toolResults.capabilities.audio': 'audio / transcription',
    'toolResults.capabilities.audio-generation': 'audio generation',
    'toolResults.capabilities.tts': 'text to speech',
    'toolResults.capabilities.model3d': '3D',
    'toolResults.capabilities.embedding': 'embedding',
    'toolResults.capabilities.reranking': 'reranking',
    'toolResults.capabilities.classification': 'classification',
    'toolResults.backendStates.installed': 'installed',
    'toolResults.backendStates.installable': 'installable',
    'toolResults.backendStates.unsupported': 'unsupported',
    'toolResults.backendStates.update_required': 'update required',
    'toolResults.backendStates.update_available': 'update available',
    'toolResults.backendStates.action_required': 'action required',
    'toolResults.backendStates.unknown': 'unknown',
    'toolResults.inventoryRetrieved': 'Model inventory retrieved.',
    'toolResults.modelLoaded': 'Model loaded.',
    'toolResults.modelUnloaded': 'Model unloaded.',
    'toolResults.modelDeleted': 'Model deleted.',
    'toolResults.noModelsLoaded': 'No models are currently loaded.',
    'toolResults.downloadComplete': 'Download complete.',
    'toolResults.needsChoice': 'A choice is required.',
    'toolResults.presentingChoices': 'Presenting choices.',
    'toolResults.noBackends': 'no backends',
    'toolResults.imageGenerated': 'Image generated.',
  };
  if (key === 'toolResults.localModels') return `${params.count} local models: ${params.loaded} loaded, ${params.downloaded} downloaded`;
  if (key === 'toolResults.registryModels') return `${params.count} registry models`;
  if (key === 'toolResults.modelLoadedNamed') return `Loaded ${params.model}.`;
  if (key === 'toolResults.modelUnloadedNamed') return `Unloaded ${params.model}.`;
  if (key === 'toolResults.modelDeletedNamed') return `Deleted ${params.model}.`;
  if (key === 'toolResults.modelsLoaded') return `${params.count} models loaded`;
  if (key === 'toolResults.serverHealth') return `Server ${params.status} · ${params.count} models loaded`;
  if (key === 'toolResults.downloadCompleteModel') return `Downloaded ${params.model}.`;
  if (key === 'toolResults.needsChoiceWithOptions') return `Choose one: ${params.choices}`;
  if (key === 'toolResults.systemInfo') return `System info: ${params.recipes} recipes, ${params.backends} installed backends.`;
  if (key === 'toolResults.systemBackendSummary') return `Backends: ${params.installed} installed, ${params.available} installable/update, across ${params.recipes} recipes.`;
  if (key === 'toolResults.backendRecipes') return `${params.count} backend recipes: ${params.names}`;
  if (key === 'toolResults.backendDefault') return `default ${params.backend}`;
  if (key === 'toolResults.backendInstalled') return `Backend installation completed for ${params.recipe}/${params.backend}.`;
  if (key === 'toolResults.error') return `Error: ${params.message}`;
  return simple[key] || key;
}

function tDe(key, params = {}) {
  const localized = {
    'toolResults.capabilities.audio-generation': 'Audio-Erzeugung',
    'toolResults.capabilities.image': 'Bild',
    'toolResults.capabilities.classification': 'Klassifikation',
    'toolResults.backendStates.installed': 'installiert',
    'toolResults.backendStates.installable': 'installierbar',
    'toolResults.backendStates.unsupported': 'nicht unterstützt',
    'toolResults.backendStates.update_required': 'Aktualisierung erforderlich',
    'toolResults.backendStates.update_available': 'Aktualisierung verfügbar',
    'toolResults.backendStates.action_required': 'Aktion erforderlich',
    'toolResults.backendStates.unknown': 'unbekannt',
  };
  return localized[key] || t(key, params);
}

const mod = loadModule();

const rawDetail = {
  name: 'Qwen Example',
  status: 'downloaded',
  capability: 'chat',
  size: '4.2 GB',
  context_window: 32768,
  checkpoint: 'org/qwen-example',
  recipes: ['llamacpp', 'vllm'],
  collection_components: [
    { name: 'Vision Component', status: 'loaded', capability: 'image', recipes: ['sd-cpp'] },
  ],
};
const detailData = mod.structuredToolResultData('get_model_info', rawDetail);
const detailText = mod.toolResultText({
  name: 'get_model_info', result: 'old summary', resultData: detailData, status: 'done',
}, t);
assert.match(detailText, /Qwen Example/);
assert.match(detailText, /4\.2 GB/);
assert.match(detailText, /Context: 32768/);
assert.match(detailText, /Checkpoint: org\/qwen-example/);
assert.match(detailText, /Recipes: llamacpp, vllm/);
assert.match(detailText, /Components:/);
assert.match(detailText, /Vision Component/);

const loadedData = mod.structuredToolResultData('get_loaded_models', {
  loaded: [
    { model: 'alpha', recipe: 'llamacpp', device: 'vulkan', type: 'llm' },
    { model: 'beta', recipe: 'whispercpp', device: 'cpu', type: 'audio' },
  ],
});
const loadedText = mod.toolResultText({
  name: 'get_loaded_models', result: '2 models loaded', resultData: loadedData, status: 'done',
}, t);
assert.match(loadedText, /alpha/);
assert.match(loadedText, /Recipe: llamacpp/);
assert.match(loadedText, /Device: vulkan/);
assert.match(loadedText, /beta/);

const manyModels = Array.from({ length: 20 }, (_, index) => ({ name: `model-${index}`, status: 'downloaded', capability: 'chat' }));
const compact = mod.structuredToolResultData('list_models', { counts: { loaded: 0, downloaded: 20, registry: 0 }, downloaded: manyModels });
assert.equal(compact.downloaded.length, 8, 'persisted tool presentation data must stay bounded');

const pullData = mod.structuredToolResultData('pull_model', {
  status: 'needs_choice',
  source: 'huggingface',
  candidates: [{ id: 'org/model-a', downloads: 1234 }, { id: 'org/model-b', downloads: 88 }],
});
const pullText = mod.toolResultText({ name: 'pull_model', result: 'old', resultData: pullData, status: 'done' }, t);
assert.match(pullText, /org\/model-a/);
assert.match(pullText, /Downloads: 1234/);

const systemData = mod.structuredToolResultData('get_system_info', {
  os: 'TestOS',
  lemonade_version: '9.9.9',
  counts: { recipes: 4, installed_backends: 3, installable_or_update_backends: 2 },
  devices: { GPU: [{ name: 'Example GPU', details: ['16 GB VRAM'] }] },
  recipes: {
    llamacpp: {
      default_backend: 'vulkan',
      backends: {
        vulkan: { state: 'installed', version: '1.2.3' },
        cpu: { state: 'installable' },
      },
    },
  },
});
const systemText = mod.toolResultText({ name: 'get_system_info', result: 'old', resultData: systemData, status: 'done' }, t);
assert.match(systemText, /3 installed, 2 installable\/update, across 4 recipes/);
assert.match(systemText, /TestOS/);
assert.match(systemText, /Example GPU/);
assert.match(systemText, /16 GB VRAM/);
assert.match(systemText, /llamacpp/);
assert.match(systemText, /default vulkan/);
assert.match(systemText, /vulkan: installed 1\.2\.3/);
assert.match(systemText, /cpu: installable/);

const localizedCapabilityData = mod.structuredToolResultData('get_model_info', {
  name: 'Audio Model',
  status: 'downloaded',
  capability: 'audio-generation',
});
const localizedCapabilityText = mod.toolResultText({
  name: 'get_model_info', result: 'old', resultData: localizedCapabilityData, status: 'done',
}, tDe);
assert.match(localizedCapabilityText, /Audio-Erzeugung/,
  'known model capabilities must be localized in structured tool presentation');
assert.doesNotMatch(localizedCapabilityText, /audio-generation/,
  'known model capabilities must not leak their internal enum to localized UI');


const localizedClassificationData = mod.structuredToolResultData('get_model_info', {
  name: 'Classification Model',
  status: 'downloaded',
  capability: 'classification',
});
const localizedClassificationText = mod.toolResultText({
  name: 'get_model_info', result: 'old', resultData: localizedClassificationData, status: 'done',
}, tDe);
assert.match(localizedClassificationText, /Klassifikation/,
  'classification must be localized like every other known model capability');
assert.doesNotMatch(localizedClassificationText, /classification/,
  'the classification enum must not leak into localized tool presentation');

const systemTextDe = mod.toolResultText({
  name: 'get_system_info', result: 'old', resultData: systemData, status: 'done',
}, tDe);
assert.match(systemTextDe, /vulkan: installiert 1\.2\.3/,
  'known backend states must be localized in system-info presentation');
assert.match(systemTextDe, /cpu: installierbar/);

const futureStateData = mod.structuredToolResultData('list_backends', {
  recipes: { experimental: { backends: { future: { state: 'future_server_state' } } } },
});
const futureStateText = mod.toolResultText({
  name: 'list_backends', result: 'old', resultData: futureStateData, status: 'done',
}, tDe);
assert.match(futureStateText, /future_server_state/,
  'unknown future backend states must remain visible as raw diagnostic data');

assert.equal(
  mod.toolResultText({ name: 'generate_image', result: 'Bild erzeugt.', status: 'done' }, t),
  'Image generated.',
  'known media results must be rendered from the current locale instead of a persisted translated string',
);
assert.equal(
  mod.toolResultText({ name: 'get_model_info', result: 'old', errorMessage: 'backend failed', status: 'error' }, t),
  'Error: backend failed',
);

console.log('Chat tool-result presentation checks passed.');
