import type { LoadedModel, ModelInfo } from './api';

export type ModelCapability =
  | 'chat'
  | 'omni'
  | 'image'
  | 'audio'
  | 'audio-generation'
  | 'tts'
  | 'model3d'
  | 'embedding'
  | 'reranking'
  | 'unknown';

export interface ModelSnapshot {
  name: string;
  type: string;
  capability: ModelCapability;
  recipe?: string;
  device?: string;
  checkpoint?: string;
}

const TYPE_TO_CAPABILITY: Record<string, ModelCapability> = {
  llm: 'chat', chat: 'chat', text: 'chat', language: 'chat',
  // A single multimodal model still uses the Chat surface. `omni` is reserved
  // for collection.omni orchestration, not for ordinary VLM/audio-capable LLMs.
  vision: 'chat', vlm: 'chat', 'vision-language': 'chat', omni: 'chat', multimodal: 'chat', 'multi-modal': 'chat',
  'audio-chat': 'chat', realtime: 'chat',
  image: 'image', diffusion: 'image', 'image-generation': 'image',
  audio: 'audio', transcription: 'audio', 'realtime-transcription': 'audio', asr: 'audio', stt: 'audio', 'speech-to-text': 'audio',
  'audio-generation': 'audio-generation', 'music-generation': 'audio-generation', 'sound-generation': 'audio-generation', sfx: 'audio-generation', music: 'audio-generation',
  tts: 'tts', speech: 'tts', 'text-to-speech': 'tts',
  '3d': 'model3d', model3d: 'model3d', '3d-generation': 'model3d', 'image-to-3d': 'model3d', mesh: 'model3d',
  embedding: 'embedding', embeddings: 'embedding', reranking: 'reranking', reranker: 'reranking',
};

const NON_CHAT_RECIPE_HINTS: Array<[string, ModelCapability]> = [
  ['trellis', 'model3d'],
  ['acestep', 'audio-generation'], ['ace-step', 'audio-generation'], ['thinksound', 'audio-generation'],
  ['stable-diffusion', 'image'], ['diffusion', 'image'], ['sd-cpp', 'image'],
  ['whisper', 'audio'], ['moonshine', 'audio'], ['asr', 'audio'], ['speech-to-text', 'audio'],
  ['openmoss', 'tts'], ['kokoro', 'tts'], ['text-to-speech', 'tts'], ['tts', 'tts'],
  ['embedding', 'embedding'], ['rerank', 'reranking'],
];

const CHAT_RECIPE_HINTS = new Set(['llamacpp', 'flm', 'ryzenai-llm', 'vllm']);

const MULTIMODAL_CHAT_NAME_PATTERNS = [
  /(^|[._\-/])omni([._\-/]|$)/,
  /(^|[._\-/])multimodal([._\-/]|$)/,
  /(^|[._\-/])multi-modal([._\-/]|$)/,
  /(^|[._\-/])vision-language([._\-/]|$)/,
  /(^|[._\-/])vlm([._\-/]|$)/,
  /(^|[._\-/])vl([._\-/]|$)/,
  /(^|[._\-/])llava([._\-/]|$)/,
  /(^|[._\-/])bakllava([._\-/]|$)/,
  /(^|[._\-/])moondream([._\-/]|$)/,
  /(^|[._\-/])pixtral([._\-/]|$)/,
  /(^|[._\-/])mllama([._\-/]|$)/,
  /(^|[._\-/])qwen\d*(?:\.\d+)?-vl([._\-/]|$)/,
  /(^|[._\-/])minicpm-v([._\-/]|$)/,
  /(^|[._\-/])minicpmv([._\-/]|$)/,
  /(^|[._\-/])phi-4-multimodal([._\-/]|$)/,
];

export function normalizeModelType(type?: string | null): string {
  return (type || 'unknown').toLowerCase().trim() || 'unknown';
}

export function capabilityFromType(type?: string | null): ModelCapability {
  return TYPE_TO_CAPABILITY[normalizeModelType(type)] || 'unknown';
}

