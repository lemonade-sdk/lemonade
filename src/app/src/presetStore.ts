import type { ModelInfo } from './api';

export type Capability = 'chat' | 'omni' | 'image' | 'transcription' | 'tts' | 'embedding' | 'reranking' | 'vision' | 'code';
export type PresetRecipe = 'llamacpp' | 'sd-cpp' | 'whispercpp' | 'moonshine' | 'flm' | 'ryzenai-llm' | 'vllm' | 'kokoro' | 'auto';

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
}

export const LS_USER_PRESETS = 'user_presets';
export const LS_APPLIED_PRESETS = 'applied_presets';

let activeStorageScope = 'guest:shared';

export function setPresetStorageScope(scope: string): void {
  activeStorageScope = scope || 'guest:shared';
}

function scopedPresetKey(key: string): string {
  return `lemonade:${activeStorageScope}:${key}`;
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
  return preset.applies_to.some(cap => modelCaps.includes(cap));
}

export function sanitizePreset(p: Partial<Preset>): Preset | null {
  if (!Array.isArray(p.applies_to) || p.applies_to.length === 0) return null;
  return {
    id: p.id || `u-${Date.now()}`,
    name: p.name || 'Untitled',
    description: p.description || '',
    applies_to: p.applies_to,
    recipe_options: p.recipe_options || {},
    sampling: p.sampling || {},
    engine_hint: p.engine_hint || 'auto',
    starter: p.starter ?? false,
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

export function saveUserPresets(presets: Preset[]): void {
  localStorage.setItem(scopedPresetKey(LS_USER_PRESETS), JSON.stringify(presets));
}

export function saveApplied(applied: Record<string, string>): void {
  localStorage.setItem(scopedPresetKey(LS_APPLIED_PRESETS), JSON.stringify(applied));
}

export function allStoredPresets(): Preset[] {
  return [...STARTERS, ...loadUserPresets()];
}

export function activePresetForModel(modelName: string): Preset | null {
  const presetId = loadApplied()[modelName];
  return allStoredPresets().find(p => p.id === presetId) || null;
}

export function recipeOptionsForModel(modelName: string): RecipeOptions | undefined {
  return activePresetForModel(modelName)?.recipe_options;
}

export function samplingForModel(modelName: string): SamplingParams {
  return activePresetForModel(modelName)?.sampling || {};
}
