import type { ModelInfo } from './api';
import { capabilityFromModelInfo, type ModelCapability } from './modelCapabilities';
import {
  NO_SYSTEM_PROMPT_ID,
  defaultSystemPromptIdForPreset,
  defaultToolsEnabledForPreset,
  sanitizeSystemPrompts,
  starterSystemPromptsForPreset,
  type PresetSystemPrompt,
} from './presetPrompts';
export { CUSTOM_PRESET_PROMPTS, NO_SYSTEM_PROMPT_ID, newCustomSystemPrompt, type PresetSystemPrompt } from './presetPrompts';

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
  voice?: string;
  speed?: number;
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
  system_prompt_id?: string;
  system_prompts?: PresetSystemPrompt[];
  tools_enabled?: boolean;
}

export interface ModelTuning {
  /** Load-time runtime options for this concrete model. */
  recipe_options: RecipeOptions;
  /** Request-time sampling defaults for this concrete model. */
  sampling: SamplingParams;
  /** Optional runtime hint kept separate from shared Preset intent. */
  engine_hint?: PresetRecipe;
  /** 'model' means discovered from model metadata/GGUF; 'user' means locally customized. */
  source?: 'model' | 'user';
  updated_at?: string;
}

export const LS_USER_PRESETS = 'user_presets';
export const LS_APPLIED_PRESETS = 'applied_presets';
export const LS_BACKEND_PRESETS = 'backend_presets';
export const LS_MODEL_TUNINGS = 'model_tunings';
// Client-local record of which preset each *currently-loaded* model is actually
// running with. Distinct from `applied_presets` (the preset LINKED to a model):
// the linked preset can be changed while a model is loaded, and the divergence
// between linked vs running is what surfaces the "Update preset" affordance
// (#2356). Cleared when the model unloads. Never sent to lemond.
export const LS_RUNNING_PRESETS = 'running_presets';
export const PRESET_STORE_EVENT = 'lemonade:preset-store-changed';
export const DEFAULT_CONTEXT_SIZE = 4096;

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
  description: 'Use current model settings and automatic backend selection.',
  applies_to: ['all'],
  recipe_options: {},
  sampling: {},
  engine_hint: 'auto',
  starter: true,
  auto_opt_enabled: true,
  auto_opt_run_id: null,
  system_prompt_id: NO_SYSTEM_PROMPT_ID,
  system_prompts: [],
  tools_enabled: true,
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

const STARTER_BASE: Preset[] = [
  { id: 's-balanced', name: 'Balanced', description: 'Everyday chat intent. Concrete runtime values come from this model tuning.', applies_to: ['chat'], recipe_options: {}, sampling: {}, engine_hint: 'auto', starter: true },
  { id: 's-thorough', name: 'Thorough', description: 'Careful answers for analysis, planning, debugging, and decisions.', applies_to: ['chat'], recipe_options: {}, sampling: {}, engine_hint: 'auto', starter: true },
  { id: 's-quick-chat', name: 'Quick Chat', description: 'Snappy responses for quick interactions.', applies_to: ['chat'], recipe_options: {}, sampling: {}, engine_hint: 'auto', starter: true },
  { id: 's-creative', name: 'Creative', description: 'Brainstorming, dialog, and divergent writing intent.', applies_to: ['chat'], recipe_options: {}, sampling: {}, engine_hint: 'auto', starter: true },
  { id: 's-long-context', name: 'Long Context', description: 'For documents, codebases, and long conversation threads.', applies_to: ['chat'], recipe_options: {}, sampling: {}, engine_hint: 'auto', starter: true },
  { id: 's-code', name: 'Code', description: 'Coding, refactoring, and technical review intent.', applies_to: ['chat'], recipe_options: {}, sampling: {}, engine_hint: 'auto', starter: true },
  { id: 's-quality', name: 'Quality', description: 'Crisp, deliberate image generation intent.', applies_to: ['image'], recipe_options: {}, sampling: {}, engine_hint: 'auto', starter: true },
  { id: 's-preview', name: 'Preview', description: 'Fast image drafts and iteration intent.', applies_to: ['image'], recipe_options: {}, sampling: {}, engine_hint: 'auto', starter: true },
  { id: 's-turbo', name: 'Turbo', description: 'Fastest image draft intent for rapid iteration.', applies_to: ['image'], recipe_options: {}, sampling: {}, engine_hint: 'auto', starter: true },
];

export const STARTERS: Preset[] = STARTER_BASE.map(preset => ({
  ...preset,
  system_prompt_id: defaultSystemPromptIdForPreset(preset.id),
  system_prompts: starterSystemPromptsForPreset(preset.id),
  tools_enabled: defaultToolsEnabledForPreset(preset.id),
}));


function formatDash(value: unknown, digits?: number): string {
  const n = Number(value);
  if (!Number.isFinite(n)) return '---';
  return digits === undefined ? String(Math.round(n)) : n.toFixed(digits);
}

function isChatPreviewCapability(capability: ModelCapability | null | undefined): boolean {
  return capability === 'chat' || capability === 'omni' || capability === 'unknown';
}

function capabilityForPresetPreview(capability: ModelCapability | null | undefined): Capability | null {
  switch (capability) {
    case 'chat': return 'chat';
    case 'omni': return 'omni';
    case 'image': return 'image';
    case 'audio': return 'transcription';
    case 'tts': return 'tts';
    case 'embedding': return 'embedding';
    case 'reranking': return 'reranking';
    default: return null;
  }
}

