import type { ModelInfo } from './api';
import { capabilityFromModelInfo, type ModelCapability } from './modelCapabilities';

export type Capability = 'all' | 'chat' | 'omni' | 'image' | 'transcription' | 'tts' | 'embedding' | 'reranking' | 'vision' | 'code';
export type PresetRecipe = 'llamacpp' | 'sd-cpp' | 'whispercpp' | 'moonshine' | 'flm' | 'ryzenai-llm' | 'vllm' | 'kokoro' | 'auto';

export const KNOWN_CAPABILITIES: Capability[] = ['all', 'chat', 'image', 'omni', 'vision', 'code', 'transcription', 'tts', 'embedding', 'reranking'];

export interface RecipeOptions {
  ctx_size?: number;
  llamacpp_backend?: string;
  llamacpp_device?: string;
  llamacpp_args?: string;
  steps?: number;
  cfg_scale?: number;
  width?: number;
  height?: number;
  sampling_method?: string;
  flow_shift?: number;
  sdcpp_args?: string;
  whispercpp_backend?: string;
  whispercpp_args?: string;
  moonshine_backend?: string;
  moonshine_args?: string;
  vllm_backend?: string;
  vllm_args?: string;
  flm_args?: string;
  merge_args?: boolean;
}

export interface SamplingParams {
  temperature?: number;
  top_p?: number;
  top_k?: number;
  repeat_penalty?: number;
}

export interface Preset {
  id: string;
  name: string;
  description: string;
  applies_to: Capability[];
  recipe_options: RecipeOptions;
  sampling: SamplingParams;
  engine_hint?: PresetRecipe;
  starter: boolean;
  auto_opt_run_id?: string | null;
  auto_opt_enabled?: boolean;
}

export const LS_USER_PRESETS = 'user_presets';
export const LS_APPLIED_PRESETS = 'applied_presets';
export const LS_BACKEND_PRESETS = 'backend_presets';
export const PRESET_STORE_EVENT = 'lemonade:preset-store-changed';

let activeStorageScope = 'guest:shared';

export function setPresetStorageScope(scope: string): void {
  activeStorageScope = scope || 'guest:shared';
}

function scopedPresetKey(key: string): string {
  return `lemonade:${activeStorageScope}:${key}`;
}

function emitPresetStoreEvent(): void {
  try { window.dispatchEvent(new CustomEvent(PRESET_STORE_EVENT)); } catch {}
}

export const DEFAULT_PRESET: Preset = {
  id: 's-default',
  name: 'Default',
  description: 'Use Lemonade defaults and automatic backend selection.',
  applies_to: ['all'],
  recipe_options: { ctx_size: 4096, steps: 20, cfg_scale: 7.0, width: 512, height: 512 },
  sampling: { temperature: 0.70, top_p: 0.90, top_k: 40, repeat_penalty: 1.05 },
  engine_hint: 'auto',
  starter: true,
  auto_opt_enabled: true,
  auto_opt_run_id: null,
};


export function normalizePresetCapabilities(id: string | undefined, caps: Capability[] | undefined): Capability[] {
  const cleaned = [...new Set((caps || []).filter((cap): cap is Capability => KNOWN_CAPABILITIES.includes(cap as Capability)))];
  if (cleaned.includes('all')) return ['all'];
  if (id === DEFAULT_PRESET.id) return ['all'];
  return [cleaned[0] || 'chat'];
}

export function presetSupportsCapability(preset: Pick<Preset, 'id' | 'applies_to'>, cap: Capability): boolean {
  const caps = normalizePresetCapabilities(preset.id, preset.applies_to);
  if (caps.includes('all')) return true;
  return caps.includes(cap);
}

