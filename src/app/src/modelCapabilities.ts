import { LoadedModel, ModelInfo } from './api';

export type ModelCapability = 'chat' | 'omni' | 'image' | 'audio' | 'tts' | 'embedding' | 'reranking' | 'unknown';

export interface ModelSnapshot {
  name: string;
  type: string;
  capability: ModelCapability;
  recipe?: string;
  device?: string;
  checkpoint?: string;
}

const TYPE_TO_CAPABILITY: Record<string, ModelCapability> = {
  llm: 'chat', chat: 'chat', vision: 'omni', vlm: 'omni', vl: 'omni', omni: 'omni', multimodal: 'omni',
  'vision-language': 'omni', 'audio-chat': 'omni', realtime: 'omni', image: 'image', diffusion: 'image',
  audio: 'audio', transcription: 'audio', 'realtime-transcription': 'audio', tts: 'tts', speech: 'tts',
  embedding: 'embedding', embeddings: 'embedding', reranking: 'reranking', reranker: 'reranking',
};

const OMNI_NAME_HINTS = ['omni','gpt-4o','gpt4o','o4-','vision','vlm','vl-','-vl','llava','bakllava','moondream','minicpm-v','minicpmv','qwen-vl','qwen2-vl','qwen2.5-vl','qwen3-vl','pixtral','mllama','multi-modal','multimodal','phi-4-multimodal','gemma-3','gemma3','audio-chat','speech-chat'];

export function normalizeModelType(type?: string | null): string {
  return (type || 'unknown').toLowerCase().trim() || 'unknown';
}

export function capabilityFromType(type?: string | null): ModelCapability {
  return TYPE_TO_CAPABILITY[normalizeModelType(type)] || 'unknown';
}

export function capabilityFromRecipe(recipe?: string | null): ModelCapability {
  const r = normalizeModelType(recipe);
  if (r === 'sd-cpp' || r.includes('stable-diffusion') || r.includes('diffusion')) return 'image';
  if (r === 'whispercpp' || r.includes('whisper')) return 'audio';
  if (r === 'kokoro' || r.includes('tts')) return 'tts';
  if (r.includes('omni') || r.includes('multimodal') || r.includes('vision')) return 'omni';
  if (r === 'llamacpp' || r === 'flm' || r === 'ryzenai-llm' || r === 'vllm') return 'chat';
  return 'unknown';
}

export function capabilityFromName(name?: string | null): ModelCapability {
  const n = normalizeModelType(name);
  if (!n || n === 'unknown') return 'unknown';
  if (OMNI_NAME_HINTS.some(hint => n.includes(hint))) return 'omni';
  if (n.includes('embed')) return 'embedding';
  if (n.includes('rerank')) return 'reranking';
  return 'unknown';
}

export function capabilityFromLabels(labels?: string[]): ModelCapability {
  const lower = (labels || []).map(l => l.toLowerCase());
  const hasOmni = lower.some(l => ['omni','multimodal','vision-language','audio-chat','chat-transcription','realtime'].includes(l));
  const hasVisionInput = lower.some(l => l === 'vision' || l === 'image-input' || l === 'vlm');
  const hasAudioInput = lower.some(l => l === 'audio-input' || l === 'speech-input');
  const hasChatLike = lower.some(l => l === 'chat' || l === 'tool-calling' || l === 'reasoning' || l === 'coding' || l === 'llm' || l === 'vision');
  if (hasOmni || (hasChatLike && (hasVisionInput || hasAudioInput))) return 'omni';
  if (lower.some(l => l === 'image' || l === 'edit' || l === 'upscaling')) return 'image';
  if (lower.some(l => l === 'tts' || l === 'speech')) return 'tts';
  if (lower.some(l => l === 'transcription' || l === 'realtime-transcription' || l === 'audio')) return 'audio';
  if (lower.some(l => l === 'embeddings' || l === 'embedding')) return 'embedding';
  if (lower.some(l => l === 'reranking' || l === 'reranker')) return 'reranking';
  if (hasChatLike) return 'chat';
  return 'unknown';
}

export function capabilityFromLoaded(model?: LoadedModel | null): ModelCapability {
  if (!model) return 'unknown';
  const typeCap = capabilityFromType(model.type);
  if (typeCap !== 'chat' && typeCap !== 'unknown') return typeCap;
  const recipeCap = capabilityFromRecipe(model.recipe);
  if (recipeCap !== 'chat' && recipeCap !== 'unknown') return recipeCap;
  const nameCap = capabilityFromName(`${model.model_name} ${model.checkpoint || ''}`);
  if (nameCap === 'omni') return 'omni';
  if (typeCap === 'chat') return 'chat';
  if (recipeCap === 'chat') return 'chat';
  return nameCap;
}

export function capabilityFromModelInfo(model: ModelInfo): ModelCapability {
  const direct = capabilityFromType((model as any).type);
  if (direct !== 'chat' && direct !== 'unknown') return direct;
  const fromLabels = capabilityFromLabels(model.labels || []);
  if (fromLabels !== 'chat' && fromLabels !== 'unknown') return fromLabels;
  const fromRecipe = capabilityFromRecipe((model as any).recipe);
  if (fromRecipe !== 'chat' && fromRecipe !== 'unknown') return fromRecipe;
  const nameCap = capabilityFromName(`${model.id || ''} ${model.name || ''} ${model.display_name || ''} ${String((model as any).checkpoint || '')}`);
  if (nameCap === 'omni') return 'omni';
  if (fromLabels === 'chat') return 'chat';
  if (direct === 'chat') return 'chat';
  if (fromRecipe === 'chat') return 'chat';
  return nameCap;
}

export function canUseChatCompletions(model?: LoadedModel | null): boolean {
  const cap = capabilityFromLoaded(model);
  return cap === 'chat' || cap === 'omni';
}

export function canSelectInComposer(model?: LoadedModel | null): boolean {
  const cap = capabilityFromLoaded(model);
  return cap === 'chat' || cap === 'omni' || cap === 'image' || cap === 'audio' || cap === 'tts';
}

export function capabilityLabel(capability: ModelCapability): string {
  switch (capability) {
    case 'chat': return 'Chat'; case 'omni': return 'Omni'; case 'image': return 'Image'; case 'audio': return 'Audio'; case 'tts': return 'TTS'; case 'embedding': return 'Embedding'; case 'reranking': return 'Reranking'; default: return 'Unknown';
  }
}

export function capabilityBadge(capability: ModelCapability): string {
  switch (capability) {
    case 'chat': return 'chat'; case 'omni': return 'omni'; case 'image': return 'image'; case 'audio': return 'audio'; case 'tts': return 'tts'; case 'embedding': return 'embed'; case 'reranking': return 'rank'; default: return 'model';
  }
}

export function capabilityIcon(capability: ModelCapability): string {
  switch (capability) {
    case 'chat': return '💬'; case 'omni': return '✦'; case 'image': return '🖼'; case 'audio': return '🎙'; case 'tts': return '🔊'; case 'embedding': return '🔢'; case 'reranking': return '🔀'; default: return '⚙';
  }
}

export function snapshotFromLoaded(model?: LoadedModel | null): ModelSnapshot | null {
  if (!model) return null;
  return { name: model.model_name, type: normalizeModelType(model.type), capability: capabilityFromLoaded(model), recipe: model.recipe, device: model.device, checkpoint: model.checkpoint };
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
