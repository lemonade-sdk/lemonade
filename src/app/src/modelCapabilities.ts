import type { LoadedModel, ModelInfo } from './api';

/**
 * Mirrors the server's deployment modes one-for-one — see the "Deployment
 * labels" table in docs/api/openai.md and find_deployment_mode() in
 * src/cpp/include/lemon/model_types.h.
 *
 * Being a collection is not a capability: an Omni or Router collection serves
 * chat and says so with the `chat` label, so its collection-ness is structure.
 */
export type ModelCapability =
  | 'chat'
  | 'image'
  | 'audio'
  | 'audio-generation'
  | 'tts'
  | 'model3d'
  | 'embedding'
  | 'reranking'
  | 'classification'
  | 'unknown';

export type ModelStructure = 'single' | 'omni' | 'router';

export type ModelIdentity = ModelCapability | 'omni' | 'router';

export interface ModelSnapshot {
  name: string;
  type: string;
  capability: ModelCapability;
  recipe?: string;
  device?: string;
  checkpoint?: string;
}

/**
 * A model declares exactly one of these; the server stamps a recipe default when
 * a registration names none, so anything served carries one. Keep in sync with
 * docs/api/openai.md — model-kind-classification.runtime.cjs fails on drift.
 */
export const DEPLOYMENT_LABEL_KIND: Record<string, ModelCapability> = {
  chat: 'chat',
  transcription: 'audio',
  embeddings: 'embedding',
  embedding: 'embedding',
  reranking: 'reranking',
  image: 'image',
  tts: 'tts',
  'audio-generation': 'audio-generation',
  classification: 'classification',
  classifier: 'classification',
  '3d': 'model3d',
};

/** ModelType strings the router reports for loaded models (model_type_to_string). */
const LOADED_TYPE_KIND: Record<string, ModelCapability> = {
  llm: 'chat',
  embedding: 'embedding',
  reranking: 'reranking',
  transcription: 'audio',
  image: 'image',
  tts: 'tts',
  'audio-generation': 'audio-generation',
  classification: 'classification',
  mesh: 'model3d',
};

const OMNI_COLLECTION_RECIPE = 'collection.omni';
const OMNI_COLLECTION_PREFIX = `${OMNI_COLLECTION_RECIPE}.`;
const ROUTER_COLLECTION_RECIPE = 'collection.router';
const ROUTER_COLLECTION_PREFIX = `${ROUTER_COLLECTION_RECIPE}.`;

const AUDIO_INPUT_LABELS = new Set([
  'audio-input', 'audio-chat', 'chat-transcription', 'speech-input',
]);

export const IMAGE_INPUT_LABELS = new Set([
  'vision', 'image-input', 'vision-language', 'vlm', 'image-text-to-text',
  'multimodal', 'multi-modal',
]);

export function normalizeModelType(type?: string | null): string {
  return (type || 'unknown').toLowerCase().trim() || 'unknown';
}

function normalizedStringList(value: unknown): string[] {
  if (!Array.isArray(value)) return [];
  return value
    .filter((item): item is string => typeof item === 'string')
    .map(item => item.toLowerCase().trim())
    .filter(Boolean);
}

/**
 * `chat` is tested first so an omni model that also declares a mode it could
 * serve standalone still resolves to chat, exactly as find_deployment_mode()
 * does. Input modalities and characteristics name no mode and are ignored.
 */
export function deploymentKindFromLabels(labels?: readonly string[] | null): ModelCapability {
  const lower = normalizedStringList(labels);
  if (lower.includes('chat')) return 'chat';
  for (const label of lower) {
    const kind = DEPLOYMENT_LABEL_KIND[label];
    if (kind) return kind;
  }
  return 'unknown';
}

export function modelStructure(recipe?: string | null): ModelStructure {
  const r = normalizeModelType(recipe);
  if (r === OMNI_COLLECTION_RECIPE || r.startsWith(OMNI_COLLECTION_PREFIX) || r === 'collection') return 'omni';
  if (r === ROUTER_COLLECTION_RECIPE || r.startsWith(ROUTER_COLLECTION_PREFIX)) return 'router';
  return 'single';
}

export function isRouterRecipe(recipe?: string | null): boolean {
  return modelStructure(recipe) === 'router';
}

export function isCollectionRecipe(recipe?: string | null): boolean {
  return modelStructure(recipe) !== 'single';
}

export function capabilityFromModelInfo(model: ModelInfo): ModelCapability {
  return deploymentKindFromLabels(model?.labels);
}