function hasOwnPreviewValue(value: unknown): boolean {
  return value !== undefined && value !== null && value !== '';
}

function previewContext(value: unknown, fallbackCtxSize?: unknown): number {
  const n = Number(value);
  if (Number.isFinite(n) && n > 0) return Math.round(n);
  const fallback = Number(fallbackCtxSize);
  if (Number.isFinite(fallback) && fallback > 0) return Math.round(fallback);
  return DEFAULT_CONTEXT_SIZE;
}

function readNumberFrom(value: unknown, paths: string[][]): number | undefined {
  for (const path of paths) {
    let cur: unknown = value;
    for (const key of path) {
      if (!cur || typeof cur !== 'object') { cur = undefined; break; }
      cur = (cur as Record<string, unknown>)[key];
    }
    const n = Number(cur);
    if (Number.isFinite(n) && n > 0) return n;
  }
  return undefined;
}

const CONTEXT_PATHS = [
  ['recipe_options', 'ctx_size'], ['options', 'ctx_size'], ['ctx_size'], ['n_ctx'],
  ['max_context_window'], ['max_ctx_size'], ['max_ctx'], ['context_window'], ['context_length'], ['max_sequence_length'],
];

function contextFromRecipe(recipe: unknown): number | undefined {
  return readNumberFrom(recipe, CONTEXT_PATHS);
}

function recipesForModel(model: ModelInfo | null | undefined): unknown[] {
  return Array.isArray(model?.recipes) ? model!.recipes : [];
}

function recipeName(recipe: unknown): string {
  if (!recipe || typeof recipe !== 'object') return '';
  return String((recipe as Record<string, unknown>).recipe
    || (recipe as Record<string, unknown>).name
    || (recipe as Record<string, unknown>).id
    || '').trim().toLowerCase();
}

function readNumberFromActiveRecipe(model: ModelInfo | null | undefined, paths: string[][]): number | undefined {
  const recipes = recipesForModel(model);
  const activeRecipe = String((model as Record<string, unknown> | null | undefined)?.recipe || '').trim().toLowerCase();
  if (activeRecipe) {
    for (const recipe of recipes) {
      if (recipeName(recipe) === activeRecipe) {
        const n = readNumberFrom(recipe, paths);
        if (n) return n;
      }
    }
  }
  for (const recipe of recipes) {
    const n = readNumberFrom(recipe, paths);
    if (n) return n;
  }
  return undefined;
}

function readNumberFromModelOrRecipe(model: ModelInfo | null | undefined, paths: string[][]): number | undefined {
  return readNumberFrom(model, paths) ?? readNumberFromActiveRecipe(model, paths);
}

export function modelContextSize(model: ModelInfo | null | undefined, fallbackCtxSize?: unknown): number {
  const fromModel = readNumberFrom(model, CONTEXT_PATHS);
  if (fromModel) return Math.round(fromModel);

  const fromRecipe = readNumberFromActiveRecipe(model, CONTEXT_PATHS);
  if (fromRecipe) return Math.round(fromRecipe);

  return previewContext(undefined, fallbackCtxSize);
}

export function presetHasOverrides(preset: Pick<Preset, 'recipe_options' | 'sampling'>): boolean {
  const recipeOptions = preset.recipe_options || {};
  const sampling = preset.sampling || {};
  return Object.values(recipeOptions).some(value => value !== undefined && value !== '')
    || Object.values(sampling).some(value => value !== undefined && value !== '');
}

export function presetHasApplicablePreviewOverrides(preset: Pick<Preset, 'recipe_options' | 'sampling'>, capability?: ModelCapability | null): boolean {
  const recipeOptions = capability
    ? recipeOptionsForCapability(preset.recipe_options || {}, capability)
    : (preset.recipe_options || {});
  const sampling = !capability || isChatPreviewCapability(capability) ? (preset.sampling || {}) : {};
  return Object.values(recipeOptions).some(hasOwnPreviewValue)
    || Object.values(sampling).some(hasOwnPreviewValue);
}

export function presetParamPreviewLines(preset: Preset, modelCapability?: ModelCapability | null, fallbackCtxSize?: unknown): string[] {
  const caps = normalizePresetCapabilities(preset.id, preset.applies_to);
  const ro = preset.recipe_options || {};
  const sp = preset.sampling || {};
  const targetCap = capabilityForPresetPreview(modelCapability);
  if (targetCap && !caps.includes('all') && !caps.includes(targetCap)) return ['---'];

  const showChat = modelCapability
    ? isChatPreviewCapability(modelCapability)
    : caps.includes('all') || caps.some(cap => cap === 'chat' || cap === 'omni' || cap === 'code' || cap === 'vision');
  const showImage = modelCapability
    ? modelCapability === 'image'
    : caps.includes('all') || caps.includes('image');
  const hasImageValues = hasOwnPreviewValue(ro.steps) || hasOwnPreviewValue(ro.cfg_scale);
  const showTts = modelCapability
    ? modelCapability === 'tts'
    : caps.includes('all') || caps.includes('tts');
  const hasTtsValues = hasOwnPreviewValue(ro.voice) || hasOwnPreviewValue(ro.speed);
  const lines: string[] = [];

  if (showChat) {
    lines.push(`temp ${formatDash(sp.temperature, 2)} · ctx ${formatDash(previewContext(ro.ctx_size, fallbackCtxSize))}`);
  }
  if (showImage && (hasImageValues || !showChat)) {
    lines.push(`${formatDash(ro.steps)} steps · cfg ${formatDash(ro.cfg_scale, 1)}`);
  }
  if (showTts && (hasTtsValues || (!showChat && !showImage))) {
    const voice = String(ro.voice || '---');
    const speed = hasOwnPreviewValue(ro.speed) ? ` · speed ${formatDash(ro.speed, 2)}` : '';
    lines.push(`voice ${voice}${speed}`);
  }
  return lines.length ? lines : ['---'];
}