export function capabilityFromRecipe(recipe?: string | null): ModelCapability {
  const r = normalizeModelType(recipe);
  if (!r || r === 'unknown') return 'unknown';
  if (r === 'collection.omni' || r.startsWith('collection.omni.')) return 'omni';
  if (r === 'collection.router' || r.startsWith('collection.router.')) return 'chat';
  if (r === 'collection') return 'omni';
  for (const [hint, cap] of NON_CHAT_RECIPE_HINTS) {
    if (r === hint || r.includes(hint)) return cap;
  }
  if (CHAT_RECIPE_HINTS.has(r)) return 'chat';
  if (r.includes('multimodal') || r.includes('vision-language') || r.includes('audio-chat')) return 'chat';
  return 'unknown';
}

export function capabilityFromName(name?: string | null): ModelCapability {
  const n = normalizeModelType(name);
  if (!n || n === 'unknown') return 'unknown';
  if (n.includes('trellis') || n.includes('image-to-3d') || n.includes('model3d')) return 'model3d';
  if (n.includes('ace-step') || n.includes('acestep') || n.includes('thinksound')) return 'audio-generation';
  if (n.includes('openmoss') || n.includes('moss-tts') || n.includes('moss_tts') || n.includes('voicegen')) return 'tts';
  if (n.includes('embed')) return 'embedding';
  if (n.includes('rerank')) return 'reranking';
  if (/(^|[._\-/])(audio-chat|speech-chat)([._\-/]|$)/.test(n)) return 'chat';
  if (MULTIMODAL_CHAT_NAME_PATTERNS.some(pattern => pattern.test(n))) return 'chat';
  return 'unknown';
}

const CHAT_PRIMARY_LABELS = new Set([
  'chat', 'llm', 'text', 'language', 'instruct', 'text-generation',
  'tool-calling', 'tools', 'reasoning', 'coding', 'code',
]);

const AUDIO_INPUT_LABELS = new Set([
  'audio-input', 'audio-chat', 'chat-transcription', 'speech-input',
]);

const IMAGE_INPUT_LABELS = new Set([
  'vision', 'image-input', 'vision-language', 'vlm', 'image-text-to-text',
  'multimodal', 'multi-modal',
]);

const STANDALONE_AUDIO_LABELS = new Set([
  'audio', 'transcription', 'realtime-transcription', 'asr', 'stt', 'speech-to-text',
]);

function normalizedStringList(value: unknown): string[] {
  if (!Array.isArray(value)) return [];
  return value
    .filter((item): item is string => typeof item === 'string')
    .map(item => item.toLowerCase().trim())
    .filter(Boolean);
}

function modelDescriptorStrings(model?: ModelInfo | LoadedModel | null): string[] {
  if (!model) return [];
  return [
    ...normalizedStringList((model as any).labels),
    ...normalizedStringList((model as any).capabilities),
  ];
}

function modelInputModalities(model?: ModelInfo | LoadedModel | null): string[] {
  if (!model) return [];
  return normalizedStringList((model as any).input_modalities);
}

function hasChatPrimaryEvidence(model?: ModelInfo | LoadedModel | null): boolean {
  if (!model) return false;
  const recipeCap = capabilityFromRecipe(String((model as any).recipe || ''));
  // Omni collections have their own orchestration surface. Do not reuse the
  // Chat + Audio decoration for them even though they ultimately call chat APIs.
  if (recipeCap === 'omni') return false;
  if (recipeCap === 'chat') return true;
  const directCaps = [
    capabilityFromType(String((model as any).capability || '')),
    capabilityFromType(String((model as any).type || '')),
  ];
  if (directCaps.some(cap => cap === 'chat' || cap === 'omni')) return true;
  return modelDescriptorStrings(model).some(value => CHAT_PRIMARY_LABELS.has(value));
}

/**
 * True when audio is an input modality of a chat model. This is intentionally
 * separate from the primary capability: a multimodal LLM stays in Chat mode
 * and merely gains the audio attachment controls. A transcription-only model
 * such as Whisper remains in Audio mode.
 */