export function capabilityFromLoaded(model?: LoadedModel | null): ModelCapability {
  if (!model) return 'unknown';
  // The router reports the mode it actually launched the subprocess for.
  const fromType = LOADED_TYPE_KIND[normalizeModelType(model.type)];
  if (fromType) return fromType;
  const labelled = deploymentKindFromLabels(model.labels);
  if (labelled !== 'unknown') return labelled;
  // A loaded collection is synthesized client-side from its components, so it
  // carries neither a router ModelType nor labels — only the recipe.
  return isCollectionRecipe(model.recipe) ? 'chat' : 'unknown';
}

/** A collection routes to backends rather than being one, so it shows itself. */
export function identityFor(capability: ModelCapability, recipe?: string | null): ModelIdentity {
  const structure = modelStructure(recipe);
  return structure === 'single' ? capability : structure;
}

export function identityFromModelInfo(model: ModelInfo): ModelIdentity {
  return identityFor(capabilityFromModelInfo(model), (model as any)?.recipe);
}

function descriptorLabels(model?: ModelInfo | LoadedModel | null): string[] {
  if (!model) return [];
  return [
    ...normalizedStringList((model as any).labels),
    ...normalizedStringList((model as any).capabilities),
  ];
}

function inputModalities(model?: ModelInfo | LoadedModel | null): string[] {
  if (!model) return [];
  return normalizedStringList((model as any).input_modalities);
}

function servesChat(model?: ModelInfo | LoadedModel | null): boolean {
  if (!model) return false;
  // Collections orchestrate their components server-side and expose their own
  // surface, so they do not take the chat attachment decoration.
  if (isCollectionRecipe((model as any).recipe)) return false;
  const labelled = deploymentKindFromLabels((model as any).labels);
  if (labelled !== 'unknown') return labelled === 'chat';
  return LOADED_TYPE_KIND[normalizeModelType((model as any).type)] === 'chat';
}

/**
 * True when audio is an input modality of a chat model. Deliberately separate
 * from the primary capability: a multimodal LLM stays in Chat mode and merely
 * gains the audio attachment control. A transcription-only model such as
 * Whisper deploys as Audio and never reaches here.
 */
export function modelSupportsChatAudioInput(
  modelInfo?: ModelInfo | null,
  loadedModel?: LoadedModel | null,
): boolean {
  if (!servesChat(modelInfo) && !servesChat(loadedModel)) return false;
  const modalities = [...inputModalities(modelInfo), ...inputModalities(loadedModel)];
  if (modalities.some(value => value === 'audio' || value === 'speech')) return true;
  return [...descriptorLabels(modelInfo), ...descriptorLabels(loadedModel)]
    .some(value => AUDIO_INPUT_LABELS.has(value));
}

/** Image input as a modality of a chat model, never a mode of its own. */
export function modelSupportsChatImageInput(
  modelInfo?: ModelInfo | null,
  loadedModel?: LoadedModel | null,
): boolean {
  if (!servesChat(modelInfo) && !servesChat(loadedModel)) return false;
  const modalities = [...inputModalities(modelInfo), ...inputModalities(loadedModel)];
  if (modalities.some(value => value === 'image' || value === 'vision')) return true;
  return [...descriptorLabels(modelInfo), ...descriptorLabels(loadedModel)]
    .some(value => IMAGE_INPUT_LABELS.has(value));
}

export function canUseChatCompletions(model?: LoadedModel | null): boolean {
  return capabilityFromLoaded(model) === 'chat';
}

const COMPOSER_CAPABILITIES: ReadonlySet<ModelCapability> = new Set<ModelCapability>([
  'chat', 'image', 'audio', 'audio-generation', 'tts', 'model3d',
]);

export function isComposerSelectableCapability(capability: ModelCapability): boolean {
  return COMPOSER_CAPABILITIES.has(capability);
}

export function canSelectInComposer(model?: LoadedModel | null): boolean {
  return isComposerSelectableCapability(capabilityFromLoaded(model));
}

export function capabilityLabel(capability: ModelIdentity): string {
  switch (capability) {
    case 'chat': return 'Chat';
    case 'omni': return 'Omni';
    case 'router': return 'Router';
    case 'image': return 'Image';
    case 'audio': return 'Audio';
    case 'audio-generation': return 'Music & SFX';
    case 'tts': return 'TTS';
    case 'model3d': return '3D';
    case 'embedding': return 'Embedding';
    case 'reranking': return 'Reranking';
    case 'classification': return 'Classification';
    default: return 'Unknown';
  }
}