export function presetParamPreview(preset: Preset): string {
  return presetParamPreviewLines(preset).join(' · ');
}

export function modelDefaultParamPreviewLines(model: ModelInfo | null | undefined, fallbackCtxSize?: unknown): string[] {
  if (!model) return [`temp --- · ctx ${formatDash(previewContext(undefined, fallbackCtxSize))}`];
  const capability = capabilityFromModelInfo(model);
  const candidate = model as Record<string, unknown>;
  const ctx = modelContextSize(model, fallbackCtxSize);
  const temperature = readNumberFrom(candidate, [
    ['sampling', 'temperature'], ['sample_params', 'temperature'], ['recipe_options', 'temperature'], ['temperature'],
  ]);
  const steps = readNumberFromModelOrRecipe(model, [
    ['recipe_options', 'steps'], ['recipe_options', 'sample_steps'], ['sample_params', 'sample_steps'], ['sample_params', 'steps'], ['steps'], ['sample_steps'],
  ]);
  const cfg = readNumberFromModelOrRecipe(model, [
    ['recipe_options', 'cfg_scale'], ['recipe_options', 'txt_cfg'], ['sample_params', 'guidance', 'txt_cfg'], ['sample_params', 'cfg_scale'], ['txt_cfg'], ['guidance'], ['cfg_scale'],
  ]);

  if (capability === 'image') {
    return [`${formatDash(steps)} steps · cfg ${formatDash(cfg, 1)}`];
  }
  if (capability === 'tts') {
    const voice = String((model as Record<string, unknown>).voice || (model as Record<string, unknown>).default_voice || '---');
    return [`voice ${voice}`];
  }
  if (isChatPreviewCapability(capability)) {
    return [`temp ${formatDash(temperature, 2)} · ctx ${formatDash(ctx)}`];
  }
  return ['---'];
}