export function modelSupportsChatAudioInput(
  modelInfo?: ModelInfo | null,
  loadedModel?: LoadedModel | null,
): boolean {
  const descriptors = [
    ...modelDescriptorStrings(modelInfo),
    ...modelDescriptorStrings(loadedModel),
  ];
  const inputModalities = [
    ...modelInputModalities(modelInfo),
    ...modelInputModalities(loadedModel),
  ];
  const primaryIsChat = hasChatPrimaryEvidence(modelInfo) || hasChatPrimaryEvidence(loadedModel);
  if (!primaryIsChat) return false;

  if (inputModalities.some(value => value === 'audio' || value === 'speech')) return true;
  if (descriptors.some(value => AUDIO_INPUT_LABELS.has(value))) return true;

  // Some backends currently expose a multimodal LLM as type/label "audio"
  // without a separate input_modalities field. It is safe to interpret that as
  // audio input only when the recipe/type independently proves this is a chat
  // model; standalone transcription recipes never enter this branch.
  if (descriptors.includes('audio')) return true;
  if (normalizeModelType(loadedModel?.type) === 'audio') return true;
  if (normalizeModelType((modelInfo as any)?.type) === 'audio') return true;
  return false;
}

/**
 * True when image/vision is an input modality of a chat model. A plain LLM
 * stays in Chat mode but must not expose the image attachment affordance.
 * Collection-based Omni routing is handled separately by the collection
 * component resolver because the collection itself is not a single VLM.
 */
export function modelSupportsChatImageInput(
  modelInfo?: ModelInfo | null,
  loadedModel?: LoadedModel | null,
): boolean {
  const descriptors = [
    ...modelDescriptorStrings(modelInfo),
    ...modelDescriptorStrings(loadedModel),
  ];
  const inputModalities = [
    ...modelInputModalities(modelInfo),
    ...modelInputModalities(loadedModel),
  ];
  const primaryIsChat = hasChatPrimaryEvidence(modelInfo) || hasChatPrimaryEvidence(loadedModel);
  if (!primaryIsChat) return false;

  if (inputModalities.some(value => value === 'image' || value === 'vision')) return true;
  if (descriptors.some(value => IMAGE_INPUT_LABELS.has(value))) return true;

  const declaredTypes = [
    normalizeModelType((modelInfo as any)?.capability),
    normalizeModelType((modelInfo as any)?.type),
    normalizeModelType((loadedModel as any)?.capability),
    normalizeModelType(loadedModel?.type),
  ];
  if (declaredTypes.some(value => IMAGE_INPUT_LABELS.has(value))) return true;

  const identities = [
    (modelInfo as any)?.model_name,
    modelInfo?.name,
    modelInfo?.id,
    (modelInfo as any)?.checkpoint,
    loadedModel?.model_name,
    loadedModel?.checkpoint,
  ]
    .filter((value): value is string => typeof value === 'string' && value.trim().length > 0)
    .map(value => normalizeModelType(value));

  return identities.some(identity => MULTIMODAL_CHAT_NAME_PATTERNS.some(pattern => pattern.test(identity)));
}

export function capabilityFromLabels(labels?: string[]): ModelCapability {
  const lower = (labels || []).map(l => l.toLowerCase().trim()).filter(Boolean);
  const set = new Set(lower);

  if (lower.some(l => ['3d', '3d-generation', 'model3d', 'image-to-3d', 'mesh-generation'].includes(l))) return 'model3d';
  if (lower.some(l => ['audio-generation', 'music-generation', 'sound-generation', 'sfx', 'music'].includes(l))) return 'audio-generation';
  if (lower.some(l => l === 'image' || l === 'image-generation' || l === 'edit' || l === 'upscaling')) return 'image';
  if (lower.some(l => l === 'tts' || l === 'speech' || l === 'text-to-speech' || l === 'voice-design')) return 'tts';
  if (lower.some(l => l === 'embeddings' || l === 'embedding')) return 'embedding';
  if (lower.some(l => l === 'reranking' || l === 'reranker')) return 'reranking';

  const hasChatLike = lower.some(l => CHAT_PRIMARY_LABELS.has(l));
  const hasAudioChat = lower.some(l => AUDIO_INPUT_LABELS.has(l));
  const hasVisionInput = lower.some(l => ['vision', 'image-input', 'vision-language', 'vlm', 'image-text-to-text'].includes(l));
  const hasMultimodalChat = lower.some(l => ['omni', 'multimodal', 'multi-modal'].includes(l));
  const hasStandaloneAudio = lower.some(l => STANDALONE_AUDIO_LABELS.has(l));

  // Labels describe inputs/features of an individual model. They never promote
  // that model to the Omni collection mode.
  if (hasMultimodalChat || hasVisionInput || hasAudioChat || (hasChatLike && set.has('audio'))) return 'chat';
  if (hasStandaloneAudio) return 'audio';
  if (hasChatLike || hasVisionInput) return 'chat';
  return 'unknown';
}