export function capabilityBadge(capability: ModelIdentity): string {
  switch (capability) {
    case 'chat': return 'chat';
    case 'omni': return 'omni';
    case 'router': return 'router';
    case 'image': return 'image';
    case 'audio': return 'audio';
    case 'audio-generation': return 'audio-gen';
    case 'tts': return 'tts';
    case 'model3d': return 'model3d';
    case 'embedding': return 'embed';
    case 'reranking': return 'rank';
    case 'classification': return 'classify';
    default: return 'model';
  }
}

export function snapshotIdentity(snapshot?: ModelSnapshot | null): ModelIdentity {
  if (!snapshot) return 'unknown';
  return identityFor(snapshot.capability, snapshot.recipe);
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
  return { name, type: 'unknown', capability: 'unknown' };
}

export function selectPreferredLoadedModel(loadedModels: LoadedModel[]): LoadedModel | null {
  return loadedModels.find(m => capabilityFromLoaded(m) === 'chat')
    || loadedModels.find(m => canSelectInComposer(m))
    || loadedModels[0]
    || null;
}

export function modelDisplayName(model: ModelSnapshot | null | undefined): string { return model?.name || 'Assistant'; }
export function modelInitial(model: ModelSnapshot | null | undefined): string { return modelDisplayName(model).charAt(0).toUpperCase(); }

/* Functional capability tags drive both the model filters and row badges. */
export type CapabilityTag =
  | 'hot' | 'popular' | 'chat' | 'omni' | 'vision' | 'tool' | 'reasoning'
  | 'code' | 'audio' | 'audio-generation' | 'image' | 'tts' | 'model3d'
  | 'embedding' | 'reranking' | 'classification';

export const CAPABILITY_TAG_ORDER: CapabilityTag[] = [
  'hot', 'popular', 'chat', 'omni', 'vision', 'tool', 'reasoning',
  'code', 'audio', 'audio-generation', 'image', 'tts', 'model3d',
  'embedding', 'reranking', 'classification',
];

export const CAPABILITY_TAG_LABELS: Record<CapabilityTag, string> = {
  hot: 'Hot', popular: 'Popular', chat: 'Chat', omni: 'Omni', vision: 'Vision',
  tool: 'Tool use', reasoning: 'Reasoning', code: 'Code', audio: 'Audio',
  'audio-generation': 'Music & SFX', image: 'Image', tts: 'Speech (TTS)',
  model3d: '3D', embedding: 'Embeddings', reranking: 'Reranking',
  classification: 'Classification',
};

const CAPABILITY_TAG_ALIASES: Record<CapabilityTag, string[]> = {
  hot: ['hot'],
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
  classification: ['classification', 'classifier'],
};

const BASE_CAPABILITY_TAG: Partial<Record<ModelIdentity, CapabilityTag>> = {
  chat: 'chat', omni: 'omni', image: 'image', audio: 'audio',
  'audio-generation': 'audio-generation', tts: 'tts', model3d: 'model3d',
  embedding: 'embedding', reranking: 'reranking', classification: 'classification',
};

export function modelCapabilityTags(model: ModelInfo): CapabilityTag[] {
  const declaredCapabilities = Array.isArray((model as any).capabilities)
    ? (model as any).capabilities.filter((value: unknown): value is string => typeof value === 'string')
    : [];
  const labels = [
    ...(model.labels || []),
    ...declaredCapabilities,
    ...inputModalities(model).map(value => `${value}-input`),
  ]
    .map(l => String(l).toLowerCase().trim())
    .filter(Boolean);
  const labelSet = new Set(labels);
  const found = new Set<CapabilityTag>();
  for (const tag of CAPABILITY_TAG_ORDER) {
    if (tag === 'omni') continue;
    if (CAPABILITY_TAG_ALIASES[tag].some(alias => labelSet.has(alias))) found.add(tag);
  }
  const base = BASE_CAPABILITY_TAG[identityFromModelInfo(model)];
  if (base) found.add(base);
  if (modelSupportsChatAudioInput(model, null)) found.add('audio');
  return CAPABILITY_TAG_ORDER.filter(tag => found.has(tag));
}

export function modelMatchesCapabilityTags(model: ModelInfo, selected: Set<string>): boolean {
  if (!selected || selected.size === 0) return true;
  return modelCapabilityTags(model).some(tag => selected.has(tag));
}