export function effectivePresetParamPreviewLines(preset: Preset, model?: ModelInfo | null, fallbackCtxSize?: unknown): string[] {
  const ctxFallback = model ? modelContextSize(model, fallbackCtxSize) : fallbackCtxSize;
  const capability = model ? capabilityFromModelInfo(model) : undefined;
  const caps = normalizePresetCapabilities(preset.id, preset.applies_to);
  const targetCap = capabilityForPresetPreview(capability);
  if (targetCap && !caps.includes('all') && !caps.includes(targetCap)) return ['---'];
  if (model && !presetHasApplicablePreviewOverrides(preset, capability)) {
    return modelDefaultParamPreviewLines(model, ctxFallback);
  }
  return presetParamPreviewLines(preset, capability, ctxFallback);
}

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
  const isDefault = id === DEFAULT_PRESET.id;
  const normalizedAppliesTo = normalizePresetCapabilities(id, p.applies_to);
  const supportsTools = normalizedAppliesTo.includes('all') || normalizedAppliesTo.some(cap => cap === 'chat' || cap === 'omni' || cap === 'code' || cap === 'vision');
  const systemPrompts = isDefault ? [] : sanitizeSystemPrompts(p.system_prompts, id);
  const requestedPromptId = typeof p.system_prompt_id === 'string' ? p.system_prompt_id : defaultSystemPromptIdForPreset(id);
  const hasRequestedPrompt = systemPrompts.some(prompt => prompt.id === requestedPromptId);
  const systemPromptId = isDefault
    ? NO_SYSTEM_PROMPT_ID
    : (requestedPromptId === NO_SYSTEM_PROMPT_ID ? NO_SYSTEM_PROMPT_ID : (hasRequestedPrompt ? requestedPromptId : (systemPrompts[0]?.id || NO_SYSTEM_PROMPT_ID)));
  return {
    id,
    name: p.name || 'Untitled',
    description: p.description || '',
    applies_to: normalizedAppliesTo,
    recipe_options: p.recipe_options || {},
    sampling: p.sampling || {},
    engine_hint: p.engine_hint || 'auto',
    starter: p.starter ?? false,
    auto_opt_run_id: p.auto_opt_run_id ?? null,
    auto_opt_enabled: p.auto_opt_enabled ?? true,
    system_prompt_id: systemPromptId,
    system_prompts: systemPrompts,
    tools_enabled: supportsTools && (typeof p.tools_enabled === 'boolean' ? p.tools_enabled : defaultToolsEnabledForPreset(id)),
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

function isBlankValue(value: unknown): boolean {
  return value === undefined || value === null || value === '';
}

function positiveNumberValue(value: unknown): number | undefined {
  const n = Number(value);
  return Number.isFinite(n) && n > 0 ? n : undefined;
}

function optionalNumberValue(value: unknown): number | undefined {
  if (isBlankValue(value)) return undefined;
  const n = Number(value);
  return Number.isFinite(n) ? n : undefined;
}

function optionalStringValue(value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined;
  const trimmed = value.trim();
  return trimmed ? trimmed : undefined;
}

const RECIPE_OPTION_NUMBER_KEYS = new Set<keyof RecipeOptions>([
  'ctx_size', 'steps', 'cfg_scale', 'width', 'height', 'flow_shift', 'speed',
]);

export function sanitizeRecipeOptions(options: Partial<RecipeOptions> | null | undefined): RecipeOptions {
  const out: RecipeOptions = {};
  if (!options || typeof options !== 'object') return out;
  for (const [key, value] of Object.entries(options) as Array<[keyof RecipeOptions, unknown]>) {
    if (isBlankValue(value)) continue;
    if (key === 'merge_args') {
      if (typeof value === 'boolean') out.merge_args = value;
      continue;
    }
    if (RECIPE_OPTION_NUMBER_KEYS.has(key)) {
      const n = optionalNumberValue(value);
      if (n !== undefined) (out as Record<string, unknown>)[key] = n;
      continue;
    }
    if (typeof value === 'string') {
      const str = optionalStringValue(value);
      if (str !== undefined) (out as Record<string, unknown>)[key] = str;
      continue;
    }
    (out as Record<string, unknown>)[key] = value;
  }
  return out;
}

export function sanitizeSamplingParams(params: Partial<SamplingParams> | null | undefined): SamplingParams {
  const out: SamplingParams = {};
  if (!params || typeof params !== 'object') return out;
  for (const key of ['temperature', 'top_p', 'top_k', 'repeat_penalty'] as Array<keyof SamplingParams>) {
    const n = optionalNumberValue(params[key]);
    if (n !== undefined) out[key] = n;
  }
  return out;
}

export function sanitizeModelTuning(raw: Partial<ModelTuning> | null | undefined): ModelTuning {
  const recipe_options = sanitizeRecipeOptions(raw?.recipe_options || {});
  const sampling = sanitizeSamplingParams(raw?.sampling || {});
  const engine_hint = raw?.engine_hint && ['auto', 'llamacpp', 'sd-cpp', 'whispercpp', 'moonshine', 'flm', 'ryzenai-llm', 'vllm', 'kokoro'].includes(String(raw.engine_hint))
    ? raw.engine_hint
    : undefined;
  return {
    recipe_options,
    sampling,
    ...(engine_hint ? { engine_hint } : {}),
    source: raw?.source === 'user' ? 'user' : (raw?.source === 'model' ? 'model' : undefined),
    updated_at: typeof raw?.updated_at === 'string' ? raw.updated_at : undefined,
  };
}

export function loadModelTunings(): Record<string, ModelTuning> {
  try {
    const raw = localStorage.getItem(scopedPresetKey(LS_MODEL_TUNINGS));
    const parsed = raw ? JSON.parse(raw) : {};
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) return {};
    return Object.fromEntries(Object.entries(parsed).map(([name, tuning]) => [name, sanitizeModelTuning(tuning as Partial<ModelTuning>)]));
  } catch {}
  return {};
}

export function saveModelTunings(tunings: Record<string, ModelTuning>): void {
  localStorage.setItem(scopedPresetKey(LS_MODEL_TUNINGS), JSON.stringify(tunings));
  emitPresetStoreEvent();
}

export function loadModelTuning(modelName: string): ModelTuning | null {
  if (!modelName) return null;
  return loadModelTunings()[modelName] || null;
}

export function saveModelTuning(modelName: string, tuning: Partial<ModelTuning>): void {
  if (!modelName) return;
  const sanitized = sanitizeModelTuning({ ...tuning, source: 'user', updated_at: new Date().toISOString() });
  const hasValues = Object.keys(sanitized.recipe_options).length > 0 || Object.keys(sanitized.sampling).length > 0 || !!sanitized.engine_hint;
  const next = { ...loadModelTunings() };
  if (hasValues) next[modelName] = sanitized;
  else delete next[modelName];
  saveModelTunings(next);
}

export function resetModelTuning(modelName: string): void {
  if (!modelName) return;
  const next = { ...loadModelTunings() };
  delete next[modelName];
  saveModelTunings(next);
}

export function hasModelTuning(modelName: string): boolean {
  const tuning = loadModelTuning(modelName);
  return !!tuning && (Object.keys(tuning.recipe_options).length > 0 || Object.keys(tuning.sampling).length > 0 || !!tuning.engine_hint);
}

function readStringFrom(value: unknown, paths: string[][]): string | undefined {
  for (const path of paths) {
    let cur: unknown = value;
    for (const key of path) {
      if (!cur || typeof cur !== 'object') { cur = undefined; break; }
      cur = (cur as Record<string, unknown>)[key];
    }
    const str = optionalStringValue(cur);
    if (str !== undefined) return str;
  }
  return undefined;
}

function activeRecipeName(model: ModelInfo | null | undefined): string {
  const direct = String((model as Record<string, unknown> | null | undefined)?.recipe || '').trim().toLowerCase();
  if (direct) return direct;
  const first = recipesForModel(model)[0];
  return recipeName(first);
}

