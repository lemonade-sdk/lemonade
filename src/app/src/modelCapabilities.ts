import { LoadedModel, ModelInfo } from './api';

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
  vision: 'omni', vlm: 'omni', 'vision-language': 'omni', omni: 'omni', multimodal: 'omni', 'multi-modal': 'omni',
  'audio-chat': 'omni', realtime: 'omni',
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

const EXPLICIT_OMNI_NAME_PATTERNS = [
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
  /(^|[._\-/])audio-chat([._\-/]|$)/,
  /(^|[._\-/])speech-chat([._\-/]|$)/,
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
  if (r === 'collection.omni' || r.includes('collection.omni')) return 'omni';
  if (r === 'collection') return 'omni';
  for (const [hint, cap] of NON_CHAT_RECIPE_HINTS) {
    if (r === hint || r.includes(hint)) return cap;
  }
  if (r.includes('omni') || r.includes('multimodal') || r.includes('vision-language')) return 'omni';
  if (CHAT_RECIPE_HINTS.has(r)) return 'chat';
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
  if (EXPLICIT_OMNI_NAME_PATTERNS.some(pattern => pattern.test(n))) return 'omni';
  return 'unknown';
}

export function capabilityFromLabels(labels?: string[]): ModelCapability {
  const lower = (labels || []).map(l => l.toLowerCase().trim()).filter(Boolean);
  const set = new Set(lower);

  if (lower.some(l => ['3d', '3d-generation', 'model3d', 'image-to-3d', 'mesh-generation'].includes(l))) return 'model3d';
  if (lower.some(l => ['audio-generation', 'music-generation', 'sound-generation', 'sfx', 'music'].includes(l))) return 'audio-generation';
  if (lower.some(l => l === 'image' || l === 'image-generation' || l === 'edit' || l === 'upscaling')) return 'image';
  if (lower.some(l => l === 'tts' || l === 'speech' || l === 'text-to-speech' || l === 'voice-design')) return 'tts';
  if (lower.some(l => l === 'audio' || l === 'transcription' || l === 'realtime-transcription' || l === 'asr' || l === 'stt' || l === 'speech-to-text')) return 'audio';
  if (lower.some(l => l === 'embeddings' || l === 'embedding')) return 'embedding';
  if (lower.some(l => l === 'reranking' || l === 'reranker')) return 'reranking';

  const hasExplicitOmni = lower.some(l => ['omni', 'multimodal', 'multi-modal', 'vision-language', 'vlm', 'image-input', 'audio-input', 'audio-chat', 'chat-transcription', 'realtime'].includes(l));
  const hasChatLike = lower.some(l => ['chat', 'tool-calling', 'tools', 'reasoning', 'coding', 'code', 'llm'].includes(l));
  const hasInputModality = lower.some(l => ['image-input', 'vision-language', 'vlm', 'audio-input', 'audio-chat', 'chat-transcription', 'realtime'].includes(l));

  if (hasExplicitOmni || (hasChatLike && hasInputModality)) return 'omni';
  if (hasChatLike || set.has('vision')) return 'chat';
  return 'unknown';
}

function preferNonChat(cap: ModelCapability): boolean {
  return cap !== 'chat' && cap !== 'unknown';
}

export function capabilityFromLoaded(model?: LoadedModel | null): ModelCapability {
  if (!model) return 'unknown';
  const recipeCap = capabilityFromRecipe(model.recipe);
  if (preferNonChat(recipeCap)) return recipeCap;
  const typeCap = capabilityFromType(model.type);
  if (preferNonChat(typeCap)) return typeCap;
  const nameCap = capabilityFromName(`${model.model_name} ${model.checkpoint || ''}`);
  if (preferNonChat(nameCap)) return nameCap;
  if (typeCap === 'chat' || recipeCap === 'chat') return 'chat';
  return nameCap;
}

export function capabilityFromModelInfo(model: ModelInfo): ModelCapability {
  const recipeCap = capabilityFromRecipe((model as any).recipe);
  if (preferNonChat(recipeCap)) return recipeCap;

  const direct = capabilityFromType((model as any).type);
  if (preferNonChat(direct)) return direct;

  const fromLabels = capabilityFromLabels(model.labels || []);
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
  const labels = (model.labels || []).map(l => String(l).toLowerCase().trim()).filter(Boolean);
  const labelSet = new Set(labels);
  const found = new Set<CapabilityTag>();
  for (const tag of CAPABILITY_TAG_ORDER) {
    if (CAPABILITY_TAG_ALIASES[tag].some(alias => labelSet.has(alias))) found.add(tag);
  }
  const base = BASE_CAPABILITY_TAG[capabilityFromModelInfo(model)];
  if (base) found.add(base);
  if (found.size === 0) found.add('chat');
  return CAPABILITY_TAG_ORDER.filter(tag => found.has(tag));
}

export function modelMatchesCapabilityTags(model: ModelInfo, selected: Set<string>): boolean {
  if (!selected || selected.size === 0) return true;
  return modelCapabilityTags(model).some(tag => selected.has(tag));
}