function preferNonChat(cap: ModelCapability): boolean {
  return cap !== 'chat' && cap !== 'unknown';
}

export function capabilityFromLoaded(model?: LoadedModel | null): ModelCapability {
  if (!model) return 'unknown';
  const recipeCap = capabilityFromRecipe(model.recipe);
  if (preferNonChat(recipeCap)) return recipeCap;

  const descriptorCap = capabilityFromLabels([
    ...(model.labels || []),
    ...(model.capabilities || []),
    ...(model.input_modalities || []).map(value => `${value}-input`),
  ]);
  const typeCap = capabilityFromType(model.type);

  // Runtime health may report "audio" for an audio-capable FLM/LLM. The
  // backend recipe is the stronger evidence for its primary interaction mode.
  if (recipeCap === 'chat' && (typeCap === 'audio' || descriptorCap === 'audio')) return 'chat';
  if (preferNonChat(typeCap)) return typeCap;
  if (preferNonChat(descriptorCap)) return descriptorCap;

  const nameCap = capabilityFromName(`${model.model_name} ${model.checkpoint || ''}`);
  if (preferNonChat(nameCap)) return nameCap;
  if (typeCap === 'chat' || descriptorCap === 'chat' || recipeCap === 'chat') return 'chat';
  return nameCap;
}

export function capabilityFromModelInfo(model: ModelInfo): ModelCapability {
  const recipeCap = capabilityFromRecipe(String((model as any).recipe || ''));
  const explicitCapability = capabilityFromType(String((model as any).capability || ''));
  const direct = capabilityFromType(String((model as any).type || ''));
  const declaredCapabilities = normalizedStringList((model as any).capabilities);
  const inputModalities = normalizedStringList((model as any).input_modalities);
  const fromLabels = capabilityFromLabels([
    ...(model.labels || []),
    ...declaredCapabilities,
    ...inputModalities.map(value => `${value}-input`),
  ]);

  // collection.omni is an explicit Lemonade orchestration recipe and is the
  // only route to the Omni primary mode.
  if (recipeCap === 'omni') return 'omni';

  const source = String((model as any).registry_source || (model as any).source || '').trim().toLowerCase();
  const isRemoteRegistryModel = source === 'huggingface' || source === 'modelscope' || source === 'hf' || source === 'ms';
  if (isRemoteRegistryModel) {
    // Remote search rows must not become Chat merely because the repository
    // name contains a keyword or because every GGUF uses the llamacpp recipe.
    // Only explicit server/provider evidence is accepted.
    if (explicitCapability !== 'unknown') return explicitCapability;
    if (direct !== 'unknown') return direct;
    if (fromLabels !== 'unknown') return fromLabels;
    return 'unknown';
  }

  if (preferNonChat(recipeCap)) return recipeCap;

  // A chat backend with audio input remains a chat model. This handles FLM
  // models whose runtime metadata currently reports type/label "audio".
  if (recipeCap === 'chat' && (
    explicitCapability === 'audio'
    || direct === 'audio'
    || fromLabels === 'audio'
    || modelSupportsChatAudioInput(model, null)
  )) return 'chat';

  if (explicitCapability !== 'unknown') return explicitCapability;
  if (preferNonChat(direct)) return direct;
  if (preferNonChat(fromLabels)) return fromLabels;

  const nameCap = capabilityFromName(`${model.id || ''} ${model.name || ''} ${model.display_name || ''} ${String((model as any).model_name || '')} ${String((model as any).checkpoint || '')}`);
  if (preferNonChat(nameCap)) return nameCap;

  if (fromLabels === 'chat' || direct === 'chat' || recipeCap === 'chat') return 'chat';
  return 'unknown';
}

