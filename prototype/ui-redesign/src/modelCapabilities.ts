import { LoadedModel, ModelInfo } from './api';

export type ModelCapability = 'chat' | 'image' | 'audio' | 'tts' | 'embedding' | 'reranking' | 'unknown';

export interface ModelSnapshot {
  name: string;
  type: string;
  capability: ModelCapability;
  recipe?: string;
  device?: string;
  checkpoint?: string;
}

const TYPE_TO_CAPABILITY: Record<string, ModelCapability> = {
  llm: 'chat',
  chat: 'chat',
  image: 'image',
  diffusion: 'image',
  audio: 'audio',
  transcription: 'audio',
  'realtime-transcription': 'audio',
  tts: 'tts',
  speech: 'tts',
  embedding: 'embedding',
  embeddings: 'embedding',
  reranking: 'reranking',
  reranker: 'reranking',
};

export function normalizeModelType(type?: string | null): string {
  return (type || 'unknown').toLowerCase().trim() || 'unknown';
}

export function capabilityFromType(type?: string | null): ModelCapability {
  const normalized = normalizeModelType(type);
  return TYPE_TO_CAPABILITY[normalized] || 'unknown';
}

export function capabilityFromRecipe(recipe?: string | null): ModelCapability {
  const r = normalizeModelType(recipe);
  if (r === 'sd-cpp' || r.includes('stable-diffusion') || r.includes('diffusion')) return 'image';
  if (r === 'whispercpp' || r.includes('whisper')) return 'audio';
  if (r === 'kokoro' || r.includes('tts')) return 'tts';
  if (r === 'llamacpp' || r === 'flm' || r === 'ryzenai-llm' || r === 'vllm') return 'chat';
  return 'unknown';
}

export function capabilityFromLabels(labels?: string[]): ModelCapability {
  const lower = (labels || []).map(l => l.toLowerCase());
  if (lower.some(l => l === 'image' || l === 'edit' || l === 'upscaling')) return 'image';
  if (lower.some(l => l === 'tts' || l === 'speech')) return 'tts';
  if (lower.some(l => l === 'transcription' || l === 'realtime-transcription' || l === 'audio')) return 'audio';
  if (lower.some(l => l === 'embeddings' || l === 'embedding')) return 'embedding';
  if (lower.some(l => l === 'reranking' || l === 'reranker')) return 'reranking';
  if (lower.some(l => l === 'chat' || l === 'vision' || l === 'tool-calling' || l === 'reasoning' || l === 'coding')) return 'chat';
  return 'unknown';
}

export function capabilityFromLoaded(model?: LoadedModel | null): ModelCapability {
  if (!model) return 'unknown';
  return capabilityFromType(model.type) !== 'unknown'
    ? capabilityFromType(model.type)
    : capabilityFromRecipe(model.recipe);
}

export function capabilityFromModelInfo(model: ModelInfo): ModelCapability {
  const direct = capabilityFromType((model as any).type);
  if (direct !== 'unknown') return direct;
  const fromLabels = capabilityFromLabels(model.labels || []);
  if (fromLabels !== 'unknown') return fromLabels;
  return capabilityFromRecipe((model as any).recipe);
}

export function canUseChatCompletions(model?: LoadedModel | null): boolean {
  return capabilityFromLoaded(model) === 'chat';
}

export function canSelectInComposer(model?: LoadedModel | null): boolean {
  const cap = capabilityFromLoaded(model);
  return cap === 'chat' || cap === 'image' || cap === 'audio' || cap === 'tts';
}

export function capabilityLabel(capability: ModelCapability): string {
  switch (capability) {
    case 'chat': return 'Chat';
    case 'image': return 'Image';
    case 'audio': return 'Audio';
    case 'tts': return 'TTS';
    case 'embedding': return 'Embedding';
    case 'reranking': return 'Reranking';
    default: return 'Unknown';
  }
}

export function capabilityBadge(capability: ModelCapability): string {
  switch (capability) {
    case 'chat': return 'chat';
    case 'image': return 'image';
    case 'audio': return 'audio';
    case 'tts': return 'tts';
    case 'embedding': return 'embed';
    case 'reranking': return 'rank';
    default: return 'model';
  }
}

export function capabilityIcon(capability: ModelCapability): string {
  switch (capability) {
    case 'chat': return '💬';
    case 'image': return '🖼';
    case 'audio': return '🎙';
    case 'tts': return '🔊';
    case 'embedding': return '🔢';
    case 'reranking': return '🔀';
    default: return '⚙';
  }
}

export function snapshotFromLoaded(model?: LoadedModel | null): ModelSnapshot | null {
  if (!model) return null;
  return {
    name: model.model_name,
    type: normalizeModelType(model.type),
    capability: capabilityFromLoaded(model),
    recipe: model.recipe,
    device: model.device,
    checkpoint: model.checkpoint,
  };
}

export function snapshotFromName(name: string | null | undefined, loadedModels: LoadedModel[]): ModelSnapshot | null {
  if (!name) return null;
  const loaded = loadedModels.find(m => m.model_name === name);
  if (loaded) return snapshotFromLoaded(loaded);
  return { name, type: 'unknown', capability: 'unknown' };
}

export function selectPreferredLoadedModel(loadedModels: LoadedModel[]): LoadedModel | null {
  return loadedModels.find(m => canUseChatCompletions(m))
    || loadedModels.find(m => canSelectInComposer(m))
    || loadedModels[0]
    || null;
}

export function modelDisplayName(model: ModelSnapshot | null | undefined): string {
  return model?.name || 'Assistant';
}

export function modelInitial(model: ModelSnapshot | null | undefined): string {
  const name = modelDisplayName(model);
  return name.charAt(0).toUpperCase();
}
