const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');

function loadTypeScriptModule(filename) {
  const source = fs.readFileSync(filename, 'utf8');
  const compiled = ts.transpileModule(source, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS,
      esModuleInterop: true,
    },
    fileName: filename,
    reportDiagnostics: true,
  });
  const diagnostics = compiled.diagnostics || [];
  assert.equal(
    diagnostics.length,
    0,
    diagnostics.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'),
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

const capabilitiesPath = path.join(root, 'src/modelCapabilities.ts');
const chatViewPath = path.join(root, 'src/components/ChatView.tsx');
const apiPath = path.join(root, 'src/api.ts');
const stylesPath = path.join(root, 'src/styles/styles.css');

const capabilitiesSource = fs.readFileSync(capabilitiesPath, 'utf8');
const chatViewSource = fs.readFileSync(chatViewPath, 'utf8');
const apiSource = fs.readFileSync(apiPath, 'utf8');
const stylesSource = fs.readFileSync(stylesPath, 'utf8');

const {
  capabilityFromLabels,
  capabilityFromLoaded,
  capabilityFromModelInfo,
  modelCapabilityTags,
  modelSupportsChatAudioInput,
  modelSupportsChatImageInput,
} = loadTypeScriptModule(capabilitiesPath);

const gemmaFlmLoaded = {
  model_name: 'Gemma-4-E2B-it-FLM',
  checkpoint: 'gemma4:e2b',
  recipe: 'flm',
  device: 'npu',
  backend_url: '',
  pid: 123,
  type: 'audio',
  last_use: Date.now(),
  input_modalities: ['text', 'audio'],
};
const gemmaFlmInfo = {
  id: 'Gemma-4-E2B-it-FLM',
  name: 'Gemma-4-E2B-it-FLM',
  recipe: 'flm',
  type: 'audio',
  labels: ['tool-calling', 'vision', 'audio'],
  input_modalities: ['text', 'audio'],
};

assert.equal(
  capabilityFromLoaded(gemmaFlmLoaded),
  'chat',
  'an audio-capable FLM remains a chat model at runtime',
);
assert.equal(
  capabilityFromModelInfo(gemmaFlmInfo),
  'chat',
  'an audio-capable FLM remains a chat model in catalog metadata',
);
assert.equal(
  modelSupportsChatAudioInput(gemmaFlmInfo, gemmaFlmLoaded),
  true,
  'audio is retained as an additional input capability',
);
assert.equal(
  modelSupportsChatImageInput(gemmaFlmInfo, gemmaFlmLoaded),
  true,
  'vision is retained as an additional input capability',
);
assert.deepEqual(
  [...modelCapabilityTags(gemmaFlmInfo)].sort(),
  ['audio', 'chat', 'tool', 'vision'].sort(),
  'the model exposes both Chat and Audio tags without becoming audio-only',
);

assert.equal(
  capabilityFromLabels(['tool-calling', 'vision', 'audio']),
  'chat',
  'vision and audio are additional inputs of a chat model, never Omni mode',
);
assert.equal(
  capabilityFromModelInfo({ id: 'single-vlm', recipe: 'llamacpp', labels: ['vision', 'multimodal'] }),
  'chat',
  'single multimodal models remain Chat models',
);
assert.equal(
  capabilityFromModelInfo({ id: 'my-suite', recipe: 'collection.omni', components: ['chat', 'audio'] }),
  'omni',
  'Omni remains reserved for collection.omni',
);
assert.equal(
  capabilityFromModelInfo({ id: 'remote-suite', recipe: 'collection.omni', registry_source: 'huggingface', components: ['chat', 'audio'] }),
  'omni',
  'a registry-backed collection.omni remains Omni after it is registered',
);
assert.equal(
  modelSupportsChatAudioInput({ id: 'my-suite', recipe: 'collection.omni', labels: ['audio'] }, null),
  false,
  'Omni collections must not be relabeled as Chat + Audio',
);
assert.equal(
  capabilityFromLabels(['transcription', 'realtime-transcription']),
  'audio',
  'standalone transcription labels remain Audio mode',
);

const whisperLoaded = {
  model_name: 'Whisper-Tiny',
  checkpoint: 'ggerganov/whisper.cpp:ggml-tiny.bin',
  recipe: 'whispercpp',
  device: 'cpu',
  backend_url: '',
  pid: 456,
  type: 'audio',
  last_use: Date.now(),
  labels: ['transcription', 'realtime-transcription'],
  input_modalities: ['audio'],
};
assert.equal(capabilityFromLoaded(whisperLoaded), 'audio');
assert.equal(modelSupportsChatAudioInput(null, whisperLoaded), false);

const plainFlm = {
  ...gemmaFlmLoaded,
  type: 'llm',
  input_modalities: ['text'],
};
assert.equal(capabilityFromLoaded(plainFlm), 'chat');
assert.equal(modelSupportsChatAudioInput(null, plainFlm), false);
assert.equal(modelSupportsChatImageInput(null, plainFlm), false,
  'a plain text LLM must not expose image attachments');
assert.equal(modelSupportsChatImageInput({ id: 'llava-next', recipe: 'llamacpp', type: 'llm' }, null), true,
  'well-known VLM identity patterns remain a fallback when modality metadata is missing');

assert.match(apiSource, /input_modalities\?: string\[\]/,
  'health metadata must preserve declared input modalities');
assert.match(apiSource, /input_modalities: Array\.isArray\(model\.input_modalities\)/,
  'health normalization must copy input modalities');
assert.match(chatViewSource, /const ModelModeIcons:/,
  'chat UI must use a paired model-mode icon component');
assert.match(chatViewSource, /<CapabilityIcon capability="audio"/,
  'paired mode icons must include the Audio glyph');
assert.match(chatViewSource, /const showAudio = audioInput && capability === 'chat'/,
  'the paired Audio icon must only decorate Chat mode, not Omni collections');
assert.match(chatViewSource, /currentLoadedModel \? \([\s\S]*composer__model-mode--\$\{capabilityBadge\(currentCapability\)\}[\s\S]*modelModeLabel\(currentCapability, supportsChatAudioInput\)/,
  'the active model pill must include the colored mode label only for the currently loaded model');
assert.doesNotMatch(chatViewSource, /composer__mode-badge--interactive/,
  'the redundant standalone mode pill must not remain in the chat composer');
assert.match(stylesSource, /\.composer__model-mode--chat \{ color: var\(--success\); \}/,
  'the merged Chat status must retain its capability color');
assert.match(stylesSource, /\.composer__model-button-name[\s\S]*color: var\(--text-primary\)/,
  'the model name must keep the normal text color beside the colored status');
assert.match(chatViewSource, /Omni collection mode/,
  'the UI must describe Omni as collection orchestration');
assert.match(chatViewSource, /Chat \+ audio mode/,
  'the composer must explain the combined chat/audio behavior');
assert.match(chatViewSource, /includeDirectAudioParts = canUseAudioInput && modeSupportsChatCompletions/,
  'audio files must be routed through the normal chat completion request');
assert.match(chatViewSource, /modelSupportsChatAudioInput\(currentKnownModelInfo, currentLoadedModel\)/,
  'audio input availability must be derived independently from primary mode');
assert.match(chatViewSource, /modelSupportsChatImageInput\(currentKnownModelInfo, currentLoadedModel\)/,
  'image input availability must be derived independently from primary Chat mode');
assert.match(chatViewSource, /const canAttach = acceptsImageAttachments \|\| acceptsAudioAttachments/,
  'the paperclip must only be enabled for modalities supported by the selected model');
assert.match(chatViewSource, /if \(!acceptsImageAttachments\) return;/,
  'paste, drop, and file selection must reject images for text-only models');
assert.match(chatViewSource, /The selected text model does not support image input/,
  'stale or retried image messages must be blocked before reaching a text-only backend');
assert.match(chatViewSource, /if \(m\.images\?\.length && supportsChatImageInput\)/,
  'image-bearing conversation history must be stripped when switching to a text-only model');
assert.match(chatViewSource, /const keepsAudioAttachments = currentCapability === 'audio'[\s\S]*modelSupportsChatAudioInput/,
  'switching to Chat + Audio must not immediately clear attached audio');
assert.match(stylesSource, /\.capability-icon-pair[\s\S]*gap: 2px/,
  'the Chat and Audio icons must render close together');

assert.doesNotMatch(capabilitiesSource, /Gemma-4-E2B-it-FLM/i,
  'the fix must be capability-driven rather than model-name-specific');

console.log('Chat + audio capability contract checks passed.');