export function canUseChatCompletions(model?: LoadedModel | null): boolean {
  const cap = capabilityFromLoaded(model);
  return cap === 'chat' || cap === 'omni';
}

export function canSelectInComposer(model?: LoadedModel | null): boolean {
  const cap = capabilityFromLoaded(model);
  return ['chat', 'omni', 'image', 'audio', 'audio-generation', 'tts', 'model3d'].includes(cap);
}

export function capabilityLabel(capability: ModelCapability): string {
  switch (capability) {
    case 'chat': return 'Chat';
    case 'omni': return 'Omni';
    case 'image': return 'Image';
    case 'audio': return 'Audio';
    case 'audio-generation': return 'Music & SFX';
    case 'tts': return 'TTS';
    case 'model3d': return '3D';
    case 'embedding': return 'Embedding';
    case 'reranking': return 'Reranking';
    default: return 'Unknown';
  }
}

export function capabilityBadge(capability: ModelCapability): string {
  switch (capability) {
    case 'chat': return 'chat';
    case 'omni': return 'omni';
    case 'image': return 'image';
    case 'audio': return 'audio';
    case 'audio-generation': return 'audio-gen';
    case 'tts': return 'tts';
    case 'model3d': return 'model3d';
    case 'embedding': return 'embed';
    case 'reranking': return 'rank';
    default: return 'model';
  }
}

export function capabilityIcon(capability: ModelCapability | 'all' | 'vision' | 'code' | 'transcription'): string {
  switch (capability) {
    case 'all': return 'All';
    case 'chat': return 'Chat';
    case 'omni': return 'Omni';
    case 'image': return 'Image';
    case 'audio': return 'Audio';
    case 'audio-generation': return 'Audio';
    case 'transcription': return 'Audio';
    case 'tts': return 'TTS';
    case 'model3d': return '3D';
    case 'embedding': return 'Emb';
    case 'reranking': return 'Rank';
    case 'vision': return 'Vision';
    case 'code': return 'Code';
    default: return 'Model';
  }
}

export function snapshotFromLoaded(model?: LoadedModel | null): ModelSnapshot | null {
  if (!model) return null;
  return { name: model.model_name, type: normalizeModelType(model.type), capability: capabilityFromLoaded(model), recipe: model.recipe, device: model.device, checkpoint: model.checkpoint };
}

export function snapshotFromModelInfo(model?: ModelInfo | null): ModelSnapshot | null {
  if (!model) return null;
  const name = String((model as any).model_name || model.name || model.id || '').trim();
  if (!name) return null;
  const capability = capabilityFromModelInfo(model);
  return {
    name,
    type: String((model as any).type || capability || 'unknown'),
    capability,
    recipe: String((model as any).recipe || ''),
    checkpoint: String((model as any).checkpoint || ''),
  };
}

export function snapshotFromName(name: string | null | undefined, loadedModels: LoadedModel[]): ModelSnapshot | null {
  if (!name) return null;
  const loaded = loadedModels.find(m => m.model_name === name);
  if (loaded) return snapshotFromLoaded(loaded);
  return { name, type: 'unknown', capability: capabilityFromName(name) };
}

export function selectPreferredLoadedModel(loadedModels: LoadedModel[]): LoadedModel | null {
  return loadedModels.find(m => capabilityFromLoaded(m) === 'chat')
    || loadedModels.find(m => capabilityFromLoaded(m) === 'omni')
    || loadedModels.find(m => canSelectInComposer(m))
    || loadedModels[0]
    || null;
}

export function modelDisplayName(model: ModelSnapshot | null | undefined): string { return model?.name || 'Assistant'; }
export function modelInitial(model: ModelSnapshot | null | undefined): string { return modelDisplayName(model).charAt(0).toUpperCase(); }