export const STARTERS: Preset[] = [
  { id: 's-balanced', name: 'Balanced', description: 'Sensible defaults. Good first pick for everyday chat.', applies_to: ['chat'], recipe_options: { ctx_size: 4096 }, sampling: { temperature: 0.70, top_p: 0.90, top_k: 40, repeat_penalty: 1.05 }, engine_hint: 'llamacpp', starter: true },
  { id: 's-quality', name: 'Quality', description: 'Larger context, slightly looser sampling for richer long-form answers.', applies_to: ['chat'], recipe_options: { ctx_size: 8192 }, sampling: { temperature: 0.70, top_p: 0.95, top_k: 40, repeat_penalty: 1.10 }, engine_hint: 'llamacpp', starter: true },
  { id: 's-fast', name: 'Fast', description: 'Small context, tight sampling. Snappy responses for quick interactions.', applies_to: ['chat'], recipe_options: { ctx_size: 2048 }, sampling: { temperature: 0.60, top_p: 0.80, top_k: 40, repeat_penalty: 1.05 }, engine_hint: 'llamacpp', starter: true },
  { id: 's-creative', name: 'Creative', description: 'Higher temperature for brainstorming, dialog, and divergent thinking.', applies_to: ['chat'], recipe_options: { ctx_size: 8192 }, sampling: { temperature: 0.95, top_p: 0.95, top_k: 60, repeat_penalty: 1.00 }, engine_hint: 'llamacpp', starter: true },
  { id: 's-long-context', name: 'Long Context', description: 'For documents, codebases, and long conversation threads.', applies_to: ['chat'], recipe_options: { ctx_size: 32768 }, sampling: { temperature: 0.70, top_p: 0.90, top_k: 40, repeat_penalty: 1.05 }, engine_hint: 'llamacpp', starter: true },
  { id: 's-code', name: 'Code', description: 'Low temperature, tight sampling for code generation and refactoring.', applies_to: ['chat'], recipe_options: { ctx_size: 8192 }, sampling: { temperature: 0.20, top_p: 0.95, top_k: 40, repeat_penalty: 1.05 }, engine_hint: 'llamacpp', starter: true },
  { id: 's-sharp', name: 'Sharp', description: 'More steps and tighter guidance for crisp, deliberate image generation.', applies_to: ['image'], recipe_options: { steps: 30, cfg_scale: 8.0 }, sampling: {}, engine_hint: 'sd-cpp', starter: true },
  { id: 's-quick', name: 'Quick', description: 'Fewer steps, looser guidance — fast drafts and iteration.', applies_to: ['image'], recipe_options: { steps: 15, cfg_scale: 7.0 }, sampling: {}, engine_hint: 'sd-cpp', starter: true },
];

export const CAPABILITY_LABELS: Record<Capability, string> = {
  all: 'All',
  chat: 'Chat',
  omni: 'Omni',
  image: 'Image',
  transcription: 'Transcription',
  tts: 'TTS',
  embedding: 'Embedding',
  reranking: 'Reranking',
  vision: 'Vision',
  code: 'Code',
};

const LABEL_MAP: Record<string, Capability> = {
  reasoning: 'chat',
  coding: 'code',
  vision: 'vision',
  'tool-calling': 'chat',
  llm: 'chat',
  omni: 'omni',
  multimodal: 'omni',
  audio: 'transcription',
  transcription: 'transcription',
  'realtime-transcription': 'transcription',
  stt: 'transcription',
  'speech-to-text': 'transcription',
  tts: 'tts',
  image: 'image',
  embedding: 'embedding',
  embeddings: 'embedding',
  reranking: 'reranking',
  rerank: 'reranking',
};

export function labelsFor(model: ModelInfo | string | null | undefined): Capability[] {
  const obj = typeof model === 'string' ? { id: model } as ModelInfo : model;
  const caps: Capability[] = [];
  if (obj?.labels) {
    for (const label of obj.labels) caps.push(LABEL_MAP[label] || (label as Capability));
  }
  const recipe = String(obj?.['recipe'] || '').toLowerCase();
  const recipes = Array.isArray(obj?.recipes) ? obj.recipes : [];
  const recipeText = `${recipe} ${recipes.map(r => String((r as any).recipe || '')).join(' ')}`.toLowerCase();
  const name = String(obj?.id || obj?.name || obj?.display_name || '').toLowerCase();
  if (recipeText.includes('whisper') || recipeText.includes('moonshine') || (recipeText.includes('flm') && (name.includes('whisper') || name.includes('parakeet')))) caps.push('transcription');
  if (recipeText.includes('kokoro')) caps.push('tts');
  if (recipeText.includes('sd-cpp')) caps.push('image');
  if (name.includes('embed')) caps.push('embedding');
  if (name.includes('rerank')) caps.push('reranking');
  const nameHasOmni = /omni|multimodal|vision|llava|qwen.*vl|pixtral|minicpm.*v|mllama/.test(name);
  if (nameHasOmni) caps.push('omni', 'vision');
  const unique = [...new Set(caps)];
  return unique.length > 0 ? unique : ['chat'];
}

export function presetLabelsFor(preset: Preset): Capability[] {
  return preset.applies_to;
}

export function isCompatible(preset: Preset, model: ModelInfo | string | null | undefined): boolean {
  const modelCaps = labelsFor(model);
  const presetCaps = normalizePresetCapabilities(preset.id, preset.applies_to);
  if (presetCaps.includes('all')) return true;
  return presetCaps.some(cap => modelCaps.includes(cap));
}

export function sanitizePreset(p: Partial<Preset>): Preset | null {
  if (!Array.isArray(p.applies_to) || p.applies_to.length === 0) return null;
  const id = p.id || `u-${Date.now()}`;
  return {
    id,
    name: p.name || 'Untitled',
    description: p.description || '',
    applies_to: normalizePresetCapabilities(id, p.applies_to as Capability[]),
    recipe_options: p.recipe_options || {},
    sampling: p.sampling || {},
    engine_hint: p.engine_hint || 'auto',
    starter: p.starter ?? false,
    auto_opt_run_id: p.auto_opt_run_id ?? null,
    auto_opt_enabled: p.auto_opt_enabled ?? true,
  };
}