function readStringFromModelOrRecipe(model: ModelInfo | null | undefined, paths: string[][]): string | undefined {
  return readStringFrom(model, paths) ?? readStringFromActiveRecipe(model, paths);
}

function readStringFromActiveRecipe(model: ModelInfo | null | undefined, paths: string[][]): string | undefined {
  const recipes = recipesForModel(model);
  const activeRecipe = activeRecipeName(model);
  if (activeRecipe) {
    for (const recipe of recipes) {
      if (recipeName(recipe) === activeRecipe) {
        const str = readStringFrom(recipe, paths);
        if (str !== undefined) return str;
      }
    }
  }
  for (const recipe of recipes) {
    const str = readStringFrom(recipe, paths);
    if (str !== undefined) return str;
  }
  return undefined;
}

export function modelDefaultRecipeOptions(model: ModelInfo | null | undefined, fallbackCtxSize?: unknown): RecipeOptions {
  if (!model) return {};
  const recipe = activeRecipeName(model);
  const capability = capabilityFromModelInfo(model);
  const out: RecipeOptions = {};
  const ctx = modelContextSize(model, fallbackCtxSize);

  if (capability === 'chat' || capability === 'omni' || capability === 'unknown') {
    if (ctx) out.ctx_size = ctx;
  }

  const backend = readStringFromModelOrRecipe(model, [
    ['recipe_options', 'backend'], ['options', 'backend'], ['backend'], ['default_backend'], ['recommended_backend'],
  ]);

  if (recipe === 'llamacpp' || recipe === '') {
    if (ctx) out.ctx_size = ctx;
    out.llamacpp_backend = readStringFromModelOrRecipe(model, [['recipe_options', 'llamacpp_backend'], ['options', 'llamacpp_backend'], ['llamacpp_backend']]) ?? backend;
    out.llamacpp_device = readStringFromModelOrRecipe(model, [['recipe_options', 'llamacpp_device'], ['options', 'llamacpp_device'], ['llamacpp_device'], ['device']]);
    out.llamacpp_args = readStringFromModelOrRecipe(model, [['recipe_options', 'llamacpp_args'], ['options', 'llamacpp_args'], ['llamacpp_args'], ['args']]);
  }
  if (recipe === 'flm') {
    if (ctx) out.ctx_size = ctx;
    out.flm_args = readStringFromModelOrRecipe(model, [['recipe_options', 'flm_args'], ['options', 'flm_args'], ['flm_args'], ['args']]);
  }
  if (recipe === 'vllm') {
    if (ctx) out.ctx_size = ctx;
    out.vllm_backend = readStringFromModelOrRecipe(model, [['recipe_options', 'vllm_backend'], ['options', 'vllm_backend'], ['vllm_backend']]) ?? backend;
    out.vllm_args = readStringFromModelOrRecipe(model, [['recipe_options', 'vllm_args'], ['options', 'vllm_args'], ['vllm_args'], ['args']]);
  }
  if (recipe === 'ryzenai-llm') {
    if (ctx) out.ctx_size = ctx;
  }

  if (capability === 'image' || recipe === 'sd-cpp') {
    out.steps = readNumberFromModelOrRecipe(model, [['recipe_options', 'steps'], ['sample_params', 'steps'], ['sample_steps'], ['steps']]);
    out.cfg_scale = readNumberFromModelOrRecipe(model, [['recipe_options', 'cfg_scale'], ['sample_params', 'cfg_scale'], ['sample_params', 'guidance', 'txt_cfg'], ['txt_cfg'], ['cfg_scale']]);
    out.width = readNumberFromModelOrRecipe(model, [['recipe_options', 'width'], ['sample_params', 'width'], ['width']]);
    out.height = readNumberFromModelOrRecipe(model, [['recipe_options', 'height'], ['sample_params', 'height'], ['height']]);
    out.sampling_method = readStringFromModelOrRecipe(model, [['recipe_options', 'sampling_method'], ['sample_params', 'sampling_method'], ['sampling_method']]);
    out.flow_shift = readNumberFromModelOrRecipe(model, [['recipe_options', 'flow_shift'], ['sample_params', 'flow_shift'], ['flow_shift']]);
    out.sdcpp_args = readStringFromModelOrRecipe(model, [['recipe_options', 'sdcpp_args'], ['options', 'sdcpp_args'], ['sdcpp_args'], ['args']]);
  }

  if (capability === 'audio' || recipe === 'whispercpp' || recipe === 'moonshine') {
    if (recipe === 'moonshine') {
      out.moonshine_backend = readStringFromModelOrRecipe(model, [['recipe_options', 'moonshine_backend'], ['options', 'moonshine_backend'], ['moonshine_backend']]) ?? backend;
      out.moonshine_args = readStringFromModelOrRecipe(model, [['recipe_options', 'moonshine_args'], ['options', 'moonshine_args'], ['moonshine_args'], ['args']]);
    } else {
      out.whispercpp_backend = readStringFromModelOrRecipe(model, [['recipe_options', 'whispercpp_backend'], ['options', 'whispercpp_backend'], ['whispercpp_backend']]) ?? backend;
      out.whispercpp_args = readStringFromModelOrRecipe(model, [['recipe_options', 'whispercpp_args'], ['options', 'whispercpp_args'], ['whispercpp_args'], ['args']]);
    }
  }

  if (capability === 'tts' || recipe === 'kokoro') {
    out.voice = readStringFromModelOrRecipe(model, [['recipe_options', 'voice'], ['sample_params', 'voice'], ['default_voice'], ['voice']]);
    out.speed = readNumberFromModelOrRecipe(model, [['recipe_options', 'speed'], ['sample_params', 'speed'], ['default_speed'], ['speed']]);
  }

  return sanitizeRecipeOptions(out);
}

