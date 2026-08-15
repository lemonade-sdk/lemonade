const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const modulePath = path.join(root, 'src/features/chatDefaultModels.ts');
const chatPath = path.join(root, 'src/components/ChatView.tsx');

const values = new Map();
global.localStorage = {
  getItem: key => values.has(key) ? values.get(key) : null,
  setItem: (key, value) => values.set(key, String(value)),
  removeItem: key => values.delete(key),
};

function capabilityFromModelInfo(model) {
  const labels = (model?.labels || []).map(value => String(value).toLowerCase());
  const recipe = String(model?.recipe || '').toLowerCase();
  if (recipe === 'collection.omni' || labels.includes('omni')) return 'omni';
  if (labels.includes('image')) return 'image';
  if (labels.includes('chat') || recipe === 'llamacpp') return 'chat';
  return 'unknown';
}

function loadModule() {
  const source = fs.readFileSync(modulePath, 'utf8')
    .replace("import type { ModelInfo } from '../api';", 'type ModelInfo = Record<string, any>;')
    .replace("import { capabilityFromModelInfo } from '../modelCapabilities';", `const capabilityFromModelInfo = ${capabilityFromModelInfo.toString()};`)
    .replace("import { storageKey } from '../storage';", "const storageKey = (key: string) => `lemonade:${key}`;");
  const compiled = ts.transpileModule(source, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS,
      esModuleInterop: true,
    },
    fileName: modulePath,
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
    modulePath,
    path.dirname(modulePath),
  );
  return module.exports;
}

const defaults = loadModule();
assert.deepEqual(
  defaults.LEMONADE_DEFAULT_CHAT_MODELS.map(model => [model.name, model.tier]),
  [
    ['Bonsai-8B-gguf', 'tiny'],
    ['Qwen3.5-4B-GGUF', 'quality'],
  ],
  'the two replaceable Lemonade defaults must stay centralized without presentation copy',
);
assert.equal(defaults.loadPreferredDefaultModelName(), 'Bonsai-8B-gguf');
assert.equal(defaults.savePreferredDefaultModelName('Qwen3.5-4B-GGUF'), 'Qwen3.5-4B-GGUF');
assert.equal(defaults.loadPreferredDefaultModelName(), 'Qwen3.5-4B-GGUF');

defaults.saveLastReadyModelName('local-last');
assert.equal(defaults.loadLastReadyModelName(), 'local-last');

const registry = [
  { id: 'older', recipe: 'llamacpp', labels: ['chat'], downloaded: true, last_used: 10 },
  { id: 'local-last', recipe: 'llamacpp', labels: ['chat'], downloaded: true, last_used: 5 },
  { id: 'newer', recipe: 'llamacpp', labels: ['chat'], downloaded: true, last_used: 20 },
  { id: 'remote-only', recipe: 'llamacpp', labels: ['chat'], downloaded: false, last_used: 100 },
  { id: 'image-ready', labels: ['image'], downloaded: true, last_used: 200 },
];
assert.equal(
  defaults.modelInfoName(defaults.resolveLastReadyChatModel(registry, 'local-last')),
  'local-last',
  'the remembered model wins while it remains downloaded and chat-capable',
);
assert.equal(
  defaults.modelInfoName(defaults.resolveLastReadyChatModel(registry, 'remote-only')),
  'newer',
  'a remembered registry-only model must not displace the newest local ready model',
);
assert.equal(defaults.modelIsDownloaded({ id: 'ready-label', labels: ['ready'] }), true);
assert.equal(defaults.modelCanAnswerChat({ id: 'image', labels: ['image'], downloaded: true }), false);

const chatSource = fs.readFileSync(chatPath, 'utf8');
assert.match(chatSource, /resolveLastReadyChatModel\(knownModelInfos, lastReadyModelName\)/);
assert.match(chatSource, /const currentModel = fallbackModelOverride\s*\|\| selectedModel/,
  'an explicitly selected deferred default must not be displaced by a background health refresh');
assert.match(chatSource, /\|\| preferredDefaultModelName/);
assert.match(chatSource, /const preparedSnapshot = await ensureChatModelReady\(currentModel, currentKnownModelInfo, convoId\)/);
assert.match(chatSource, /await api\.pullModel\(modelName/);
assert.match(chatSource, /if \(!sawDownload\) return false;[\s\S]*terminal\?\.status === 'error'/,
  'a previous failed download must remain retryable on the next send');
assert.match(chatSource, /messages: \[userMessage\]/,
  'the user message must become visible before a potentially long first download');
assert.match(chatSource, /errors\.serverDisconnected[\s\S]*isError: true/,
  'send failures must produce a localized assistant response instead of returning silently');
assert.match(chatSource, /option\.defaultLabel/,
  'Tiny and Quality roles must remain available as text metadata without requiring an icon');
assert.doesNotMatch(chatSource, /Bonsai-8B-gguf|Qwen3\.5-4B-GGUF/,
  'default model names must remain isolated in the replaceable configuration module');
assert.doesNotMatch(chatSource, /composer__model-default-icon/,
  'default model roles must not reintroduce dedicated model icons');

console.log('Default chat model fallback contract checks passed.');