/* Functional capability tags drive both the model filters and row badges. */
export type CapabilityTag =
  | 'popular' | 'chat' | 'omni' | 'vision' | 'tool' | 'reasoning'
  | 'code' | 'audio' | 'audio-generation' | 'image' | 'tts' | 'model3d'
  | 'embedding' | 'reranking';

export const CAPABILITY_TAG_ORDER: CapabilityTag[] = [
  'popular', 'chat', 'omni', 'vision', 'tool', 'reasoning',
  'code', 'audio', 'audio-generation', 'image', 'tts', 'model3d', 'embedding', 'reranking',
];

export const CAPABILITY_TAG_LABELS: Record<CapabilityTag, string> = {
  popular: 'Popular', chat: 'Chat', omni: 'Omni', vision: 'Vision',
  tool: 'Tool use', reasoning: 'Reasoning', code: 'Code', audio: 'Audio',
  'audio-generation': 'Music & SFX', image: 'Image', tts: 'Speech (TTS)',
  model3d: '3D', embedding: 'Embeddings', reranking: 'Reranking',
};

const CAPABILITY_TAG_ALIASES: Record<CapabilityTag, string[]> = {
  popular: ['popular', 'trending', 'featured', 'recommended'],
  chat: ['chat', 'llm', 'text', 'language', 'instruct', 'text-generation'],
  omni: ['omni', 'multimodal', 'multi-modal'],
  vision: ['vision', 'vlm', 'vision-language', 'image-input', 'image-text-to-text'],
  tool: ['tool', 'tools', 'tool-use', 'tool-calling', 'tools-enabled', 'function-calling'],
  reasoning: ['reasoning', 'thinking', 'reasoner', 'mtp'],
  code: ['code', 'coding', 'coder'],
  audio: ['audio', 'transcription', 'asr', 'stt', 'speech-to-text', 'realtime-transcription'],
  'audio-generation': ['audio-generation', 'music-generation', 'sound-generation', 'sfx', 'music'],
  image: ['image', 'image-generation', 'diffusion', 'edit', 'upscaling', 'text-to-image'],
  tts: ['tts', 'speech', 'text-to-speech', 'voice-design'],
  model3d: ['3d', '3d-generation', 'model3d', 'image-to-3d', 'mesh-generation'],
  embedding: ['embedding', 'embeddings'],
  reranking: ['reranking', 'reranker'],
};

const BASE_CAPABILITY_TAG: Partial<Record<ModelCapability, CapabilityTag>> = {
  chat: 'chat', omni: 'omni', image: 'image', audio: 'audio',
  'audio-generation': 'audio-generation', tts: 'tts', model3d: 'model3d',
  embedding: 'embedding', reranking: 'reranking',
};

export function modelCapabilityTags(model: ModelInfo): CapabilityTag[] {
  const declaredCapabilities = Array.isArray((model as any).capabilities)
    ? (model as any).capabilities.filter((value: unknown): value is string => typeof value === 'string')
    : [];
  const inputModalities = normalizedStringList((model as any).input_modalities);
  const labels = [
    ...(model.labels || []),
    ...declaredCapabilities,
    ...inputModalities.map(value => `${value}-input`),
  ]
    .map(l => String(l).toLowerCase().trim())
    .filter(Boolean);
  const labelSet = new Set(labels);
  const found = new Set<CapabilityTag>();
  for (const tag of CAPABILITY_TAG_ORDER) {
    if (tag === 'omni') continue;
    if (CAPABILITY_TAG_ALIASES[tag].some(alias => labelSet.has(alias))) found.add(tag);
  }
  const base = BASE_CAPABILITY_TAG[capabilityFromModelInfo(model)];
  if (base) found.add(base);
  if (modelSupportsChatAudioInput(model, null)) found.add('audio');
  return CAPABILITY_TAG_ORDER.filter(tag => found.has(tag));
}

export function modelMatchesCapabilityTags(model: ModelInfo, selected: Set<string>): boolean {
  if (!selected || selected.size === 0) return true;
  return modelCapabilityTags(model).some(tag => selected.has(tag));
}
