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
  description: 'Use current models defaults and automatic backend selection.',
  applies_to: ['all'],
  recipe_options: {},
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
  { id: 's-thorough', name: 'Thorough', description: 'Careful answers for analysis, planning, debugging, and decisions.', applies_to: ['chat'], recipe_options: { ctx_size: 32768 }, sampling: { temperature: 0.40, top_p: 0.95, top_k: 40, repeat_penalty: 1.10 }, engine_hint: 'llamacpp', starter: true },
  { id: 's-quick-chat', name: 'Quick Chat', description: 'Small context, tight sampling. Snappy responses for quick interactions.', applies_to: ['chat'], recipe_options: { ctx_size: 4048 }, sampling: { temperature: 0.60, top_p: 0.80, top_k: 40, repeat_penalty: 1.05 }, engine_hint: 'llamacpp', starter: true },
  { id: 's-creative', name: 'Creative', description: 'Higher temperature for brainstorming, dialog, and divergent thinking.', applies_to: ['chat'], recipe_options: { ctx_size: 32768 }, sampling: { temperature: 0.95, top_p: 0.95, top_k: 60, repeat_penalty: 1.00 }, engine_hint: 'llamacpp', starter: true },
  { id: 's-long-context', name: 'Long Context', description: 'For documents, codebases, and long conversation threads.', applies_to: ['chat'], recipe_options: { ctx_size: 262144 }, sampling: { temperature: 0.70, top_p: 0.90, top_k: 40, repeat_penalty: 1.05 }, engine_hint: 'llamacpp', starter: true },
  { id: 's-code', name: 'Code', description: 'Low temperature, tight sampling for code generation and refactoring.', applies_to: ['chat'], recipe_options: { ctx_size: 131072 }, sampling: { temperature: 0.20, top_p: 0.95, top_k: 40, repeat_penalty: 1.05 }, engine_hint: 'llamacpp', starter: true },
  { id: 's-quality', name: 'Quality', description: 'More steps and tighter guidance for crisp, deliberate image generation.', applies_to: ['image'], recipe_options: { steps: 20, cfg_scale: 8.0 }, sampling: {}, engine_hint: 'sd-cpp', starter: true },
  { id: 's-preview', name: 'Preview', description: 'Fewer steps, looser guidance — fast drafts and iteration.', applies_to: ['image'], recipe_options: { steps: 8, cfg_scale: 6.0 }, sampling: {}, engine_hint: 'sd-cpp', starter: true },
  { id: 's-turbo', name: 'Turbo', description: 'Fastest image drafts for rapid iteration.', applies_to: ['image'], recipe_options: { steps: 4, cfg_scale: 1.0 }, sampling: {}, engine_hint: 'sd-cpp', starter: true },
];


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
  const lines: string[] = [];

  if (showChat) {
    lines.push(`temp ${formatDash(sp.temperature, 2)} · ctx ${formatDash(previewContext(ro.ctx_size, fallbackCtxSize))}`);
  }
  if (showImage && (hasImageValues || !showChat)) {
    lines.push(`${formatDash(ro.steps)} steps · cfg ${formatDash(ro.cfg_scale, 1)}`);
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