export function loadUserPresets(): Preset[] {
  try {
    const raw = localStorage.getItem(scopedPresetKey(LS_USER_PRESETS));
    if (raw) return (JSON.parse(raw) as Partial<Preset>[]).map(sanitizePreset).filter((p): p is Preset => !!p);
  } catch {}
  return [];
}

export function loadApplied(): Record<string, string> {
  try {
    const raw = localStorage.getItem(scopedPresetKey(LS_APPLIED_PRESETS));
    if (raw) return JSON.parse(raw);
  } catch {}
  return {};
}

export function loadBackendApplied(): Record<string, string> {
  try {
    const raw = localStorage.getItem(scopedPresetKey(LS_BACKEND_PRESETS));
    if (raw) return JSON.parse(raw);
  } catch {}
  return {};
}

export function saveUserPresets(presets: Preset[]): void {
  localStorage.setItem(scopedPresetKey(LS_USER_PRESETS), JSON.stringify(presets));
  emitPresetStoreEvent();
}

export function saveApplied(applied: Record<string, string>): void {
  localStorage.setItem(scopedPresetKey(LS_APPLIED_PRESETS), JSON.stringify(applied));
  emitPresetStoreEvent();
}

export function saveBackendApplied(applied: Record<string, string>): void {
  localStorage.setItem(scopedPresetKey(LS_BACKEND_PRESETS), JSON.stringify(applied));
  emitPresetStoreEvent();
}

export function allStoredPresets(): Preset[] {
  return [DEFAULT_PRESET, ...STARTERS, ...loadUserPresets()];
}

export function activePresetForModel(modelName: string): Preset {
  const presetId = loadApplied()[modelName] || DEFAULT_PRESET.id;
  return allStoredPresets().find(p => p.id === presetId) || DEFAULT_PRESET;
}

export function activePresetForBackend(key: string): Preset {
  const presetId = loadBackendApplied()[key] || DEFAULT_PRESET.id;
  return allStoredPresets().find(p => p.id === presetId) || DEFAULT_PRESET;
}

function pickRecipeOptions(options: RecipeOptions, keys: Array<keyof RecipeOptions>): RecipeOptions {
  const picked: RecipeOptions = {};
  for (const key of keys) {
    const value = options[key];
    if (value !== undefined && value !== '') {
      (picked as Record<string, unknown>)[key] = value;
    }
  }
  return picked;
}

export function recipeOptionsForCapability(options: RecipeOptions, capability: ModelCapability | 'all' | 'vision' | 'code' | 'transcription'): RecipeOptions {
  if (!options || Object.keys(options).length === 0) return {};

  switch (capability) {
    case 'image':
      return pickRecipeOptions(options, ['steps', 'cfg_scale', 'width', 'height', 'sampling_method', 'flow_shift', 'sdcpp_args', 'merge_args']);
    case 'audio':
    case 'transcription':
      return pickRecipeOptions(options, ['whispercpp_backend', 'whispercpp_args', 'moonshine_backend', 'moonshine_args', 'merge_args']);
    case 'tts':
      return pickRecipeOptions(options, ['merge_args']);
    case 'embedding':
    case 'reranking':
    case 'chat':
    case 'omni':
    case 'vision':
    case 'code':
      return pickRecipeOptions(options, ['ctx_size', 'llamacpp_backend', 'llamacpp_device', 'llamacpp_args', 'flm_args', 'vllm_backend', 'vllm_args', 'merge_args']);
    case 'all':
    default:
      return { ...options };
  }
}

export function recipeOptionsForModel(modelName: string, model?: ModelInfo | null): RecipeOptions | undefined {
  const preset = activePresetForModel(modelName);
  const options = preset.recipe_options || {};
  const scopedOptions = model
    ? recipeOptionsForCapability(options, capabilityFromModelInfo(model))
    : options;
  return Object.keys(scopedOptions || {}).length > 0 ? scopedOptions : undefined;
}

export function samplingForModel(modelName: string): SamplingParams {
  return activePresetForModel(modelName).sampling || {};
}

export function presetIcon(preset: Pick<Preset, 'id' | 'name' | 'starter'> | null | undefined): string {
  if (!preset) return '🧰';
  const id = String(preset.id || '').toLowerCase();
  const name = String(preset.name || '').toLowerCase();
  if (id === DEFAULT_PRESET.id || name === 'default') return '🍋';
  if (name.includes('balanced')) return '⚖️';
  if (name.includes('quality')) return '💎';
  if (name.includes('fast')) return '🏎️';
  if (name.includes('quick')) return '⏱️';
  if (name.includes('creative')) return '✍️';
  if (name.includes('long')) return '📚';
  if (name.includes('code')) return '💻';
  if (name.includes('sharp')) return '🔍';
  if (name.includes('memory')) return '💾';
  return preset.starter ? '🧪' : '🧰';
}