export function modelDefaultSampling(model: ModelInfo | null | undefined): SamplingParams {
  if (!model) return {};
  return sanitizeSamplingParams({
    temperature: readNumberFrom(model, [['sampling', 'temperature'], ['sample_params', 'temperature'], ['recipe_options', 'temperature'], ['temperature']]),
    top_p: readNumberFrom(model, [['sampling', 'top_p'], ['sample_params', 'top_p'], ['recipe_options', 'top_p'], ['top_p']]),
    top_k: readNumberFrom(model, [['sampling', 'top_k'], ['sample_params', 'top_k'], ['recipe_options', 'top_k'], ['top_k']]),
    repeat_penalty: readNumberFrom(model, [['sampling', 'repeat_penalty'], ['sample_params', 'repeat_penalty'], ['recipe_options', 'repeat_penalty'], ['repeat_penalty']]),
  });
}

export function modelBaseTuningForModel(model: ModelInfo | null | undefined, fallbackCtxSize?: unknown): ModelTuning {
  return {
    recipe_options: modelDefaultRecipeOptions(model, fallbackCtxSize),
    sampling: modelDefaultSampling(model),
    engine_hint: (activeRecipeName(model) as PresetRecipe) || 'auto',
    source: 'model',
  };
}

export function effectiveModelTuningForModel(modelName: string, model: ModelInfo | null | undefined, fallbackCtxSize?: unknown): ModelTuning {
  const base = modelBaseTuningForModel(model, fallbackCtxSize);
  const user = loadModelTuning(modelName);
  if (!user) return base;
  return {
    recipe_options: { ...base.recipe_options, ...user.recipe_options },
    sampling: { ...base.sampling, ...user.sampling },
    engine_hint: user.engine_hint || base.engine_hint,
    source: user.source || 'user',
    updated_at: user.updated_at,
  };
}

export function allStoredPresets(): Preset[] {
  return [DEFAULT_PRESET, ...STARTERS, ...loadUserPresets()];
}

export function selectedSystemPromptForPreset(preset: Preset | null | undefined): PresetSystemPrompt | null {
  if (!preset || preset.system_prompt_id === NO_SYSTEM_PROMPT_ID) return null;
  const prompts = sanitizeSystemPrompts(preset.system_prompts, preset.id);
  const selected = prompts.find(prompt => prompt.id === preset.system_prompt_id) || prompts[0];
  return selected && selected.prompt.trim() ? selected : null;
}

export function systemPromptTextForPreset(preset: Preset | null | undefined): string | null {
  return selectedSystemPromptForPreset(preset)?.prompt.trim() || null;
}

export function systemPromptNameForPreset(preset: Preset | null | undefined): string {
  if (!preset || preset.system_prompt_id === NO_SYSTEM_PROMPT_ID) return 'No prompt';
  return selectedSystemPromptForPreset(preset)?.name || 'No prompt';
}

export function activePresetForModel(modelName: string): Preset {
  const presetId = loadApplied()[modelName] || DEFAULT_PRESET.id;
  return allStoredPresets().find(p => p.id === presetId) || DEFAULT_PRESET;
}

export function activePresetForBackend(key: string): Preset {
  const presetId = loadBackendApplied()[key] || DEFAULT_PRESET.id;
  return allStoredPresets().find(p => p.id === presetId) || DEFAULT_PRESET;
}

/**
 * Per-recipe field in RecipeOptions that names the concrete backend the runtime
 * will bind at load time (e.g. `llamacpp_backend: 'vulkan'`). Used to resolve the
 * EXACT backend a load will use so we only merge the matching `recipe:backend`
 * binding (#2432 round-3).
 */
const BACKEND_FIELD_BY_RECIPE: Record<string, keyof RecipeOptions> = {
  llamacpp: 'llamacpp_backend',
  vllm: 'vllm_backend',
  whispercpp: 'whispercpp_backend',
  moonshine: 'moonshine_backend',
};

function normalizeBackendValue(value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined;
  const trimmed = value.trim().toLowerCase();
  return trimmed ? trimmed : undefined;
}

/** Read `recipes[recipe].default_backend` from a /system-info payload. */
function recipeDefaultBackend(systemInfo: Record<string, unknown> | null | undefined, recipe: string): string | undefined {
  const recipes = (systemInfo?.recipes ?? null) as Record<string, { default_backend?: unknown }> | null;
  if (!recipes) return undefined;
  return normalizeBackendValue(recipes[recipe]?.default_backend);
}

/**
 * Resolve the CONCRETE backend that a load for `recipe` will actually use, in
 * sensible precedence: explicit load options → model-preset recipe_options →
 * recipe default backend (from /system-info). The backend preset's own value is
 * deliberately NOT consulted here — that would be circular (we're deciding
 * whether the backend preset even applies).
 */
function concreteBackendForRecipe(
  recipe: string,
  explicitOptions: RecipeOptions | null | undefined,
  modelPresetOptions: RecipeOptions | null | undefined,
  systemInfo: Record<string, unknown> | null | undefined,
): string | undefined {
  const field = BACKEND_FIELD_BY_RECIPE[recipe];
  if (!field) return undefined;
  return normalizeBackendValue(explicitOptions?.[field])
    ?? normalizeBackendValue(modelPresetOptions?.[field])
    ?? recipeDefaultBackend(systemInfo, recipe);
}

export interface BackendResolutionContext {
  /** Options passed explicitly to the load call (highest precedence). */
  explicitOptions?: RecipeOptions | null;
  /** The model preset's recipe_options (model-specific defaults). */
  modelPresetOptions?: RecipeOptions | null;
  /** /system-info payload, used for the recipe's default_backend. */
  systemInfo?: Record<string, unknown> | null;
}

/**
 * #2432 (round-3): resolve the GLOBAL backend preset that applies to a model's
 * load, if any.
 *
 * Backend presets are keyed by the EXACT `recipe:backend` pair (e.g.
 * `llamacpp:vulkan`). A binding therefore only applies when the CONCRETE backend
 * that this load will use equals the binding's backend part — NOT merely when the
 * recipe matches. We resolve the concrete backend the same way the load does
 * (explicit options → model preset → recipe default_backend) and only merge the
 * preset bound to that exact key. This keeps the semantics "all models using this
 * BACKEND" rather than the wrong "all models using this RECIPE".
 *
 * Default is treated as "no backend preset" (returns null) so it never
 * contributes args — consistent with the Backend view hiding Default.
 */
export function activePresetForModelBackend(model?: ModelInfo | null, ctx?: BackendResolutionContext): Preset | null {
  if (!model) return null;
  const applied = loadBackendApplied();
  if (Object.keys(applied).length === 0) return null;

  // Ordered list of the model's recipes (active recipe first).
  const recipes: string[] = [];
  const active = String((model as Record<string, unknown>).recipe || '').trim().toLowerCase();
  if (active) recipes.push(active);
  for (const recipe of recipesForModel(model)) {
    const name = recipeName(recipe);
    if (name && !recipes.includes(name)) recipes.push(name);
  }
  if (recipes.length === 0) return null;

  for (const recipe of recipes) {
    const backend = concreteBackendForRecipe(recipe, ctx?.explicitOptions, ctx?.modelPresetOptions, ctx?.systemInfo);
    if (!backend) continue;
    const presetId = applied[`${recipe}:${backend}`];
    if (!presetId || presetId === DEFAULT_PRESET.id) continue;
    const preset = allStoredPresets().find(p => p.id === presetId);
    if (preset) return preset;
  }
  return null;
}

/* ── Running-preset store (#2356: update preset while loaded) ─────
 *
 * Tracks the preset a *loaded* model is currently running with. When a model
 * is loaded we snapshot its running preset = its linked preset at load time.
 * If the user later re-links a different preset, `running` lags behind `linked`
 * and an "Update preset" action becomes available in the detail panel.
 *
 * This is purely client-local bookkeeping for the POC. Live (request-time)
 * changes are applied by request composition; load-time changes go through a
 * real reload (`api.reloadModel` = unload + load). See UPDATE_PRESET_CONTRACT.md. */

export function loadRunningPresets(): Record<string, string> {
  try {
    const raw = localStorage.getItem(scopedPresetKey(LS_RUNNING_PRESETS));
    if (raw) return JSON.parse(raw);
  } catch {}
  return {};
}

export function saveRunningPresets(running: Record<string, string>): void {
  localStorage.setItem(scopedPresetKey(LS_RUNNING_PRESETS), JSON.stringify(running));
  emitPresetStoreEvent();
}

export function runningPresetIdForModel(modelName: string): string | undefined {
  return loadRunningPresets()[modelName];
}

/** Record (snapshot) the preset a model is now running with. */
export function setRunningPreset(modelName: string, presetId: string): void {
  if (!modelName) return;
  const next = { ...loadRunningPresets() };
  if (next[modelName] === presetId) return;
  next[modelName] = presetId;
  saveRunningPresets(next);
}

/** Forget a model's running preset (call when it unloads). */
export function clearRunningPreset(modelName: string): void {
  if (!modelName) return;
  const next = { ...loadRunningPresets() };
  if (!(modelName in next)) return;
  delete next[modelName];
  saveRunningPresets(next);
}

/**
 * How an already-loaded model must absorb a preset change.
 *  - 'none'   : presets are equivalent in every field that affects a load.
 *  - 'live'   : only request-time fields differ (system prompt, sampling,
 *               tools toggle) — apply live, NO reload.
 *  - 'reload' : a field that the runtime binds at init differs (recipe_options
 *               such as ctx_size / backend / device / args / steps, or the
 *               engine hint) — the model must be reinitialized (reloaded).
 */
export type PresetChangeKind = 'none' | 'live' | 'reload';

/** Fields that the backend/runtime binds at load time → require a reload. */
const RELOAD_FIELDS: Array<keyof Preset> = ['recipe_options', 'engine_hint'];
/** Fields applied per request at generation time → can be updated live. */
const LIVE_FIELDS: Array<keyof Preset> = ['sampling', 'system_prompt_id', 'system_prompts', 'tools_enabled'];

function stableEqual(a: unknown, b: unknown): boolean {
  return JSON.stringify(a ?? null) === JSON.stringify(b ?? null);
}

export function classifyPresetChange(
  running: Preset | null | undefined,
  next: Preset | null | undefined,
): PresetChangeKind {
  if (!running || !next) return 'none';
  // NOTE: do NOT early-return on running.id === next.id. Editing a preset in
  // place (same id, but changed temperature / system_prompt / ctx_size) must
  // still classify as 'live' or 'reload'. Only the actual field comparisons
  // below decide; 'none' is returned solely when nothing relevant differs.
  for (const field of RELOAD_FIELDS) {
    if (!stableEqual(running[field], next[field])) return 'reload';
  }
  for (const field of LIVE_FIELDS) {
    if (!stableEqual(running[field], next[field])) return 'live';
  }
  // No load-affecting or request-time field differs (regardless of id).
  return 'none';
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
      return pickRecipeOptions(options, ['voice', 'speed', 'merge_args']);
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

export function recipeOptionsForModel(
  modelName: string,
  model?: ModelInfo | null,
  explicitOptions?: RecipeOptions | null,
  systemInfo?: Record<string, unknown> | null,
): RecipeOptions | undefined {
  const capability = model ? capabilityFromModelInfo(model) : undefined;

  // Shared Preset = user intent. It may still carry legacy/custom recipe_options,
  // but concrete per-model values now belong to Model Tuning and win below.
  const preset = activePresetForModel(modelName);
  const presetOptionsRaw = preset.recipe_options || {};
  const presetOptions = model
    ? recipeOptionsForCapability(presetOptionsRaw, capability!)
    : presetOptionsRaw;

  // Model Tuning = per-model user override layer. The UI still displays
  // built-in/GGUF-derived model defaults through effectiveModelTuningForModel(),
  // but load options should only send explicit overrides. Otherwise a UI fallback
  // such as ctx_size=4096 would accidentally override preset/backend defaults.
  const modelTuningOptionsRaw = loadModelTuning(modelName)?.recipe_options || {};
  const modelTuningOptions = model
    ? recipeOptionsForCapability(modelTuningOptionsRaw, capability!)
    : modelTuningOptionsRaw;

  // Backend preset = GLOBAL backend/runtime defaults (#2432). It only applies
  // when the CONCRETE backend this load resolves to matches its exact
  // `recipe:backend` binding. The backend preset is a base layer; preset intent
  // and model tuning are more specific.
  const backendPreset = activePresetForModelBackend(model, {
    explicitOptions,
    modelPresetOptions: { ...presetOptions, ...modelTuningOptions },
    systemInfo,
  });
  const backendOptions = backendPreset && model
    ? recipeOptionsForCapability(backendPreset.recipe_options || {}, capability!)
    : (backendPreset ? (backendPreset.recipe_options || {}) : {});

  const merged: RecipeOptions = { ...backendOptions, ...presetOptions, ...modelTuningOptions };
  return Object.keys(merged || {}).length > 0 ? merged : undefined;
}

export function samplingForModel(modelName: string, model?: ModelInfo | null): SamplingParams {
  const presetSampling = activePresetForModel(modelName).sampling || {};
  const modelSampling = loadModelTuning(modelName)?.sampling || {};
  const merged = { ...presetSampling, ...modelSampling };
  return Object.keys(merged).length > 0 ? merged : {};
}

export type PresetIconName =
  | 'citrus'
  | 'scale'
  | 'gem'
  | 'gauge'
  | 'timer'
  | 'scan-eye'
  | 'pen-line'
  | 'library'
  | 'code'
  | 'search-check'
  | 'hard-drive'
  | 'sliders-horizontal';

export function getPresetIcon(id: string, nameRaw: string): PresetIconName {
  const normalizedId = String(id || '').toLowerCase();
  const name = String(nameRaw || '').toLowerCase();

  if (normalizedId === DEFAULT_PRESET.id || name === 'default') return 'citrus';
  // Chat
  if (name.includes('balanced')) return 'scale';
  if (name.includes('thorough')) return 'search-check';
  if (name.includes('quick')) return 'timer';
  if (name.includes('creative')) return 'pen-line';
  if (name.includes('long')) return 'library';
  if (name.includes('code')) return 'code';
  if (name.includes('memory')) return 'hard-drive';
  if (name.includes('transcription') || name.includes('transcribe')) return 'scan-eye';
  if (name.includes('voice')) return 'hard-drive';
  // Image
  if (name.includes('quality')) return 'gem';
  if (name.includes('preview')) return 'scan-eye';
  if (name.includes('turbo')) return 'gauge';

  return 'sliders-horizontal';
}

export function presetIconName(preset: Pick<Preset, 'id' | 'name' | 'starter'> | null | undefined): PresetIconName {
  if (!preset) return 'sliders-horizontal';
  return getPresetIcon(String(preset.id || ''), String(preset.name || ''));
}

// Backwards-compatible string API for older call sites. New UI code should render
// the returned icon name through <PresetIcon /> instead of showing emoji glyphs.
export function presetIcon(preset: Pick<Preset, 'id' | 'name' | 'starter'> | null | undefined): PresetIconName {
  return presetIconName(preset);
}
