/**
 * ModelDetailPanel — right-side detail view for the selected model.
 * Contains: header (title, metadata, primary actions) + tablist (README / Presets / Model Tuning / Files).
 *
 * Part of the master-detail layout introduced in #2355 Slice 1.
 */
import React, { useState, useEffect, useMemo, useRef, useCallback } from 'react';
import MarkdownIt from 'markdown-it';
import DOMPurify from 'dompurify';
import type { ModelInfo, LoadedModel, ModelFileInfo, HFModelResult, PullVariantsResult } from '../api';
import api from '../api';
import { capabilityFromModelInfo, capabilityLabel } from '../modelCapabilities';
import {
  DEFAULT_PRESET, PRESET_STORE_EVENT, Preset, PresetChangeKind,
  allStoredPresets, isCompatible, loadApplied, saveApplied,
  effectivePresetParamPreviewLines, activePresetForModel,
  runningPresetIdForModel, setRunningPreset, clearRunningPreset,
  classifyPresetChange,
  effectiveModelTuningForModel, modelBaseTuningForModel, resolvedModelTuningForPreset, loadModelTuning,
  saveModelTuning, resetModelTuning, sanitizeRecipeOptions, sanitizeSamplingParams,
  TEMPERATURE_HINTS, EDITABLE_CONTEXT_HINTS, TEMPERATURE_HINT_LABELS, CONTEXT_HINT_LABELS,
  type RecipeOptions, type SamplingParams, type TuningValueSource, type TemperatureHint, type ContextHint, type EditableContextHint,
} from '../presetStore';
import { Icon, CapabilityIcon, PresetIcon, type IconName } from './Icon';
import { getCollectionComponents, isCollectionModel } from '../features/collections/collectionModels';

/* ── Helpers (local copies to keep component self-contained) ──── */

function mdName(m: ModelInfo | null | undefined): string {
  if (!m) return '';
  return String((m as any).model_name ?? m.name ?? m.id ?? '').trim();
}

function fmtSize(gb: number): string {
  if (!Number.isFinite(gb) || gb <= 0) return '';
  if (gb >= 1) return `${gb.toFixed(1)} GB`;
  if (gb >= 0.01) return `${(gb * 1000).toFixed(0)} MB`;
  return '< 1 MB';
}

function fmtDownloads(n: number): string {
  if (n >= 1_000_000) return `${(n / 1_000_000).toFixed(1)}M`;
  if (n >= 1_000) return `${(n / 1_000).toFixed(1)}K`;
  return String(n);
}

function recipeDisplayLabel(recipe: string): string {
  const n = String(recipe || '').toLowerCase();
  switch (n) {
    case 'llamacpp': return 'llama.cpp';
    case 'vllm': return 'vLLM';
    case 'flm': return 'FastFlowLM';
    case 'ryzenai-llm': return 'RyzenAI';
    case 'sd-cpp': return 'Stable Diffusion';
    case 'whispercpp': return 'Whisper';
    case 'moonshine': return 'Moonshine';
    case 'kokoro': return 'Kokoro TTS';
    case 'acestep': return 'ACE-Step';
    case 'thinksound': return 'ThinkSound';
    case 'openmoss': return 'OpenMOSS TTS';
    case 'trellis': return 'TRELLIS.2';
    case 'collection.omni': return 'Omni Collection';
    case 'collection': return 'Collection';
    default: return recipe || 'Unknown';
  }
}

function activeRecipeForModel(model: ModelInfo | null | undefined): string {
  if (!model) return '';
  const direct = String((model as any).recipe || '').trim().toLowerCase();
  if (direct) return direct;
  const recipes = Array.isArray((model as any).recipes) ? ((model as any).recipes as Record<string, unknown>[]) : [];
  const first = recipes[0];
  return String(first?.recipe || first?.name || first?.id || '').trim().toLowerCase();
}

function recipesForDisplay(model: ModelInfo | null | undefined): string[] {
  if (!model) return [];
  const out: string[] = [];
  const active = activeRecipeForModel(model);
  if (active) out.push(active);
  const recipes = Array.isArray((model as any).recipes) ? ((model as any).recipes as Record<string, unknown>[]) : [];
  for (const recipe of recipes) {
    const name = String(recipe.recipe || recipe.name || recipe.id || '').trim().toLowerCase();
    if (name && !out.includes(name)) out.push(name);
  }
  return out;
}

function tuningValue(value: unknown): string {
  if (value === undefined || value === null || value === '') return 'auto';
  if (typeof value === 'boolean') return value ? 'true' : 'false';
  if (typeof value === 'number') return Number.isFinite(value) ? String(value) : 'auto';
  return String(value);
}

function optionalDisplayValue(value: unknown): string {
  const text = String(value ?? '').trim();
  return text && text.toLowerCase() !== 'unknown' ? text : '';
}

function fieldValue(value: unknown): string {
  if (value === undefined || value === null) return '';
  return String(value);
}

function parseNumberOrUndefined(value: string): number | undefined {
  if (!value.trim()) return undefined;
  const n = Number(value);
  return Number.isFinite(n) ? n : undefined;
}

const TUNING_FIELD_LABELS: Record<keyof RecipeOptions, string> = {
  ctx_size: 'Context size',
  llamacpp_backend: 'Backend',
  llamacpp_device: 'Device',
  llamacpp_args: 'Backend args',
  steps: 'Steps',
  cfg_scale: 'CFG scale',
  width: 'Width',
  height: 'Height',
  sampling_method: 'Sampling method',
  flow_shift: 'Flow shift',
  sdcpp_args: 'Backend args',
  whispercpp_backend: 'Backend',
  whispercpp_args: 'Backend args',
  moonshine_backend: 'Backend',
  moonshine_args: 'Backend args',
  acestep_backend: 'Backend',
  thinksound_backend: 'Backend',
  openmoss_backend: 'Backend',
  trellis_backend: 'Backend',
  vllm_backend: 'Backend',
  vllm_args: 'Backend args',
  flm_args: 'Backend args',
  voice: 'Voice',
  speed: 'Speed',
  merge_args: 'Backend args behavior',
  mmproj_enabled: 'Vision projector',
};

const TUNING_FIELD_HINTS: Partial<Record<keyof RecipeOptions, string>> = {
  ctx_size: 'Runtime context window for this exact model.',
  llamacpp_backend: 'Backend for this model recipe. Switching back restores the last draft args for that backend in this browser session.',
  vllm_backend: 'Backend for this model recipe. Switching back restores the last draft args for that backend in this browser session.',
  whispercpp_backend: 'Backend for this model recipe. Switching back restores the last draft args for that backend in this browser session.',
  moonshine_backend: 'Backend for this model recipe. Switching back restores the last draft args for that backend in this browser session.',
  acestep_backend: 'ACE-Step accelerator backend for music generation.',
  thinksound_backend: 'ThinkSound accelerator backend for sound-effect generation.',
  openmoss_backend: 'OpenMOSS accelerator backend for speech generation.',
  trellis_backend: 'TRELLIS accelerator backend for 3D reconstruction.',
  llamacpp_device: 'Optional device selector for the selected backend.',
  llamacpp_args: 'Raw backend args for this model and selected backend only.',
  sdcpp_args: 'Raw backend args for this image model only.',
  whispercpp_args: 'Raw backend args for this transcription model only.',
  moonshine_args: 'Raw backend args for this transcription model only.',
  vllm_args: 'Raw backend args for this model only.',
  flm_args: 'Raw backend args for this model only.',
  merge_args: 'Choose whether backend defaults, model args, or both should be used for this model.',
  mmproj_enabled: "Off frees the projector's memory for context when image input is not needed.",
};

const NUMERIC_TUNING_KEYS = new Set<keyof RecipeOptions>(['steps', 'cfg_scale', 'width', 'height', 'flow_shift', 'speed']);
const BOOLEAN_TUNING_KEYS = new Set<keyof RecipeOptions>(['merge_args', 'mmproj_enabled']);
const BACKEND_TUNING_KEYS = new Set<keyof RecipeOptions>(['llamacpp_backend', 'vllm_backend', 'whispercpp_backend', 'moonshine_backend', 'acestep_backend', 'thinksound_backend', 'openmoss_backend', 'trellis_backend']);
const DEVICE_TUNING_KEYS = new Set<keyof RecipeOptions>(['llamacpp_device']);
const ARGS_TUNING_KEYS = new Set<keyof RecipeOptions>(['llamacpp_args', 'sdcpp_args', 'whispercpp_args', 'moonshine_args', 'vllm_args', 'flm_args']);
const BACKEND_ARGS_KEY: Partial<Record<keyof RecipeOptions, keyof RecipeOptions>> = {
  llamacpp_backend: 'llamacpp_args',
  vllm_backend: 'vllm_args',
  whispercpp_backend: 'whispercpp_args',
  moonshine_backend: 'moonshine_args',
};

const LLAMACPP_RECIPE_KEYS: Array<keyof RecipeOptions> = ['llamacpp_backend', 'llamacpp_device', 'llamacpp_args', 'mmproj_enabled', 'merge_args'];
const VLLM_RECIPE_KEYS: Array<keyof RecipeOptions> = ['vllm_backend', 'vllm_args', 'merge_args'];
const FLM_RECIPE_KEYS: Array<keyof RecipeOptions> = ['flm_args', 'merge_args'];
const RYZENAI_RECIPE_KEYS: Array<keyof RecipeOptions> = ['merge_args'];
const IMAGE_RECIPE_KEYS: Array<keyof RecipeOptions> = ['steps', 'cfg_scale', 'width', 'height', 'sampling_method', 'flow_shift', 'sdcpp_args', 'merge_args'];
const WHISPER_RECIPE_KEYS: Array<keyof RecipeOptions> = ['whispercpp_backend', 'whispercpp_args', 'merge_args'];
const MOONSHINE_RECIPE_KEYS: Array<keyof RecipeOptions> = ['moonshine_backend', 'moonshine_args', 'merge_args'];
const TTS_RECIPE_KEYS: Array<keyof RecipeOptions> = ['voice', 'speed', 'merge_args'];
const ACESTEP_RECIPE_KEYS: Array<keyof RecipeOptions> = ['acestep_backend'];
const THINKSOUND_RECIPE_KEYS: Array<keyof RecipeOptions> = ['thinksound_backend'];
const OPENMOSS_RECIPE_KEYS: Array<keyof RecipeOptions> = ['openmoss_backend', 'voice', 'speed'];
const TRELLIS_RECIPE_KEYS: Array<keyof RecipeOptions> = ['trellis_backend'];


const TEMPERATURE_INTENT_ICONS: Record<TemperatureHint, IconName> = {
  precise: 'crosshair',
  balanced: 'scale',
  exploratory: 'compass',
  creative: 'lightbulb',
};

const CONTEXT_INTENT_ICONS: Record<ContextHint, IconName> = {
  small: 'minimize-2',
  medium: 'panel-top',
  large: 'expand',
  max: 'maximize-2',
};

function formatContextSize(value: number): string {
  if (!Number.isFinite(value) || value <= 0) return 'auto';
  if (value >= 1024 && value % 1024 === 0) return `${Math.round(value / 1024)}K`;
  return value.toLocaleString();
}

function recipeKeysForRecipe(recipe: string): Array<keyof RecipeOptions> | null {
  switch (recipe) {
    case 'llamacpp': return LLAMACPP_RECIPE_KEYS;
    case 'vllm': return VLLM_RECIPE_KEYS;
    case 'flm': return FLM_RECIPE_KEYS;
    case 'ryzenai-llm': return RYZENAI_RECIPE_KEYS;
    case 'sd-cpp': return IMAGE_RECIPE_KEYS;
    case 'whispercpp': return WHISPER_RECIPE_KEYS;
    case 'moonshine': return MOONSHINE_RECIPE_KEYS;
    case 'kokoro': return TTS_RECIPE_KEYS;
    case 'acestep': return ACESTEP_RECIPE_KEYS;
    case 'thinksound': return THINKSOUND_RECIPE_KEYS;
    case 'openmoss': return OPENMOSS_RECIPE_KEYS;
    case 'trellis': return TRELLIS_RECIPE_KEYS;
    default: return null;
  }
}

function tuningKeysForModel(model: ModelInfo): Array<keyof RecipeOptions> {
  const cap = capabilityFromModelInfo(model);
  const recipes = recipesForDisplay(model);
  const activeRecipe = activeRecipeForModel(model);
  const set = new Set<keyof RecipeOptions>();
  const add = (keys: Array<keyof RecipeOptions>) => keys.forEach(key => set.add(key));

  const activeKeys = recipeKeysForRecipe(activeRecipe);
  if (activeKeys) add(activeKeys);
  else if (cap === 'chat' || cap === 'omni' || cap === 'unknown') add(LLAMACPP_RECIPE_KEYS);
  else if (cap === 'image') add(IMAGE_RECIPE_KEYS);
  else if (cap === 'audio') add(recipes.includes('moonshine') ? MOONSHINE_RECIPE_KEYS : WHISPER_RECIPE_KEYS);
  else if (cap === 'audio-generation') add(recipes.includes('acestep') ? ACESTEP_RECIPE_KEYS : THINKSOUND_RECIPE_KEYS);
  else if (cap === 'tts') add(recipes.includes('openmoss') ? OPENMOSS_RECIPE_KEYS : TTS_RECIPE_KEYS);
  else if (cap === 'model3d') add(TRELLIS_RECIPE_KEYS);

  const base = modelBaseTuningForModel(model).recipe_options;
  Object.keys(base).forEach(key => {
    if (key !== 'ctx_size') set.add(key as keyof RecipeOptions);
  });
  set.delete('ctx_size');
  return [...set];
}

type SystemInfoLike = Record<string, unknown> | null | undefined;

function systemRecipes(info: SystemInfoLike): Record<string, any> | null {
  const recipes = (info as any)?.recipes;
  return recipes && typeof recipes === 'object' && !Array.isArray(recipes) ? recipes as Record<string, any> : null;
}

function backendMapForRecipe(info: SystemInfoLike, recipe: string): Record<string, any> | null {
  const recipeInfo = systemRecipes(info)?.[recipe];
  const backends = recipeInfo?.backends;
  return backends && typeof backends === 'object' && !Array.isArray(backends) ? backends as Record<string, any> : null;
}

function backendState(info: unknown): string {
  return String((info as any)?.state || '').trim().toLowerCase();
}

function backendIsSelectable(recipe: string, backend: string, info: unknown): boolean {
  if (!backend || backendState(info) === 'unsupported') return false;
  // llama.cpp has no NPU backend; FLM/RyzenAI own the NPU paths.
  if (recipe === 'llamacpp' && backend.toLowerCase().includes('npu')) return false;
  return true;
}

function activeRecipeForBackendKey(key: keyof RecipeOptions, model: ModelInfo): string {
  switch (key) {
    case 'llamacpp_backend': return 'llamacpp';
    case 'vllm_backend': return 'vllm';
    case 'whispercpp_backend': return 'whispercpp';
    case 'moonshine_backend': return 'moonshine';
    case 'acestep_backend': return 'acestep';
    case 'thinksound_backend': return 'thinksound';
    case 'openmoss_backend': return 'openmoss';
    case 'trellis_backend': return 'trellis';
    default: return activeRecipeForModel(model);
  }
}

function fallbackBackendsForRecipe(recipe: string): string[] {
  switch (recipe) {
    case 'vllm': return ['cpu', 'cuda', 'rocm'];
    case 'whispercpp': return ['cpu', 'cuda', 'vulkan', 'opencl'];
    case 'moonshine': return ['cpu', 'cuda'];
    case 'sd-cpp': return ['cpu', 'cuda', 'vulkan', 'rocm'];
    case 'kokoro': return ['cpu'];
    case 'acestep':
    case 'thinksound':
    case 'openmoss':
    case 'trellis': return ['cuda', 'rocm', 'vulkan'];
    case 'llamacpp':
    default:
      // Keep fallback conservative: no Metal/NPU unless the server explicitly reports them as selectable.
      return ['cpu', 'cuda', 'vulkan', 'opencl', 'rocm'];
  }
}

function recipeDefaultBackend(info: SystemInfoLike, recipe: string): string {
  return optionalDisplayValue(systemRecipes(info)?.[recipe]?.default_backend);
}

function backendOptionsForKey(key: keyof RecipeOptions, current: string | undefined, model: ModelInfo, info: SystemInfoLike): string[] {
  const recipe = activeRecipeForBackendKey(key, model);
  const fromServer = Object.entries(backendMapForRecipe(info, recipe) || {})
    .filter(([backend, backendInfo]) => backendIsSelectable(recipe, backend, backendInfo) && backendMatchesDetectedHardware(backend, info))
    .map(([backend]) => backend);
  const rawBase = fromServer.length ? fromServer : fallbackBackendsForRecipe(recipe);
  const base = rawBase.filter(backend => backendMatchesDetectedHardware(backend, info));
  const safeBase = Array.from(new Set(['auto', ...(base.length ? base : ['cpu'])]));
  const normalizedCurrent = optionalDisplayValue(current);
  const options = normalizedCurrent && !safeBase.includes(normalizedCurrent) ? [normalizedCurrent, ...safeBase] : safeBase;
  return Array.from(new Set(options.filter(Boolean)));
}

function activeBackendValue(key: keyof RecipeOptions, baseValue: unknown, model: ModelInfo, info: SystemInfoLike): string {
  const fromModel = optionalDisplayValue(baseValue);
  if (fromModel) return fromModel;
  const recipe = activeRecipeForBackendKey(key, model);
  return recipeDefaultBackend(info, recipe) || 'auto';
}

function availableDeviceCounts(info: SystemInfoLike): { nvidia: number; amd: number; metal: boolean; npu: boolean; cpu: boolean } {
  const devices = (info as any)?.devices || {};
  const asList = (value: unknown): any[] => Array.isArray(value) ? value : (value ? [value] : []);
  const available = (device: any) => device?.available !== false;
  const nvidia = asList(devices.nvidia_gpu).filter(available).length;
  const amd = [...asList(devices.amd_gpu), ...asList(devices.amd_dgpu), ...asList(devices.amd_igpu)].filter(available).length;
  return {
    nvidia,
    amd,
    metal: !!devices.metal && available(devices.metal),
    npu: !!(devices.amd_npu || devices.npu) && available(devices.amd_npu || devices.npu),
    cpu: devices.cpu?.available !== false,
  };
}
function backendMatchesDetectedHardware(backend: string, info: SystemInfoLike): boolean {
  if (!(info as any)?.devices) return true;
  const b = backend.toLowerCase();
  const devices = availableDeviceCounts(info);
  if (b.includes('metal')) return devices.metal;
  if (b.includes('npu') || b.includes('ryzenai')) return devices.npu;
  if (b.includes('cuda') || b.includes('nvidia')) return devices.nvidia > 0;
  if (b.includes('rocm')) return devices.amd > 0;
  return true;
}


function indexed(prefix: string, count: number, fallbackCount = 1): string[] {
  const n = Math.max(count, fallbackCount);
  return Array.from({ length: n }, (_, i) => `${prefix}${i}`);
}

function deviceOptionsForKey(key: keyof RecipeOptions, current: string | undefined, selectedBackend: string, model: ModelInfo, info: SystemInfoLike): string[] {
  const recipe = activeRecipeForModel(model);
  const backend = selectedBackend.toLowerCase();
  const devices = availableDeviceCounts(info);
  let base: string[] = [];

  if (!backend || backend === 'auto') {
    base = ['cpu'];
    if (devices.nvidia > 0) base.push(...indexed('cuda', devices.nvidia, 0));
    if (devices.amd > 0) base.push(...indexed('vulkan', devices.amd, 0));
    if (devices.metal) base.push('metal');
    if (devices.npu && recipe !== 'llamacpp') base.push('npu0');
  } else if (backend.includes('cpu')) base = ['cpu'];
  else if (backend.includes('cuda')) base = indexed('cuda', devices.nvidia, 1);
  else if (backend.includes('vulkan')) base = indexed('vulkan', devices.amd + devices.nvidia, 1);
  else if (backend.includes('opencl')) base = indexed('opencl', devices.amd + devices.nvidia, 1);
  else if (backend.includes('rocm')) base = indexed('rocm', devices.amd, 1);
  else if (backend.includes('metal') && devices.metal) base = ['metal'];
  else if (backend.includes('npu') && devices.npu && recipe !== 'llamacpp') base = ['npu0'];

  const normalizedCurrent = optionalDisplayValue(current);
  const options = normalizedCurrent && !base.includes(normalizedCurrent) ? [normalizedCurrent, ...base] : base;
  return Array.from(new Set(options.filter(Boolean)));
}

function numericSliderSpec(key: keyof RecipeOptions | keyof SamplingParams): { min: number; max: number; step: number; fallback: number; digits?: number } | null {
  switch (key) {
    case 'temperature': return { min: 0, max: 2, step: 0.05, fallback: 0.7, digits: 2 };
    case 'top_p': return { min: 0, max: 1, step: 0.01, fallback: 0.9, digits: 2 };
    case 'top_k': return { min: 1, max: 200, step: 1, fallback: 40 };
    case 'repeat_penalty': return { min: 0.9, max: 1.5, step: 0.01, fallback: 1.05, digits: 2 };
    case 'steps': return { min: 1, max: 100, step: 1, fallback: 20 };
    case 'cfg_scale': return { min: 0, max: 30, step: 0.5, fallback: 7.5, digits: 1 };
    case 'flow_shift': return { min: 0, max: 20, step: 0.1, fallback: 1, digits: 1 };
    case 'speed': return { min: 0.5, max: 2, step: 0.05, fallback: 1, digits: 2 };
    default: return null;
  }
}

function sliderDisplay(value: number, digits?: number): string {
  return digits === undefined ? String(Math.round(value)) : value.toFixed(digits);
}

function samplingAllowedForModel(model: ModelInfo): boolean {
  const cap = capabilityFromModelInfo(model);
  return cap === 'chat' || cap === 'omni' || cap === 'unknown';
}

/** Regex: only attempt HF README fetch when the derived value looks like `owner/repo`. */
const HF_REPO_RE = /^[\w.-]+\/[\w.-]+$/;

/**
 * Derive the best-effort HF repo from model.checkpoint or model.checkpoints.main
 * (falling back to the first available checkpoint value).
 * Strips the variant/file suffix after `:`.
 * Returns null if no valid `owner/repo` can be derived.
 */
function deriveHFRepo(
  checkpoint: string | null | undefined,
  checkpoints: Record<string, string> | null | undefined,
): string | null {
  const candidates: (string | undefined)[] = [
    checkpoint ?? undefined,
    checkpoints?.main,
    ...(checkpoints ? Object.values(checkpoints) : []),
  ];
  for (const c of candidates) {
    if (!c) continue;
    const repo = c.split(':')[0].trim();
    if (HF_REPO_RE.test(repo)) return repo;
  }
  return null;
}

/* ── Shared markdown-it instance for README rendering ─────────── */

// html:true is safe here because the rendered output is passed through the
// strict DOMPurify allowlist (README_PURIFY_CONFIG) below before injection.
// HF model READMEs commonly embed raw HTML (<div align="center">, <img>,
// tables, badges); with html:false markdown-it escapes those to literal text.
const readmeMd = new MarkdownIt({ html: true, linkify: true, typographer: true });

const README_PURIFY_CONFIG: DOMPurify.Config = {
  ALLOWED_TAGS: [
    'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'p', 'br', 'hr',
    'ul', 'ol', 'li', 'dl', 'dt', 'dd',
    'table', 'thead', 'tbody', 'tfoot', 'tr', 'th', 'td', 'caption', 'colgroup', 'col',
    'div', 'span', 'a', 'img', 'picture', 'source',
    'strong', 'em', 'b', 'i', 'u', 's', 'del', 'mark', 'code', 'pre',
    'sup', 'sub', 'kbd', 'samp', 'var',
    'blockquote', 'details', 'summary', 'figure', 'figcaption', 'abbr',
  ],
  ALLOWED_ATTR: ['class', 'href', 'target', 'rel', 'src', 'srcset', 'alt', 'title', 'width', 'height', 'align', 'colspan', 'rowspan'],
};

/**
 * Strip a leading YAML frontmatter block from an HF README.
 * HF READMEs begin with metadata delimited by `---` ... `---`. With html:true
 * this would otherwise render as a stray <hr> plus dumped key/value text.
 * Defensive: only strips a well-formed leading block; never throws.
 */
function stripFrontmatter(source: string): string {
  if (typeof source !== 'string') return '';
  const leading = source.replace(/^\s+/, '');
  if (!leading.startsWith('---\n') && !leading.startsWith('---\r\n')) return source;
  const lines = leading.split('\n');
  // lines[0] is the opening '---'; find the next line that is exactly '---'.
  for (let i = 1; i < lines.length; i++) {
    if (lines[i].replace(/\r$/, '').trim() === '---') {
      return lines.slice(i + 1).join('\n');
    }
  }
  // No closing delimiter found: not a well-formed block, leave untouched.
  return source;
}

const readmeCache = new Map<string, string>();

/* ── README tab ──────────────────────────────────────────────── */

const ModelReadmeTab: React.FC<{ model: ModelInfo | null | undefined; isActive: boolean }> = ({ model, isActive }) => {
  const checkpoint = model ? String((model as any).checkpoint || '') : '';
  const checkpoints = model ? ((model as any).checkpoints as Record<string, string> | null ?? null) : null;
  const hfRepo = deriveHFRepo(checkpoint || null, checkpoints);
  const readmeUrl = hfRepo ? `https://huggingface.co/${hfRepo}/raw/main/README.md` : null;

  const [readme, setReadme] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);

  useEffect(() => {
    if (!isActive) return;
    if (!readmeUrl) { setReadme(''); return; }

    const cached = readmeCache.get(readmeUrl);
    if (cached !== undefined) { setReadme(cached); return; }

    let cancelled = false;
    setLoading(true);
    fetch(readmeUrl)
      .then(r => r.ok ? r.text() : null)
      .then(text => {
        if (cancelled) return;
        const content = text || '';
        readmeCache.set(readmeUrl, content);
        setReadme(content);
      })
      .catch(() => {
        if (!cancelled) { readmeCache.set(readmeUrl, ''); setReadme(''); }
      })
      .finally(() => { if (!cancelled) setLoading(false); });

    return () => { cancelled = true; };
  }, [readmeUrl, isActive]);

  if (loading) {
    return (
      <div className="detail-tab-content detail-readme detail-readme--loading" aria-live="polite" aria-busy="true">
        <span>Loading README…</span>
      </div>
    );
  }

  if (!readme) {
    return (
      <div className="detail-tab-content detail-readme detail-readme--empty">
        <Icon name="book-open" size={32} aria-hidden="true" />
        <p>README unavailable for this model.</p>
      </div>
    );
  }

  const html = DOMPurify.sanitize(readmeMd.render(stripFrontmatter(readme)), README_PURIFY_CONFIG);

  return (
    <div
      className="detail-tab-content detail-readme"
      dangerouslySetInnerHTML={{ __html: html }}
    />
  );
};

/* ── HF README tab ────────────────────────────────────────────── */

const HfReadmeTab: React.FC<{ hfId: string; isActive: boolean }> = ({ hfId, isActive }) => {
  const readmeUrl = `https://huggingface.co/${hfId}/raw/main/README.md`;
  const [readme, setReadme] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);

  useEffect(() => {
    if (!isActive) return;
    const cached = readmeCache.get(readmeUrl);
    if (cached !== undefined) { setReadme(cached); return; }
    let cancelled = false;
    setLoading(true);
    fetch(readmeUrl)
      .then(r => r.ok ? r.text() : null)
      .then(text => {
        if (cancelled) return;
        const content = text || '';
        readmeCache.set(readmeUrl, content);
        setReadme(content);
      })
      .catch(() => {
        if (!cancelled) { readmeCache.set(readmeUrl, ''); setReadme(''); }
      })
      .finally(() => { if (!cancelled) setLoading(false); });
    return () => { cancelled = true; };
  }, [readmeUrl, isActive]);

  if (loading) {
    return (
      <div className="detail-tab-content detail-readme detail-readme--loading" aria-live="polite" aria-busy="true">
        <span>Loading README…</span>
      </div>
    );
  }
  if (!readme) {
    return (
      <div className="detail-tab-content detail-readme detail-readme--empty">
        <Icon name="book-open" size={32} aria-hidden="true" />
        <p>README unavailable for this model.</p>
      </div>
    );
  }
  const html = DOMPurify.sanitize(readmeMd.render(stripFrontmatter(readme)), README_PURIFY_CONFIG);
  return (
    <div className="detail-tab-content detail-readme" dangerouslySetInnerHTML={{ __html: html }} />
  );
};

/* ── HF overview tab ──────────────────────────────────────────── */

const HfOverviewTab: React.FC<{
  hfModel: HFModelResult;
  hfVariants?: PullVariantsResult;
  onHfPull?: (hfId: string, variantName: string, recipe: string) => void;
  isPulling: boolean;
  isActive: boolean;
}> = ({ hfModel, hfVariants, onHfPull, isPulling, isActive }) => {
  if (!isActive) return null;
  return (
    <div className="detail-tab-content hf-detail__overview">
      {hfVariants ? (
        <>
          {hfVariants.suggested_labels.length > 0 && (
            <div className="hf-detail__overview-section">
              <span className="hf-detail__overview-label">Capabilities</span>
              <div className="hf-detail__overview-value">{hfVariants.suggested_labels.join(', ')}</div>
            </div>
          )}
          {hfVariants.variants.length > 0 ? (
            <div className="hf-detail__overview-section">
              <span className="hf-detail__overview-label">Variants — pick one to download</span>
              <div className="hf-detail__gguf-list">
                {hfVariants.variants.map(v => (
                  <button
                    key={v.name}
                    className="hf-detail__gguf-btn"
                    aria-label={`Download ${v.name} from ${hfModel.id}`}
                    disabled={isPulling}
                    onClick={() => onHfPull?.(hfModel.id, v.name, hfVariants.recipe)}
                  >
                    <span className="hf-detail__gguf-name">
                      {v.name}{v.sharded ? ' (sharded)' : ''}
                    </span>
                    <span className="hf-detail__gguf-size">{fmtBytes(v.size_bytes)}</span>
                    <span className="hf-detail__gguf-action">Download</span>
                  </button>
                ))}
              </div>
            </div>
          ) : (
            <div className="hf-detail__overview-section">
              <span className="hf-detail__overview-value hf-detail__overview-value--muted">No downloadable variants found for this repository.</span>
            </div>
          )}
        </>
      ) : (
        <div className="hf-detail__overview-section">
          <span className="hf-detail__overview-value hf-detail__overview-value--muted">Loading variant information…</span>
        </div>
      )}
    </div>
  );
};

/* ── HF detail view ───────────────────────────────────────────── */

type HfDetailTab = 'overview' | 'readme';

const HF_DETAIL_TABS: Array<{ id: HfDetailTab; label: string }> = [
  { id: 'overview', label: 'Overview' },
  { id: 'readme', label: 'README' },
];

const HfDetailView: React.FC<{
  hfModel: HFModelResult;
  hfVariants?: PullVariantsResult;
  onFetchHfVariants?: (hfId: string) => void;
  onHfPull?: (hfId: string, variantName: string, recipe: string) => void;
  pullingHf?: Record<string, number>;
  onCancelHfPull?: (hfId: string) => void;
  onBack?: () => void;
  onClose?: () => void;
}> = ({ hfModel, hfVariants, onFetchHfVariants, onHfPull, pullingHf, onCancelHfPull, onBack, onClose }) => {
  const [activeTab, setActiveTab] = useState<HfDetailTab>('overview');
  const tabRefs = useRef<Array<HTMLButtonElement | null>>([]);
  const panelHeadingRef = useRef<HTMLHeadingElement>(null);

  const repoName = hfModel.id;
  const pipelineTag = hfModel.pipeline_tag || '';
  const displayTags = (hfModel.tags || [])
    .filter(t => t !== 'gguf' && t !== 'transformers' && t !== 'pytorch' && t !== 'safetensors')
    .slice(0, 8);
  const isPulling = (pullingHf?.[hfModel.id]) !== undefined;
  const pullPct = pullingHf?.[hfModel.id] ?? 0;

  useEffect(() => {
    setActiveTab('overview');
    panelHeadingRef.current?.focus();
  }, [repoName]);

  useEffect(() => {
    if (!hfVariants && onFetchHfVariants) {
      onFetchHfVariants(hfModel.id);
    }
  }, [hfModel.id, hfVariants, onFetchHfVariants]);

  const handleTabKeyDown = (e: React.KeyboardEvent<HTMLButtonElement>, index: number) => {
    const count = HF_DETAIL_TABS.length;
    let next = -1;
    if (e.key === 'ArrowRight') { e.preventDefault(); next = (index + 1) % count; }
    else if (e.key === 'ArrowLeft') { e.preventDefault(); next = (index - 1 + count) % count; }
    else if (e.key === 'Home') { e.preventDefault(); next = 0; }
    else if (e.key === 'End') { e.preventDefault(); next = count - 1; }
    if (next >= 0) {
      setActiveTab(HF_DETAIL_TABS[next].id);
      tabRefs.current[next]?.focus();
    }
  };

  return (
    <div className="model-detail-panel model-detail-panel--hf" role="region" aria-label={`HuggingFace model: ${repoName}`}>
      {onClose && (
        <button
          type="button"
          className="model-detail-panel__close-btn"
          onClick={onClose}
          aria-label="Close detail panel"
        >
          ×
        </button>
      )}
      {onBack && (
        <button
          type="button"
          className="model-detail-panel__back-btn"
          onClick={onBack}
          aria-label="Back to models list"
        >
          ← Back to models
        </button>
      )}

      <div className="model-detail-panel__head model-detail-panel__head--hf">
        <div className="hf-detail__source-label">
          <Icon name="download" size={11} aria-hidden="true" /> HuggingFace
        </div>
        <h2
          className="model-detail-panel__name"
          ref={panelHeadingRef}
          tabIndex={-1}
          id="detail-panel-heading"
        >
          {repoName}
        </h2>

        <div className="model-detail-panel__meta">
          {pipelineTag && (
            <span className="model-detail-panel__badge model-detail-panel__badge--pipeline">{pipelineTag}</span>
          )}
          {hfVariants?.recipe && (
            <span className="model-detail-panel__badge model-detail-panel__badge--recipe">
              {recipeDisplayLabel(hfVariants.recipe)}
            </span>
          )}
          <span className="model-detail-panel__badge">{fmtDownloads(hfModel.downloads)} downloads</span>
          <span className="model-detail-panel__badge">{fmtDownloads(hfModel.likes)} likes</span>
          {hfModel.createdAt && (
            <span className="model-detail-panel__badge">{new Date(hfModel.createdAt).toLocaleDateString()}</span>
          )}
        </div>

        {displayTags.length > 0 && (
          <div className="hf-detail__tag-row">
            {displayTags.map(t => (
              <span key={t} className="row__label row__label--hf">{t}</span>
            ))}
          </div>
        )}

        <div className="model-detail-panel__actions" aria-label={`Actions for ${repoName}`}>
          {isPulling ? (
            <>
              <div className="row__progress">
                <div className="row__progress-bar">
                  <div className="row__progress-fill" style={{ width: `${pullPct}%` }} />
                </div>
                <span className="row__progress-text">{pullPct.toFixed(0)}%</span>
              </div>
              {onCancelHfPull && (
                <button
                  type="button"
                  className="btn btn--ghost btn--sm"
                  onClick={() => onCancelHfPull(hfModel.id)}
                  aria-label={`Cancel download of ${repoName}`}
                >
                  Cancel
                </button>
              )}
            </>
          ) : null}
          <a
            className="model-detail-panel__hf-link"
            href={`https://huggingface.co/${repoName}`}
            target="_blank"
            rel="noopener noreferrer"
            aria-label={`View ${repoName} on Hugging Face (opens in new tab)`}
          >
            <Icon name="globe" size={12} /> Hugging Face
          </a>
        </div>
      </div>

      <div
        className="detail-tabs__tablist"
        role="tablist"
        aria-label="Model details sections"
        aria-labelledby="detail-panel-heading"
      >
        {HF_DETAIL_TABS.map((tab, i) => (
          <button
            key={tab.id}
            ref={el => { tabRefs.current[i] = el; }}
            role="tab"
            id={`detail-tab-${tab.id}`}
            aria-selected={activeTab === tab.id}
            aria-controls={`detail-panel-${tab.id}`}
            tabIndex={activeTab === tab.id ? 0 : -1}
            className={`detail-tabs__tab${activeTab === tab.id ? ' detail-tabs__tab--active' : ''}`}
            onClick={() => setActiveTab(tab.id)}
            onKeyDown={e => handleTabKeyDown(e, i)}
          >
            {tab.label}
          </button>
        ))}
      </div>

      {HF_DETAIL_TABS.map(tab => (
        <div
          key={tab.id}
          id={`detail-panel-${tab.id}`}
          role="tabpanel"
          aria-labelledby={`detail-tab-${tab.id}`}
          className={`detail-tabs__panel${activeTab === tab.id ? ' detail-tabs__panel--active' : ''}`}
          hidden={activeTab !== tab.id}
        >
          {tab.id === 'overview' && (
            <HfOverviewTab
              hfModel={hfModel}
              hfVariants={hfVariants}
              onHfPull={onHfPull}
              isPulling={isPulling}
              isActive={activeTab === 'overview'}
            />
          )}
          {tab.id === 'readme' && (
            <HfReadmeTab hfId={hfModel.id} isActive={activeTab === 'readme'} />
          )}
        </div>
      ))}
    </div>
  );
};



const ModelPresetsTab: React.FC<{
  model: ModelInfo;
  isActive: boolean;
}> = ({ model, isActive }) => {
  const name = mdName(model);
  const [allPresets, setAllPresets] = useState<Preset[]>(() => allStoredPresets());
  const [appliedPresets, setAppliedPresets] = useState<Record<string, string>>(() => loadApplied());
  const [notice, setNotice] = useState<string | null>(null);
  const [showChooser, setShowChooser] = useState(false);
  const liveRef = useRef<HTMLDivElement>(null);
  const changeBtnRef = useRef<HTMLButtonElement>(null);
  const chooserRef = useRef<HTMLDivElement>(null);

  // Reload when preset store changes
  useEffect(() => {
    const handler = () => {
      setAllPresets(allStoredPresets());
      setAppliedPresets(loadApplied());
    };
    window.addEventListener(PRESET_STORE_EVENT, handler);
    return () => window.removeEventListener(PRESET_STORE_EVENT, handler);
  }, []);

  // Close chooser when model changes
  useEffect(() => { setShowChooser(false); }, [name]);

  // Focus first focusable element in chooser when it opens
  useEffect(() => {
    if (showChooser) {
      requestAnimationFrame(() => {
        const first = chooserRef.current?.querySelector<HTMLElement>('button:not(.detail-presets__chooser-close), [tabindex="0"]');
        first?.focus();
      });
    }
  }, [showChooser]);

  const linkedPresetId = appliedPresets[name] || DEFAULT_PRESET.id;
  const linkedPreset = allPresets.find(p => p.id === linkedPresetId) || DEFAULT_PRESET;

  const compatiblePresets = useMemo(
    () => allPresets.filter(p => p.id !== DEFAULT_PRESET.id && isCompatible(p, model)),
    [allPresets, model],
  );

  const handleAttach = useCallback((preset: Preset) => {
    if (!name) return;
    setAppliedPresets(prev => {
      const next = { ...prev };
      if (preset.id === DEFAULT_PRESET.id) delete next[name];
      else next[name] = preset.id;
      saveApplied(next);
      return next;
    });
    const msg = preset.id === DEFAULT_PRESET.id
      ? `Reset to default preset for ${name}`
      : `Attached "${preset.name}" to ${name}`;
    setNotice(msg);
    setTimeout(() => setNotice(null), 2500);
  }, [name]);

  const handleAttachFromChooser = useCallback((preset: Preset) => {
    handleAttach(preset);
    setShowChooser(false);
    requestAnimationFrame(() => changeBtnRef.current?.focus());
  }, [handleAttach]);

  const handleCloseChooser = useCallback(() => {
    setShowChooser(false);
    requestAnimationFrame(() => changeBtnRef.current?.focus());
  }, []);

  const navigateToPresets = useCallback(() => {
    // Client-local deep-link to the global Presets page (no lemond involvement).
    window.dispatchEvent(new CustomEvent('lemonade:navigate', { detail: { view: 'presets' } }));
  }, []);

  if (!isActive) return null;

  const previewLines = effectivePresetParamPreviewLines(linkedPreset, model, undefined);

  return (
    <div className="detail-tab-content detail-presets">
      {/* Always-present live region for attachment announcements */}
      <div ref={liveRef} role="status" aria-live="polite" aria-atomic="true" className="sr-only">
        {notice || ''}
      </div>

      {/* Linked preset */}
      <section className="detail-presets__linked-section" aria-label="Linked preset">
        <h3 className="detail-presets__section-title">Linked preset</h3>
        <div
          className="detail-presets__card detail-presets__linked-card"
          aria-current="true"
          aria-label={`Active preset: ${linkedPreset.name}`}
        >
          <div className="detail-presets__card-header">
            <PresetIcon preset={linkedPreset} size={14} />
            <strong className="detail-presets__card-name">{linkedPreset.name}</strong>
            <span className="detail-presets__card-badge detail-presets__card-badge--linked">Active</span>
          </div>
          {linkedPreset.description && (
            <p className="detail-presets__card-desc">{linkedPreset.description}</p>
          )}
          {previewLines.length > 0 && (
            <p className="detail-presets__card-meta" aria-label="Preset parameters">
              {previewLines.join(' · ')}
            </p>
          )}
          {linkedPreset.id !== DEFAULT_PRESET.id && (
            <div className="detail-presets__linked-actions">
              <button
                ref={changeBtnRef}
                type="button"
                className="btn btn--primary btn--tiny detail-presets__change-btn"
                onClick={() => setShowChooser(v => !v)}
                aria-label={`Change linked preset for ${name}`}
                aria-expanded={showChooser}
                aria-haspopup="dialog"
              >
                Change
              </button>
              <button
                type="button"
                className="btn btn--ghost btn--tiny detail-presets__detach-btn"
                onClick={() => handleAttach(DEFAULT_PRESET)}
                aria-label={`Detach preset "${linkedPreset.name}" from ${name}, reset to default`}
              >
                Reset to default
              </button>
            </div>
          )}
        </div>

        {/* Inline change-preset chooser */}
        {showChooser && (
          <div
            ref={chooserRef}
            className="detail-presets__change-chooser"
            role="dialog"
            aria-label="Switch linked preset"
            aria-modal="true"
          >
            <div className="detail-presets__chooser-head">
              <span className="detail-presets__chooser-title">Switch to a different preset</span>
              <button
                type="button"
                className="detail-presets__chooser-close btn btn--ghost btn--tiny"
                onClick={handleCloseChooser}
                aria-label="Close preset chooser"
              >
                <Icon name="x" size={12} />
              </button>
            </div>
            {compatiblePresets.filter(p => p.id !== linkedPresetId).length === 0 ? (
              <p className="detail-presets__chooser-empty">
                {compatiblePresets.length === 0
                  ? 'No compatible presets available. Create one in the Presets page.'
                  : 'No other compatible presets to switch to.'}
              </p>
            ) : (
              <ul className="detail-presets__chooser-list" role="listbox" aria-label="Select a preset to switch to">
                {compatiblePresets
                  .filter(p => p.id !== linkedPresetId)
                  .map(preset => (
                    <li key={preset.id} role="option" aria-selected={false}>
                      <button
                        type="button"
                        className="detail-presets__chooser-option"
                        onClick={() => handleAttachFromChooser(preset)}
                        aria-label={`Switch to preset "${preset.name}"`}
                      >
                        <PresetIcon preset={preset} size={12} />
                        <span className="detail-presets__chooser-option-name">{preset.name}</span>
                        {preset.description && (
                          <span className="detail-presets__chooser-option-desc">{preset.description}</span>
                        )}
                      </button>
                    </li>
                  ))}
              </ul>
            )}
          </div>
        )}
      </section>

      {/* Recommended / compatible presets — a neat grid of compact cards */}
      {compatiblePresets.length > 0 && (
        <section className="detail-presets__recommended-section" aria-label="Recommended presets">
          <div className="detail-presets__section-head">
            <h3 className="detail-presets__section-title">Recommended presets</h3>
            <button
              type="button"
              className="btn btn--ghost btn--tiny detail-presets__browse-btn"
              onClick={navigateToPresets}
              aria-label="Browse all presets in the Presets page"
            >
              Browse presets
            </button>
          </div>
          <ul
            className="detail-presets__preset-grid"
            role="list"
            aria-label="Recommended presets — select to attach"
          >
            {compatiblePresets.map(preset => {
              const isLinked = preset.id === linkedPresetId;
              const paramLines = effectivePresetParamPreviewLines(preset, model, undefined);
              return (
                <li
                  key={preset.id}
                  aria-current={isLinked ? 'true' : undefined}
                  className={`detail-presets__card detail-presets__preset-card detail-presets__preset-card--sm${isLinked ? ' detail-presets__preset-card--selected' : ''}`}
                  aria-label={`${preset.name}${isLinked ? ' (currently linked)' : ''}`}
                >
                  <div className="detail-presets__card-header">
                    <PresetIcon preset={preset} size={13} />
                    <strong className="detail-presets__card-name">{preset.name}</strong>
                    {isLinked && <span className="detail-presets__card-badge detail-presets__card-badge--linked">Linked</span>}
                  </div>
                  {preset.description && (
                    <p className="detail-presets__card-desc">{preset.description}</p>
                  )}
                  {paramLines.length > 0 && (
                    <p className="detail-presets__card-meta">{paramLines.join(' · ')}</p>
                  )}
                  <div className="detail-presets__card-footer">
                    {isLinked ? (
                      <span className="detail-presets__card-linked-note">Currently linked</span>
                    ) : (
                      <button
                        type="button"
                        className="btn btn--primary btn--tiny detail-presets__attach-btn"
                        onClick={() => handleAttach(preset)}
                        aria-label={`${linkedPreset.id !== DEFAULT_PRESET.id ? 'Switch to' : 'Attach'} preset "${preset.name}" for ${name}`}
                      >
                        {linkedPreset.id !== DEFAULT_PRESET.id ? 'Switch' : 'Attach'}
                      </button>
                    )}
                  </div>
                </li>
              );
            })}
          </ul>
        </section>
      )}

      {compatiblePresets.length === 0 && (
        <div className="detail-presets__empty-block">
          <p className="detail-presets__empty">No compatible presets found. Create a preset in the Presets page and set the model type to match this model.</p>
          <button
            type="button"
            className="btn btn--ghost btn--tiny detail-presets__browse-btn"
            onClick={navigateToPresets}
            aria-label="Manage presets in the Presets page"
          >
            Manage presets
          </button>
        </div>
      )}
    </div>
  );
};


/* ── Model tuning tab ────────────────────────────────────────── */

function tuningSourceLabel(source: TuningValueSource | undefined): string {
  switch (source) {
    case 'custom': return 'Custom tuning';
    case 'built-in': return 'Built-in tuning';
    case 'optimized': return 'Optimized';
    default: return 'Generic fallback';
  }
}

const ModelTuningTab: React.FC<{
  model: ModelInfo;
  loadedModel: LoadedModel | null;
  isActive: boolean;
  serverDefaultCtxSize: number;
  onReloadModel?: (model: LoadedModel, recipeOptions?: Record<string, unknown>) => Promise<void>;
}> = ({ model, loadedModel, isActive, serverDefaultCtxSize, onReloadModel }) => {
  const name = mdName(model);
  const [storeVersion, setStoreVersion] = useState(0);
  const [notice, setNotice] = useState<string | null>(null);
  const [isReloading, setIsReloading] = useState(false);
  const [systemInfo, setSystemInfo] = useState<Record<string, unknown> | null>(() => api.systemInfoData);

  useEffect(() => {
    const handler = () => setStoreVersion(v => v + 1);
    window.addEventListener(PRESET_STORE_EVENT, handler);
    return () => window.removeEventListener(PRESET_STORE_EVENT, handler);
  }, []);

  useEffect(() => {
    if (!isActive) return;
    let alive = true;
    const cached = api.systemInfoData;
    if (cached) setSystemInfo(cached);
    api.systemInfo()
      .then(info => { if (alive) setSystemInfo(info); })
      .catch(() => { if (alive) setSystemInfo(api.systemInfoData); });
    return () => { alive = false; };
  }, [isActive]);

  const linkedPreset = activePresetForModel(name);
  const compatiblePresets = useMemo(
    () => allStoredPresets().filter(preset => isCompatible(preset, model)),
    [model, storeVersion],
  );
  const [selectedPresetId, setSelectedPresetId] = useState(linkedPreset.id);

  useEffect(() => {
    if (!compatiblePresets.some(preset => preset.id === selectedPresetId)) {
      setSelectedPresetId(linkedPreset.id);
    }
  }, [compatiblePresets, linkedPreset.id, selectedPresetId]);

  const selectedPreset = compatiblePresets.find(preset => preset.id === selectedPresetId) || linkedPreset;
  const selectedPresetIsLinked = selectedPreset.id === linkedPreset.id;
  const userTuning = useMemo(
    () => loadModelTuning(name, selectedPreset.id),
    [name, selectedPreset.id, storeVersion],
  );
  const baseTuning = useMemo(
    () => modelBaseTuningForModel(model, serverDefaultCtxSize, selectedPreset),
    [model, serverDefaultCtxSize, selectedPreset],
  );
  const resolvedTuning = useMemo(
    () => resolvedModelTuningForPreset(name, model, selectedPreset, serverDefaultCtxSize),
    [name, model, selectedPreset, serverDefaultCtxSize, storeVersion],
  );
  const effectiveTuning = resolvedTuning.tuning;
  const recipeKeys = useMemo(() => tuningKeysForModel(model), [model]);
  const activeArgsKey = useMemo(() => recipeKeys.find(key => ARGS_TUNING_KEYS.has(key)) as keyof RecipeOptions | undefined, [recipeKeys]);
  const allowSampling = samplingAllowedForModel(model);

  const [temperatureIntentDraft, setTemperatureIntentDraft] = useState<Partial<Record<TemperatureHint, string>>>({});
  const [contextIntentDraft, setContextIntentDraft] = useState<Partial<Record<EditableContextHint, string>>>({});
  const [selectedContextIntent, setSelectedContextIntent] = useState<EditableContextHint>(() => {
    const hint = selectedPreset.context_hint || 'medium';
    return hint === 'max' ? 'large' : hint;
  });
  const [recipeDraft, setRecipeDraft] = useState<Record<string, string>>({});
  const [samplingDraft, setSamplingDraft] = useState<Record<string, string>>({});
  const [backendArgsDrafts, setBackendArgsDrafts] = useState<Record<string, string>>({});

  useEffect(() => {
    const nextUser = loadModelTuning(name, selectedPreset.id);
    const nextTemperatureIntent: Partial<Record<TemperatureHint, string>> = {};
    for (const hint of TEMPERATURE_HINTS) {
      const value = nextUser?.intent_values?.temperature?.[hint];
      if (value !== undefined) nextTemperatureIntent[hint] = fieldValue(value);
    }
    const nextContextIntent: Partial<Record<EditableContextHint, string>> = {};
    for (const hint of EDITABLE_CONTEXT_HINTS) {
      const value = nextUser?.intent_values?.context?.[hint];
      if (value !== undefined) nextContextIntent[hint] = fieldValue(value);
    }
    const nextRecipe: Record<string, string> = {};
    for (const [key, value] of Object.entries(nextUser?.recipe_options || {})) {
      if (key !== 'ctx_size') nextRecipe[key] = fieldValue(value);
    }
    const nextSampling: Record<string, string> = {};
    for (const [key, value] of Object.entries(nextUser?.sampling || {})) nextSampling[key] = fieldValue(value);
    const nextArgMemory: Record<string, string> = {};
    for (const backendKey of BACKEND_TUNING_KEYS) {
      const argsKey = BACKEND_ARGS_KEY[backendKey];
      if (!argsKey) continue;
      const backendValue = nextRecipe[backendKey] || '';
      const argsValue = nextRecipe[argsKey] || '';
      if (backendValue || argsValue) nextArgMemory[`${String(backendKey)}:${backendValue}`] = argsValue;
    }
    setTemperatureIntentDraft(nextTemperatureIntent);
    setContextIntentDraft(nextContextIntent);
    setRecipeDraft(nextRecipe);
    setSamplingDraft(nextSampling);
    setBackendArgsDrafts(nextArgMemory);
    setNotice(null);
  }, [name, selectedPreset.id, storeVersion]);

  useEffect(() => {
    const hint = selectedPreset.context_hint || 'medium';
    setSelectedContextIntent(hint === 'max' ? 'large' : hint);
  }, [name, selectedPreset.id, selectedPreset.context_hint]);

  if (!isActive) return null;

  const recipes = recipesForDisplay(model);
  const cap = capabilityFromModelInfo(model);
  const hasUserTuning = !!userTuning && (
    Object.keys(userTuning.intent_values?.temperature || {}).length > 0 ||
    Object.keys(userTuning.intent_values?.context || {}).length > 0 ||
    Object.keys(userTuning.recipe_options).length > 0 ||
    Object.keys(userTuning.sampling).length > 0 ||
    !!userTuning.engine_hint
  );
  const hasDraftValues = Object.values(temperatureIntentDraft).some(value => value?.trim())
    || Object.values(contextIntentDraft).some(value => value?.trim())
    || Object.values(recipeDraft).some(value => value.trim())
    || Object.values(samplingDraft).some(value => value.trim());

  const setRecipeField = (key: keyof RecipeOptions, value: string) => {
    if (BACKEND_TUNING_KEYS.has(key)) {
      const argsKey = BACKEND_ARGS_KEY[key];
      setRecipeDraft(prev => {
        const next = { ...prev, [key]: value };
        if (argsKey) {
          const previousBackend = prev[key] || '';
          const previousArgs = prev[argsKey] || '';
          const previousMemoryKey = `${String(key)}:${previousBackend}`;
          const nextMemoryKey = `${String(key)}:${value}`;
          const rememberedArgs = backendArgsDrafts[nextMemoryKey];
          setBackendArgsDrafts(mem => ({ ...mem, [previousMemoryKey]: previousArgs }));
          next[argsKey] = rememberedArgs ?? '';
        }
        return next;
      });
      return;
    }

    setRecipeDraft(prev => ({ ...prev, [key]: value }));
  };

  const setSamplingField = (key: keyof SamplingParams, value: string) => {
    setSamplingDraft(prev => ({ ...prev, [key]: value }));
  };

  const setTemperatureIntentField = (hint: TemperatureHint, value: string) => {
    setTemperatureIntentDraft(prev => ({ ...prev, [hint]: value }));
  };

  const setContextIntentField = (hint: EditableContextHint, value: string) => {
    setContextIntentDraft(prev => ({ ...prev, [hint]: value }));
  };

  const clearTemperatureIntentField = (hint: TemperatureHint) => {
    setTemperatureIntentDraft(prev => {
      const next = { ...prev };
      delete next[hint];
      return next;
    });
  };

  const clearContextIntentField = (hint: EditableContextHint) => {
    setContextIntentDraft(prev => {
      const next = { ...prev };
      delete next[hint];
      return next;
    });
  };

  const clearRecipeField = (key: keyof RecipeOptions) => {
    setRecipeDraft(prev => {
      const next = { ...prev };
      delete next[key];
      return next;
    });
  };

  const clearSamplingField = (key: keyof SamplingParams) => {
    setSamplingDraft(prev => {
      const next = { ...prev };
      delete next[key];
      return next;
    });
  };

  const buildRecipeOptions = (): RecipeOptions => {
    const raw: Partial<RecipeOptions> = {};
    for (const [key, value] of Object.entries(recipeDraft) as Array<[keyof RecipeOptions, string]>) {
      if (key === 'ctx_size' || !value.trim()) continue;
      if (BOOLEAN_TUNING_KEYS.has(key)) {
        (raw as Record<string, unknown>)[key] = value === 'true';
      } else if (NUMERIC_TUNING_KEYS.has(key)) {
        const n = parseNumberOrUndefined(value);
        if (n !== undefined) (raw as Record<string, unknown>)[key] = n;
      } else {
        (raw as Record<string, unknown>)[key] = value.trim();
      }
    }
    return sanitizeRecipeOptions(raw);
  };

  const buildIntentValues = () => {
    const temperature: Partial<Record<TemperatureHint, number>> = {};
    const context: Partial<Record<EditableContextHint, number>> = {};
    for (const hint of TEMPERATURE_HINTS) {
      const value = parseNumberOrUndefined(temperatureIntentDraft[hint] || '');
      if (value !== undefined) temperature[hint] = value;
    }
    for (const hint of EDITABLE_CONTEXT_HINTS) {
      const value = parseNumberOrUndefined(contextIntentDraft[hint] || '');
      if (value !== undefined) context[hint] = Math.min(resolvedTuning.max_context, Math.max(1, Math.round(value)));
    }
    return { temperature, context };
  };

  const buildSampling = (): SamplingParams => {
    const raw: Partial<SamplingParams> = {};
    for (const key of ['top_p', 'top_k', 'repeat_penalty'] as Array<keyof SamplingParams>) {
      const n = parseNumberOrUndefined(samplingDraft[key] || '');
      if (n !== undefined) raw[key] = n;
    }
    return sanitizeSamplingParams(raw);
  };

  const saveDraft = () => {
    saveModelTuning(name, { intent_values: buildIntentValues(), recipe_options: buildRecipeOptions(), sampling: buildSampling() }, selectedPreset.id);
    setNotice(`Model tuning saved for ${selectedPreset.name}. Temperature intent applies to the next request; context and runtime fields apply on the next load.`);
  };

  const resetDraft = () => {
    resetModelTuning(name, selectedPreset.id);
    setTemperatureIntentDraft({});
    setContextIntentDraft({});
    setRecipeDraft({});
    setSamplingDraft({});
    setBackendArgsDrafts({});
    setNotice(`Model tuning for ${selectedPreset.name} restored to its resolved built-in and generic values.`);
  };

  const reloadWithTuning = async () => {
    if (!loadedModel || !onReloadModel) return;
    saveDraft();
    setIsReloading(true);
    try {
      await onReloadModel(loadedModel);
      setNotice('Model reloaded with current tuning.');
    } catch {
      setNotice('Could not reload this model with the current tuning.');
    } finally {
      setIsReloading(false);
    }
  };

  const renderClearOverrideButton = (onClick: () => void, disabled: boolean) => (
    <button type="button" className="btn btn--ghost btn--tiny detail-tuning__default-btn" onClick={onClick} disabled={disabled}>
      Clear
    </button>
  );

  const renderTemperatureIntentField = (hint: TemperatureHint) => {
    const override = temperatureIntentDraft[hint];
    const resolved = resolvedTuning.intent_values.temperature[hint];
    const value = override ?? String(resolved);
    const active = (selectedPreset.temperature_hint || 'balanced') === hint;
    return (
      <label key={hint} className={`detail-tuning__intent-card${active ? ' is-active' : ''}`}>
        <span className="detail-tuning__intent-name">
          <Icon name={TEMPERATURE_INTENT_ICONS[hint]} size={14} aria-hidden="true" />
          {TEMPERATURE_HINT_LABELS[hint]}
          {active && <span className="detail-tuning__active-chip">Active</span>}
        </span>
        <div className="detail-tuning__intent-control">
          <input
            className="input detail-tuning__intent-input"
            type="number"
            min={0}
            max={2}
            step={0.05}
            value={value}
            onChange={event => setTemperatureIntentField(hint, event.target.value)}
            aria-label={`${TEMPERATURE_HINT_LABELS[hint]} temperature value`}
            data-model-tuning-temperature-intent={hint}
          />
          {renderClearOverrideButton(() => clearTemperatureIntentField(hint), override === undefined)}
        </div>
        <small>{tuningSourceLabel(resolvedTuning.intent_sources.temperature[hint])}</small>
      </label>
    );
  };

  const contextIntentValue = (hint: EditableContextHint): number => {
    const override = parseNumberOrUndefined(contextIntentDraft[hint] || '');
    return override ?? resolvedTuning.intent_values.context[hint];
  };

  const contextIntentBounds = (hint: EditableContextHint): { min: number; max: number; step: number } => {
    const modelMaximum = Math.max(1, resolvedTuning.max_context);
    const step = modelMaximum >= 1024 ? 1024 : 1;
    const minimum = hint === 'small'
      ? Math.min(step, modelMaximum)
      : contextIntentValue(hint === 'medium' ? 'small' : 'medium');
    const maximum = hint === 'large'
      ? modelMaximum
      : contextIntentValue(hint === 'small' ? 'medium' : 'large');
    return {
      min: Math.min(minimum, maximum),
      max: Math.max(minimum, maximum),
      step,
    };
  };

  const setBoundedContextIntent = (hint: EditableContextHint, value: number) => {
    const bounds = contextIntentBounds(hint);
    const rounded = bounds.step > 1 ? Math.round(value / bounds.step) * bounds.step : Math.round(value);
    const bounded = Math.min(bounds.max, Math.max(bounds.min, rounded));
    setContextIntentField(hint, String(bounded));
  };

  const renderContextIntentField = (hint: ContextHint) => {
    const active = (selectedPreset.context_hint || 'medium') === hint;
    if (hint === 'max') {
      return (
        <div key={hint} className={`detail-tuning__intent-card detail-tuning__intent-card--fixed${active ? ' is-active' : ''}`} data-model-tuning-context-intent="max">
          <span className="detail-tuning__intent-name">
            <Icon name={CONTEXT_INTENT_ICONS[hint]} size={14} aria-hidden="true" />
            {CONTEXT_HINT_LABELS[hint]}
            {active && <span className="detail-tuning__active-chip">Active</span>}
          </span>
          <output className="detail-tuning__intent-output">{formatContextSize(resolvedTuning.max_context)}</output>
          <small>Model maximum</small>
        </div>
      );
    }

    const editableHint = hint as EditableContextHint;
    const selected = selectedContextIntent === editableHint;
    const value = contextIntentValue(editableHint);
    return (
      <button
        key={hint}
        type="button"
        className={`detail-tuning__intent-card detail-tuning__intent-card--selectable${active ? ' is-active' : ''}${selected ? ' is-selected' : ''}`}
        onClick={() => setSelectedContextIntent(editableHint)}
        aria-pressed={selected}
        data-model-tuning-context-intent={hint}
      >
        <span className="detail-tuning__intent-name">
          <Icon name={CONTEXT_INTENT_ICONS[hint]} size={14} aria-hidden="true" />
          {CONTEXT_HINT_LABELS[hint]}
          {active && <span className="detail-tuning__active-chip">Active</span>}
        </span>
        <output className="detail-tuning__intent-output">{formatContextSize(value)}</output>
        <small>{tuningSourceLabel(resolvedTuning.intent_sources.context[hint])}{selected ? ' · Editing' : ''}</small>
      </button>
    );
  };

  const selectedContextBounds = contextIntentBounds(selectedContextIntent);
  const selectedContextRawValue = contextIntentValue(selectedContextIntent);
  const selectedContextValue = Math.min(selectedContextBounds.max, Math.max(selectedContextBounds.min, selectedContextRawValue));
  const selectedContextOverride = contextIntentDraft[selectedContextIntent];

  const renderRecipeField = (key: keyof RecipeOptions) => {
    const baseValue = baseTuning.recipe_options[key];
    const draftValue = recipeDraft[key] || '';
    const inputId = `tuning-${name}-${key}`.replace(/[^a-zA-Z0-9_-]/g, '-');
    const label = TUNING_FIELD_LABELS[key] || key;
    const hint = TUNING_FIELD_HINTS[key];

    if (BACKEND_TUNING_KEYS.has(key)) {
      const activeBackend = activeBackendValue(key, baseValue, model, systemInfo);
      const current = draftValue || activeBackend;
      const options = backendOptionsForKey(key, current, model, systemInfo).filter(option => option !== activeBackend);
      return (
        <label key={key} className="detail-tuning__field" htmlFor={inputId}>
          <span>{label}</span>
          <select id={inputId} className="select" value={draftValue} onChange={e => setRecipeField(key, e.target.value)}>
            <option value="">{activeBackend}</option>
            {options.map(option => (
              <option key={option} value={option}>{option}</option>
            ))}
          </select>
          {hint && <small>{hint}</small>}
        </label>
      );
    }

    if (DEVICE_TUNING_KEYS.has(key)) {
      const backendKey: keyof RecipeOptions = 'llamacpp_backend';
      const selectedBackend = recipeDraft[backendKey] || activeBackendValue(backendKey, baseTuning.recipe_options[backendKey], model, systemInfo);
      const activeDevice = optionalDisplayValue(baseValue) || 'auto';
      const current = draftValue || activeDevice;
      const options = deviceOptionsForKey(key, current, selectedBackend, model, systemInfo).filter(option => option !== activeDevice);
      return (
        <label key={key} className="detail-tuning__field" htmlFor={inputId}>
          <span>{label}</span>
          <select id={inputId} className="select" value={draftValue} onChange={e => setRecipeField(key, e.target.value)}>
            <option value="">{activeDevice}</option>
            {options.map(option => (
              <option key={option} value={option}>{option}</option>
            ))}
          </select>
          {hint && <small>{hint}</small>}
        </label>
      );
    }

    if (key === 'mmproj_enabled') {
      const activeState = typeof baseValue === 'boolean' && !baseValue ? 'Off' : 'On';
      return (
        <label key={key} className="detail-tuning__field" htmlFor={inputId}>
          <span>{label}</span>
          <select id={inputId} className="select" value={draftValue} onChange={e => setRecipeField(key, e.target.value)}>
            <option value="">{activeState}</option>
            <option value="true">On</option>
            <option value="false">Off</option>
          </select>
          {hint && <small>{hint}</small>}
        </label>
      );
    }

    if (BOOLEAN_TUNING_KEYS.has(key)) {
      const argsKey = activeArgsKey;
      const hasModelArgs = !!(argsKey && (recipeDraft[argsKey] || fieldValue(baseTuning.recipe_options[argsKey])));
      const activeBehavior = typeof baseValue === 'boolean'
        ? (baseValue ? 'Merge backend + model args' : 'Use model args only')
        : (hasModelArgs ? 'Use model args only' : 'Use backend args only');
      const setArgsBehavior = (value: string) => {
        if (value === '__backend_only') {
          setRecipeDraft(prev => {
            const next = { ...prev };
            delete next[key];
            if (argsKey) delete next[argsKey];
            return next;
          });
          return;
        }
        setRecipeField(key, value);
      };
      return (
        <label key={key} className="detail-tuning__field" htmlFor={inputId}>
          <span>{label}</span>
          <select id={inputId} className="select" value={draftValue} onChange={e => setArgsBehavior(e.target.value)}>
            <option value="">{activeBehavior}</option>
            <option value="__backend_only">Use backend args only</option>
            <option value="false">Use model args only</option>
            <option value="true">Merge backend + model args</option>
          </select>
          {hint && <small>{hint}</small>}
        </label>
      );
    }

    const sliderSpec = numericSliderSpec(key);
    if (sliderSpec) {
      const baseNumber = parseNumberOrUndefined(fieldValue(baseValue));
      const currentValue = parseNumberOrUndefined(draftValue) ?? baseNumber ?? sliderSpec.fallback;
      return (
        <label key={key} className="detail-tuning__field" htmlFor={inputId}>
          <span>{label}</span>
          <div className="field__row detail-tuning__control-row">
            <input
              id={inputId}
              className="slider"
              type="range"
              min={sliderSpec.min}
              max={sliderSpec.max}
              step={sliderSpec.step}
              value={currentValue}
              onChange={e => setRecipeField(key, e.target.value)}
            />
            <span className="field__value">{sliderDisplay(currentValue, sliderSpec.digits)}</span>
            {renderClearOverrideButton(() => clearRecipeField(key), !draftValue)}
          </div>
          {hint && <small>{hint}</small>}
        </label>
      );
    }

    if (ARGS_TUNING_KEYS.has(key)) {
      return (
        <label key={key} className="detail-tuning__field detail-tuning__field--wide" htmlFor={inputId}>
          <span>{label}</span>
          <textarea
            id={inputId}
            className="input detail-tuning__args"
            rows={3}
            value={draftValue}
            placeholder={optionalDisplayValue(baseValue) || 'Type backend args here...'}
            onChange={e => setRecipeField(key, e.target.value)}
          />
          {hint && <small>{hint}</small>}
        </label>
      );
    }

    return (
      <label key={key} className="detail-tuning__field" htmlFor={inputId}>
        <span>{label}</span>
        <input
          id={inputId}
          className="input"
          type={NUMERIC_TUNING_KEYS.has(key) ? 'number' : 'text'}
          value={draftValue}
          placeholder={optionalDisplayValue(baseValue) || 'Type a value here...'}
          onChange={e => setRecipeField(key, e.target.value)}
        />
        {hint && <small>{hint}</small>}
      </label>
    );
  };

  const renderSamplingField = (key: keyof SamplingParams) => {
    const inputId = `tuning-${name}-${key}`.replace(/[^a-zA-Z0-9_-]/g, '-');
    const draftValue = samplingDraft[key] || '';
    const baseValue = baseTuning.sampling[key];
    const spec = numericSliderSpec(key)!;
    const currentValue = parseNumberOrUndefined(draftValue) ?? (typeof baseValue === 'number' ? baseValue : undefined) ?? spec.fallback;
    return (
      <label key={key} className="detail-tuning__field" htmlFor={inputId}>
        <span>{key}</span>
        <div className="field__row detail-tuning__control-row">
          <input
            id={inputId}
            className="slider"
            type="range"
            min={spec.min}
            max={spec.max}
            step={spec.step}
            value={currentValue}
            onChange={e => setSamplingField(key, e.target.value)}
          />
          <span className="field__value">{sliderDisplay(currentValue, spec.digits)}</span>
          {renderClearOverrideButton(() => clearSamplingField(key), !draftValue)}
        </div>
        <small>{draftValue ? 'Override for this model' : `Current: ${tuningValue(baseValue)}`}</small>
      </label>
    );
  };

  const effectiveRecipeEntries = Object.entries(effectiveTuning.recipe_options || {});
  const effectiveSamplingEntries = Object.entries(effectiveTuning.sampling || {});

  return (
    <div className="detail-tab-content detail-tuning">
      <div role="status" aria-live="polite" aria-atomic="true" className="sr-only">{notice || ''}</div>

      <section className="detail-tuning__intro" aria-label="Model tuning concept">
        <h3 className="detail-tuning__title">Model Tuning</h3>
        <p>
          Presets describe intent. Model Tuning stores the concrete implementation separately for every model × preset combination.
        </p>
      </section>

      <section className="detail-tuning__summary" aria-label="Effective runtime summary">
        <label className="detail-tuning__summary-card detail-tuning__preset-select">
          <span className="detail-tuning__summary-label">Selected preset</span>
          <select
            className="select"
            value={selectedPreset.id}
            onChange={event => {
              const nextId = event.target.value;
              const nextPreset = compatiblePresets.find(preset => preset.id === nextId);
              const nextHint = nextPreset?.context_hint || 'medium';
              setSelectedContextIntent(nextHint === 'max' ? 'large' : nextHint);
              setSelectedPresetId(nextId);
            }}
            data-model-tuning-preset
          >
            {compatiblePresets.map(preset => (
              <option key={preset.id} value={preset.id}>{preset.name}{preset.id === linkedPreset.id ? ' (linked)' : ''}</option>
            ))}
          </select>
        </label>
        <div className="detail-tuning__summary-card">
          <span className="detail-tuning__summary-label">Capability</span>
          <strong>{capabilityLabel(cap)}</strong>
        </div>
        <div className="detail-tuning__summary-card">
          <span className="detail-tuning__summary-label">Recipe</span>
          <strong>{recipes.length ? recipes.map(recipeDisplayLabel).join(' / ') : 'Auto'}</strong>
        </div>
        <div className="detail-tuning__summary-card">
          <span className="detail-tuning__summary-label">Pair source</span>
          <strong>{resolvedTuning.tuning.source === 'optimized' ? 'AutoOpt optimized' : (hasUserTuning ? 'Custom tuning' : 'Resolved defaults')}</strong>
        </div>
      </section>

      <section className="detail-tuning__effective" aria-label="Effective tuning values">
        <h3 className="detail-tuning__section-title">Effective runtime</h3>
        {effectiveRecipeEntries.length === 0 && effectiveSamplingEntries.length === 0 ? (
          <p className="detail-tuning__empty">No local overrides are needed. Lemonade will use the current model and backend values.</p>
        ) : (
          <div className="detail-tuning__kv-grid">
            {effectiveRecipeEntries.map(([key, value]) => (
              <div className="detail-tuning__kv" key={`ro-${key}`}>
                <span>{TUNING_FIELD_LABELS[key as keyof RecipeOptions] || key}</span>
                <code>{tuningValue(value)}</code>
                <small>
                  {key === 'ctx_size' && selectedPreset.context_hint === 'max'
                    && resolvedTuning.sources.recipe_options.ctx_size === 'generic'
                    ? 'Model maximum'
                    : tuningSourceLabel(resolvedTuning.sources.recipe_options[key as keyof RecipeOptions])}
                </small>
              </div>
            ))}
            {effectiveSamplingEntries.map(([key, value]) => (
              <div className="detail-tuning__kv" key={`sp-${key}`}>
                <span>{key}</span>
                <code>{tuningValue(value)}</code>
                <small>{tuningSourceLabel(resolvedTuning.sources.sampling[key as keyof SamplingParams])}</small>
              </div>
            ))}
            {allowSampling && (
              <div className="detail-tuning__kv" key="thinking-mode">
                <span>Thinking</span>
                <code>{resolvedTuning.thinking_mode}</code>
                <small>{tuningSourceLabel(resolvedTuning.sources.thinking_mode)}</small>
              </div>
            )}
          </div>
        )}
      </section>

      <section className="detail-tuning__editor" aria-label="Customize model tuning">
        <div className="detail-tuning__section-head">
          <div>
            <h3 className="detail-tuning__section-title">Customize {selectedPreset.name}</h3>
            <p className="detail-tuning__hint">Overrides apply only to {name} × {selectedPreset.name}. Leave a field blank to use the resolved value.</p>
          </div>
          {notice && <p className="detail-tuning__notice">{notice}</p>}
        </div>

        {allowSampling && (
          <div className="detail-tuning__intent-map" aria-label="Intent translation values">
            <div className="detail-tuning__intent-group">
              <div className="detail-tuning__intent-head">
                <h4><Icon name="thermometer" size={15} aria-hidden="true" /> Temperature intent</h4>
                <span>Concrete value used for every temperature level</span>
              </div>
              <div className="detail-tuning__intent-grid">
                {TEMPERATURE_HINTS.map(renderTemperatureIntentField)}
              </div>
            </div>
            <div className="detail-tuning__intent-group">
              <div className="detail-tuning__intent-head">
                <h4><Icon name="scan-text" size={15} aria-hidden="true" /> Context intent</h4>
                <span>Max always follows the model-supported maximum</span>
              </div>
              <div className="detail-tuning__intent-grid">
                {(['small', 'medium', 'large', 'max'] as ContextHint[]).map(renderContextIntentField)}
              </div>
              <div className="detail-tuning__context-slider">
                <span className="detail-tuning__context-slider-head">
                  <strong>{CONTEXT_HINT_LABELS[selectedContextIntent]} context</strong>
                  <output>{formatContextSize(selectedContextValue)} · {selectedContextValue.toLocaleString()} tokens</output>
                </span>
                <div className="detail-tuning__context-slider-row">
                  <span className="detail-tuning__context-bound">{formatContextSize(selectedContextBounds.min)}</span>
                  <input
                    className="slider"
                    type="range"
                    min={selectedContextBounds.min}
                    max={selectedContextBounds.max}
                    step={selectedContextBounds.step}
                    value={selectedContextValue}
                    onChange={event => setBoundedContextIntent(selectedContextIntent, Number(event.target.value))}
                    aria-label={`${CONTEXT_HINT_LABELS[selectedContextIntent]} context size`}
                    data-model-tuning-context-slider={selectedContextIntent}
                  />
                  <span className="detail-tuning__context-bound">{formatContextSize(selectedContextBounds.max)}</span>
                  <input
                    className="input detail-tuning__context-number"
                    type="number"
                    min={selectedContextBounds.min}
                    max={selectedContextBounds.max}
                    step={selectedContextBounds.step}
                    value={selectedContextValue}
                    onChange={event => setBoundedContextIntent(selectedContextIntent, Number(event.target.value))}
                    aria-label={`${CONTEXT_HINT_LABELS[selectedContextIntent]} context tokens`}
                    data-model-tuning-context-number={selectedContextIntent}
                  />
                  {renderClearOverrideButton(() => clearContextIntentField(selectedContextIntent), selectedContextOverride === undefined)}
                </div>
                <small>Range is constrained by the neighboring context intents. Max remains fixed to the model maximum.</small>
              </div>
            </div>
          </div>
        )}

        {recipeKeys.length > 0 && (
          <div className="detail-tuning__runtime">
            <h4>Load-specific settings</h4>
            <div className="detail-tuning__field-grid">
              {recipeKeys.map(renderRecipeField)}
            </div>
          </div>
        )}

        {allowSampling && (
          <div className="detail-tuning__sampling">
            <h4>Advanced sampling</h4>
            <div className="detail-tuning__field-grid">
              {(['top_p', 'top_k', 'repeat_penalty'] as Array<keyof SamplingParams>).map(renderSamplingField)}
            </div>
          </div>
        )}

        <div className="detail-tuning__actions">
          <button type="button" className="btn btn--primary btn--sm" onClick={saveDraft}>Save tuning</button>
          <button type="button" className="btn btn--ghost btn--sm" onClick={resetDraft} disabled={!hasUserTuning && !hasDraftValues}>Reset tuning</button>
          {loadedModel && onReloadModel && (
            <button
              type="button"
              className="btn btn--ghost btn--sm"
              onClick={reloadWithTuning}
              disabled={isReloading || !selectedPresetIsLinked}
              aria-busy={isReloading}
              title={selectedPresetIsLinked ? 'Reload the model with this tuning' : 'Link this preset before reloading with its tuning'}
            >
              <Icon name="rotate-ccw" size={13} aria-hidden="true" /> {isReloading ? 'Reloading…' : 'Reload with tuning'}
            </button>
          )}
        </div>
      </section>
    </div>
  );
};

/* ── Files tab ───────────────────────────────────────────────── */

/** Human-readable byte size (B / KB / MB / GB) using binary units. */
function fmtBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes <= 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  const decimals = unit === 0 ? 0 : value >= 100 ? 0 : value >= 10 ? 1 : 2;
  return `${value.toFixed(decimals)} ${units[unit]}`;
}

/** Title-case a role slug for display (e.g. "mmproj" → "Mmproj", "main" → "Main"). */
function roleLabel(role: string): string {
  const r = String(role || '').trim();
  if (!r) return 'File';
  return r.charAt(0).toUpperCase() + r.slice(1);
}

const ModelFilesTab: React.FC<{ model: ModelInfo | null | undefined; isActive: boolean }> = ({ model, isActive }) => {
  const modelId = model ? String(model.id || '') : '';
  const [files, setFiles] = useState<ModelFileInfo[] | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState(false);

  useEffect(() => {
    if (!isActive) return;
    if (!modelId) { setFiles([]); return; }

    let cancelled = false;
    setLoading(true);
    setError(false);
    api.getModelFiles(modelId)
      .then(resp => {
        if (cancelled) return;
        if (!resp) { setError(true); setFiles(null); return; }
        setFiles(resp.files);
      })
      .catch(() => { if (!cancelled) { setError(true); setFiles(null); } })
      .finally(() => { if (!cancelled) setLoading(false); });

    return () => { cancelled = true; };
  }, [modelId, isActive]);

  if (loading) {
    return (
      <div className="detail-tab-content detail-files detail-files--loading" aria-live="polite" aria-busy="true">
        <span>Loading files…</span>
      </div>
    );
  }

  if (error) {
    return (
      <div className="detail-tab-content detail-files detail-files--empty">
        <Icon name="hard-drive" size={32} aria-hidden="true" />
        <p>Unable to load files for this model.</p>
      </div>
    );
  }

  if (!files || files.length === 0) {
    return (
      <div className="detail-tab-content detail-files detail-files--empty">
        <Icon name="hard-drive" size={32} aria-hidden="true" />
        <p>No files found for this model.</p>
        <small>Files appear here once the model has been downloaded.</small>
      </div>
    );
  }

  return (
    <div className="detail-tab-content detail-files">
      <table className="detail-files__table">
        <caption className="sr-only">Files backing {mdName(model) || modelId}</caption>
        <thead>
          <tr>
            <th scope="col">File</th>
            <th scope="col">Role</th>
            <th scope="col" className="detail-files__col-size">Size</th>
            <th scope="col">Status</th>
          </tr>
        </thead>
        <tbody>
          {files.map((file, idx) => (
            <tr key={`${file.name}-${idx}`}>
              <td className="detail-files__name">
                <Icon name="file" size={14} aria-hidden="true" />
                <span title={file.name}>{file.name}</span>
              </td>
              <td>
                <span className="detail-files__role-badge">{roleLabel(file.role)}</span>
              </td>
              <td className="detail-files__col-size">{fmtBytes(file.size_bytes)}</td>
              <td>
                {file.exists ? (
                  <span className="detail-files__status detail-files__status--present">
                    <Icon name="check" size={14} aria-hidden="true" />
                    <span>Downloaded</span>
                  </span>
                ) : (
                  <span className="detail-files__status detail-files__status--missing">
                    <Icon name="download" size={14} aria-hidden="true" />
                    <span>Not downloaded</span>
                  </span>
                )}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
};

const COLLECTION_ROLE_LABELS: Record<string, string> = {
  llm: 'Planner LLM',
  vision: 'Vision',
  image: 'Image generation',
  edit: 'Image editing',
  transcription: 'Transcription',
  speech: 'Text to speech',
};

const CustomCollectionSettingsTab: React.FC<{ model: ModelInfo; onEdit?: (model: ModelInfo) => void }> = ({ model, onEdit }) => {
  const roles = ((model as any).component_roles || {}) as Record<string, string>;
  const components = getCollectionComponents(model);
  const displayRoles: Record<string, string> = { ...roles, llm: roles.llm || components[0] || '' };
  const assigned = new Set(Object.values(displayRoles).filter(Boolean));
  const unassigned = components.filter(component => !assigned.has(component));
  const tools = Array.isArray((model as any).custom_tools) ? (model as any).custom_tools as Array<Record<string, unknown>> : [];
  const hasCustomPrompt = Boolean(String((model as any).system_prompt || '').trim());

  return (
    <div className="detail-tab-content custom-collection-settings">
      <div className="custom-collection-settings__intro">
        <div>
          <h3>Collection settings</h3>
          <p>Components stay editable after the collection has been saved or downloaded.</p>
        </div>
        {onEdit && (
          <button type="button" className="btn btn--primary btn--sm" onClick={() => onEdit(model)}>
            <Icon name="edit" size={13} /> Edit settings
          </button>
        )}
      </div>

      <section className="custom-collection-settings__section" aria-label="Collection components">
        <h4>Components</h4>
        <div className="custom-collection-settings__components">
          {Object.entries(COLLECTION_ROLE_LABELS).map(([role, label]) => (
            <div className="custom-collection-settings__component" key={role}>
              <span>{label}</span>
              <strong>{displayRoles[role] || 'Not set'}</strong>
            </div>
          ))}
          {unassigned.map(component => (
            <div className="custom-collection-settings__component" key={component}>
              <span>Tool model</span>
              <strong>{component}</strong>
            </div>
          ))}
        </div>
      </section>

      <section className="custom-collection-settings__section" aria-label="Advanced collection settings summary">
        <h4>Advanced</h4>
        <div className="custom-collection-settings__summary">
          <span>System prompt</span>
          <strong>{hasCustomPrompt ? 'Customized' : 'Default'}</strong>
          <span>Custom LLM tools</span>
          <strong>{tools.length}</strong>
        </div>
      </section>
    </div>
  );
};

/* ── ModelDetailPanel ────────────────────────────────────────── */

export interface ModelDetailPanelProps {
  model: ModelInfo | null;
  loadedModel: LoadedModel | null;
  loadingModel: string | null;
  pulling: Record<string, number>;
  loadError: { modelName: string; message: string } | null;
  onLoad: (model: ModelInfo) => void;
  onUnload: (model: LoadedModel) => void;
  /**
   * Reload an already-loaded model so a *load-time* preset change takes effect
   * (#2356). This is the only server round-trip in the simplified design: it is
   * literally an unload + load (see `api.reloadModel`). Live (request-time)
   * changes do NOT call this — rebinding the active preset is the whole live op.
   * Resolves once the reload completes.
   */
  onReloadModel?: (
    model: LoadedModel,
    recipeOptions?: Record<string, unknown>,
  ) => Promise<void>;
  onPull: (model: ModelInfo) => void;
  onPullAndLoad: (model: ModelInfo) => void;
  onDelete: (model: ModelInfo) => void;
  onCancelPull: (name: string) => void;
  serverDefaultCtxSize: number;
  /** Whether this model is currently marked a favorite (client-local pin store). */
  isFavorite?: boolean;
  /** Toggle this model's favorite/pin state. Receives the model name. */
  onToggleFavorite?: (name: string) => void;
  /** Open the persistent settings editor for a saved custom Omni collection. */
  onEditCustomCollection?: (model: ModelInfo) => void;
  /** Called when the "Back to models" button is clicked (narrow viewports). */
  onBack?: () => void;
  /** Called when the close button is clicked to dismiss the detail panel. */
  onClose?: () => void;
  /** True when the registry has no models at all (empty state guidance differs
      from the normal "nothing selected yet" copy). */
  noModelsAvailable?: boolean;
  /** HuggingFace model to show in the detail panel (alternative to a local model). */
  hfModel?: HFModelResult | null;
  /** Pre-fetched variant data for the selected HF model. */
  hfVariants?: PullVariantsResult;
  /** Trigger fetching variants for an HF model id. */
  onFetchHfVariants?: (hfId: string) => void;
  /** Initiate a pull of a specific HF variant. */
  onHfPull?: (hfId: string, variantName: string, recipe: string) => void;
  /** Current pull progress per HF model id (0-100). */
  pullingHf?: Record<string, number>;
  /** Cancel an in-progress HF download. */
  onCancelHfPull?: (hfId: string) => void;
}

type DetailTab = 'settings' | 'readme' | 'presets' | 'tuning' | 'files';

const TABS: Array<{ id: DetailTab; label: string }> = [
  { id: 'readme', label: 'README' },
  { id: 'presets', label: 'Presets' },
  { id: 'tuning', label: 'Model Tuning' },
  { id: 'files', label: 'Files' },
];

const CUSTOM_COLLECTION_TABS: Array<{ id: DetailTab; label: string }> = [
  { id: 'settings', label: 'Settings' },
  { id: 'files', label: 'Files' },
];

export const ModelDetailPanel: React.FC<ModelDetailPanelProps> = ({
  model,
  loadedModel,
  loadingModel,
  pulling,
  loadError,
  onLoad,
  onUnload,
  onReloadModel,
  onPull,
  onPullAndLoad,
  onDelete,
  onCancelPull,
  serverDefaultCtxSize,
  isFavorite = false,
  onToggleFavorite,
  onEditCustomCollection,
  onBack,
  onClose,
  noModelsAvailable = false,
  hfModel,
  hfVariants,
  onFetchHfVariants,
  onHfPull,
  pullingHf,
  onCancelHfPull,
}) => {
  const [activeTab, setActiveTab] = useState<DetailTab>('readme');
  const tabRefs = useRef<Array<HTMLButtonElement | null>>([]);
  const panelHeadingRef = useRef<HTMLHeadingElement>(null);
  const updateBtnRef = useRef<HTMLButtonElement>(null);
  const unloadBtnRef = useRef<HTMLButtonElement>(null);

  // ── Update-preset-while-loaded state (#2356) ──────────────────────────────
  // storeTick forces a re-read of the applied/running preset stores whenever
  // they change (e.g. the user re-links a preset in the Presets tab).
  const [storeTick, setStoreTick] = useState(0);
  type UpdatePhase = 'idle' | 'live' | 'reload' | 'done-live' | 'done-reload' | 'error';
  const [updateStatus, setUpdateStatus] = useState<{ phase: UpdatePhase; msg: string }>({ phase: 'idle', msg: '' });
  const [focusUnloadAfterPresetUpdate, setFocusUnloadAfterPresetUpdate] = useState(false);

  const detailName = model ? mdName(model) : '';
  const detailLoaded = !!loadedModel;
  const isCustomCollection = Boolean(model && (model as any).custom && isCollectionModel(model));
  const detailTabs = isCustomCollection ? CUSTOM_COLLECTION_TABS : TABS;

  // Move focus to heading when model changes
  useEffect(() => {
    if (model) panelHeadingRef.current?.focus();
    setActiveTab(isCustomCollection ? 'settings' : 'readme');
  }, [detailName, isCustomCollection]);

  // Re-render when the preset store changes (applied/running/user presets).
  useEffect(() => {
    const handler = () => setStoreTick(t => t + 1);
    window.addEventListener(PRESET_STORE_EVENT, handler);
    return () => window.removeEventListener(PRESET_STORE_EVENT, handler);
  }, []);

  // Snapshot the running preset when a model becomes loaded; clear it when it
  // unloads. The snapshot baseline = the preset linked at the moment we first
  // observe the model loaded, so later re-links diverge and surface "Update preset".
  useEffect(() => {
    if (!detailName) return;
    if (detailLoaded) {
      if (runningPresetIdForModel(detailName) === undefined) {
        setRunningPreset(detailName, activePresetForModel(detailName).id);
      }
    } else if (runningPresetIdForModel(detailName) !== undefined) {
      clearRunningPreset(detailName);
    }
  }, [detailName, detailLoaded, storeTick]);

  // Reset transient update feedback when the selected model changes.
  useEffect(() => { setUpdateStatus({ phase: 'idle', msg: '' }); }, [detailName]);

  // Auto-dismiss terminal update messages so the live region settles.
  useEffect(() => {
    if (['done-live', 'done-reload', 'error'].includes(updateStatus.phase)) {
      const t = window.setTimeout(() => setUpdateStatus({ phase: 'idle', msg: '' }), 6000);
      return () => window.clearTimeout(t);
    }
  }, [updateStatus]);

  // After applying a preset, the Apply/Reload button is removed from the DOM.
  // Keep keyboard focus inside the actions group by focusing the current Unload
  // button only after React and any model-refresh side effects have settled.
  useEffect(() => {
    if (!focusUnloadAfterPresetUpdate || !detailName || !detailLoaded) return;

    let raf1 = 0;
    let raf2 = 0;
    let retryTimer = 0;
    const deadline = window.performance.now() + 1000;

    const tryFocusUnload = (): boolean => {
      const btn = unloadBtnRef.current;
      if (!btn || btn.disabled || !document.contains(btn)) return false;
      btn.focus();
      setFocusUnloadAfterPresetUpdate(false);
      return true;
    };

    const retryUntilReady = () => {
      if (tryFocusUnload()) return;
      if (window.performance.now() < deadline) {
        retryTimer = window.setTimeout(retryUntilReady, 50);
      } else {
        setFocusUnloadAfterPresetUpdate(false);
      }
    };

    raf1 = window.requestAnimationFrame(() => {
      if (tryFocusUnload()) return;
      raf2 = window.requestAnimationFrame(retryUntilReady);
    });

    return () => {
      if (raf1) window.cancelAnimationFrame(raf1);
      if (raf2) window.cancelAnimationFrame(raf2);
      if (retryTimer) window.clearTimeout(retryTimer);
    };
  }, [focusUnloadAfterPresetUpdate, detailName, detailLoaded, updateStatus.phase]);

  const handleUpdatePreset = useCallback(async () => {
    if (!model || !loadedModel) return;
    const targetName = mdName(model);
    const linked = activePresetForModel(targetName);
    const runId = runningPresetIdForModel(targetName);
    const running = runId ? (allStoredPresets().find(p => p.id === runId) ?? null) : null;
    const kind = classifyPresetChange(running, linked);
    if (kind === 'none') return;

    if (kind === 'live') {
      // Live (request-time) change: rebinding the active preset IS the whole
      // operation. Nothing is POSTed — request composition (`samplingForModel`
      // in api.ts, `systemPromptTextForPreset` in ChatView) carries the new
      // sampling / system_prompt / tools on the next generation request. We
      // record the new running preset so the affordance clears.
      setRunningPreset(targetName, linked.id);
      setUpdateStatus({ phase: 'done-live', msg: `Preset updated to “${linked.name}” — applied live, no reload needed.` });
      setFocusUnloadAfterPresetUpdate(true);
    } else {
      // Load-time change: a real reload (unload + load) is required. The
      // active-preset binding PERSISTS across the reload — `linked` is already
      // the active preset, so the reloaded model comes up running it; we then
      // snapshot it as the running preset. (Assumption flagged to @fl0rianr.)
      setUpdateStatus({ phase: 'reload', msg: `Reloading ${targetName} with preset “${linked.name}”…` });
      try {
        await onReloadModel?.(loadedModel, linked.recipe_options as Record<string, unknown> | undefined);
        setRunningPreset(targetName, linked.id);
        setUpdateStatus({ phase: 'done-reload', msg: `Preset updated to “${linked.name}” — model reloaded.` });
        setFocusUnloadAfterPresetUpdate(true);
      } catch {
        setUpdateStatus({ phase: 'error', msg: `Couldn’t reload ${targetName} with the new preset. Please try again.` });
        requestAnimationFrame(() => updateBtnRef.current?.focus());
      }
    }
  }, [model, loadedModel, onReloadModel]);

  // Roving tabindex: keyboard navigation across tabs
  const handleTabKeyDown = (e: React.KeyboardEvent<HTMLButtonElement>, index: number) => {
    const count = detailTabs.length;
    let next = -1;
    if (e.key === 'ArrowRight') { e.preventDefault(); next = (index + 1) % count; }
    else if (e.key === 'ArrowLeft') { e.preventDefault(); next = (index - 1 + count) % count; }
    else if (e.key === 'Home') { e.preventDefault(); next = 0; }
    else if (e.key === 'End') { e.preventDefault(); next = count - 1; }
    if (next >= 0) {
      setActiveTab(detailTabs[next].id);
      tabRefs.current[next]?.focus();
    }
  };

  if (!model && hfModel) {
    return (
      <HfDetailView
        hfModel={hfModel}
        hfVariants={hfVariants}
        onFetchHfVariants={onFetchHfVariants}
        onHfPull={onHfPull}
        pullingHf={pullingHf}
        onCancelHfPull={onCancelHfPull}
        onBack={onBack}
        onClose={onClose}
      />
    );
  }

  if (!model) {
    return (
      <div className="model-detail-panel model-detail-panel--empty" aria-label="Model detail">
        {onBack && (
          <button
            type="button"
            className="model-detail-panel__back-btn"
            onClick={onBack}
            aria-label="Back to models list"
          >
            ← Back to models
          </button>
        )}
        <div className="model-detail-panel__placeholder">
          <Icon name="model" size={40} aria-hidden="true" />
          <p>{noModelsAvailable ? 'No models found' : 'No model selected'}</p>
          <p className="model-detail-panel__placeholder-sub">
            {noModelsAvailable
              ? 'No models are available in the registry yet. Pull a model or adjust your filters to get started.'
              : 'Select a model from the list to view its details.'}
          </p>
        </div>
      </div>
    );
  }

  const name = mdName(model);
  const recipe = String((model as any).recipe || '');
  const checkpoint = String((model as any).checkpoint || '');
  const checkpoints = (model as any).checkpoints as Record<string, string> | null ?? null;
  const hfRepo = deriveHFRepo(checkpoint || null, checkpoints);
  const isLoaded = !!loadedModel;
  const isLoadingThis = loadingModel === name;
  const isPulling = pulling[name] !== undefined;
  const pullPct = pulling[name] ?? 0;
  const isDownloaded = Boolean((model as any).downloaded);
  const cap = capabilityFromModelInfo(model);

  // ── Update-preset-while-loaded derivation (#2356) ─────────────────────────
  // Reference storeTick so this recomputes when the preset store changes.
  void storeTick;
  const linkedPreset = activePresetForModel(name);
  const runningPresetId = isLoaded ? runningPresetIdForModel(name) : undefined;
  const runningPreset = runningPresetId
    ? (allStoredPresets().find(p => p.id === runningPresetId) ?? null)
    : null;
  const presetChangeKind: PresetChangeKind = isLoaded && runningPreset
    ? classifyPresetChange(runningPreset, linkedPreset)
    : 'none';
  const isUpdatingPreset = updateStatus.phase === 'live' || updateStatus.phase === 'reload';
  const canUpdatePreset = isLoaded && presetChangeKind !== 'none' && !isUpdatingPreset && !isLoadingThis;

  return (
    <div className="model-detail-panel" role="region" aria-label={`Model details: ${name}`}>

      {/* Close button */}
      {onClose && (
        <button
          type="button"
          className="model-detail-panel__close-btn"
          onClick={onClose}
          aria-label="Close detail panel"
        >
          ×
        </button>
      )}

      {/* Back button for narrow viewports */}
      {onBack && (
        <button
          type="button"
          className="model-detail-panel__back-btn"
          onClick={onBack}
          aria-label="Back to models list"
        >
          ← Back to models
        </button>
      )}

      {/* Header */}
      <div className="model-detail-panel__head">
        <h2
          className="model-detail-panel__name"
          ref={panelHeadingRef}
          tabIndex={-1}
          id="detail-panel-heading"
        >
          {model.display_name || name}
        </h2>

        {/* Metadata row */}
        <div className="model-detail-panel__meta">
          {recipe && (
            <span className="model-detail-panel__badge model-detail-panel__badge--recipe">
              {recipeDisplayLabel(recipe)}
            </span>
          )}
          {model.size != null && model.size > 0 && (
            <span className="model-detail-panel__badge">{fmtSize(model.size)}</span>
          )}
          {cap && (
            <span className="model-detail-panel__badge model-detail-panel__badge--cap">
              <CapabilityIcon capability={cap} size={11} />
              {capabilityLabel(cap)}
            </span>
          )}
          {isLoaded && (
            <span className="model-detail-panel__status model-detail-panel__status--running">
              <span className="row__pulse" aria-hidden="true" /> Running
            </span>
          )}
          {isDownloaded && !isLoaded && (
            <span className="model-detail-panel__status model-detail-panel__status--ready">Ready</span>
          )}
        </div>

        {/* Primary actions */}
        <div className="model-detail-panel__actions" aria-label={`Actions for ${name}`}>
          {onToggleFavorite && (
            <button
              type="button"
              className={`model-detail-panel__fav-btn${isFavorite ? ' model-detail-panel__fav-btn--on' : ''}`}
              onClick={() => onToggleFavorite(name)}
              aria-pressed={isFavorite}
              aria-label={isFavorite ? `Remove ${name} from favorites` : `Add ${name} to favorites`}
              title={isFavorite ? 'Remove from favorites' : 'Add to favorites'}
            >
              <span aria-hidden="true" className="model-detail-panel__fav-icon">{isFavorite ? '★' : '☆'}</span>
            </button>
          )}
          {isPulling ? (
            <>
              <div className="row__progress">
                <div className="row__progress-bar">
                  <div className="row__progress-fill" style={{ width: `${pullPct}%` }} />
                </div>
                <span className="row__progress-text">{pullPct.toFixed(0)}%</span>
              </div>
              <button
                type="button"
                className="btn btn--ghost btn--sm"
                onClick={() => onCancelPull(name)}
                aria-label={`Cancel download of ${name}`}
              >
                Cancel
              </button>
            </>
          ) : isLoaded ? (
            <>
              {/* Update preset (#2356): appears next to Unload when a different
                  preset has been linked to this loaded model. */}
              {(canUpdatePreset || isUpdatingPreset) && (
                <button
                  ref={updateBtnRef}
                  type="button"
                  className="btn btn--primary btn--sm model-detail-panel__update-preset-btn"
                  onClick={handleUpdatePreset}
                  disabled={isUpdatingPreset || !canUpdatePreset}
                  aria-busy={isUpdatingPreset}
                  aria-label={
                    isUpdatingPreset
                      ? (updateStatus.phase === 'reload' ? `Reloading ${name} with new preset…` : `Applying preset for ${name}…`)
                      : (presetChangeKind === 'reload'
                        ? `Reload ${name} to apply preset`
                        : `Apply preset for ${name}`)
                  }
                >
                  <Icon name="rotate-ccw" size={13} aria-hidden="true" />{' '}
                  {isUpdatingPreset
                    ? (updateStatus.phase === 'reload' ? 'Reloading…' : 'Applying…')
                    : (presetChangeKind === 'reload' ? 'Reload to apply preset' : 'Apply preset')}
                </button>
              )}
              <button
                ref={unloadBtnRef}
                type="button"
                className="btn btn--ghost btn--sm"
                onClick={() => onUnload(loadedModel!)}
                disabled={isLoadingThis || isUpdatingPreset}
                aria-label={isLoadingThis ? `Working on ${name}…` : `Unload ${name}`}
              >
                {isLoadingThis ? 'Working…' : 'Unload'}
              </button>
            </>
          ) : isDownloaded ? (
            <button
              type="button"
              className="btn btn--primary btn--sm"
              onClick={() => onLoad(model)}
              disabled={isLoadingThis}
              aria-label={isLoadingThis ? `Loading ${name}…` : `Load ${name}`}
            >
              {isLoadingThis ? 'Loading…' : <><Icon name="play" size={13} /> Load</>}
            </button>
          ) : (
            <>
              <button
                type="button"
                className="btn btn--ghost btn--sm"
                onClick={() => onPull(model)}
                aria-label={`Download ${name}`}
              >
                <Icon name="download" size={13} /> Download
              </button>
              <button
                type="button"
                className="btn btn--primary btn--sm"
                onClick={() => onPullAndLoad(model)}
                aria-label={`Get and load ${name}`}
              >
                <Icon name="download" size={13} /> Get & Load
              </button>
            </>
          )}
          {(isDownloaded || isLoaded) && (
            <button
              type="button"
              className="btn btn--ghost btn--sm btn--danger"
              onClick={() => onDelete(model)}
              disabled={isLoadingThis}
              aria-label={(model as any).custom ? `Delete custom model definition for ${name}` : `Delete downloaded files for ${name}`}
            >
              <Icon name="trash" size={13} />
            </button>
          )}
        </div>

        {/* Load error */}
        {loadError?.modelName === name && (
          <div className="model-detail-panel__error" role="alert">
            <Icon name="alert" size={13} /> {loadError.message}
          </div>
        )}

        {/* Update-preset feedback + live region (#2356).
            Always-present polite live region so screen readers announce the
            live-apply / reload outcome; a visible pill mirrors it sighted. */}
        <div
          className={`model-detail-panel__preset-update${updateStatus.phase !== 'idle' ? ' model-detail-panel__preset-update--active' : ''}`}
          role="status"
          aria-live="polite"
          aria-atomic="true"
          data-preset-update-phase={updateStatus.phase}
        >
          {updateStatus.phase === 'reload' && (
            <span className="model-detail-panel__preset-update-spinner" aria-hidden="true" />
          )}
          {updateStatus.msg}
        </div>

        {/* Sighted hint explaining why Update preset appeared (not announced —
            the button's accessible name already conveys the reload semantics). */}
        {canUpdatePreset && updateStatus.phase === 'idle' && (
          <p className="model-detail-panel__preset-update-hint" aria-hidden="true">
            {presetChangeKind === 'reload'
              ? 'A different preset is linked. Updating will reload the model to apply it.'
              : 'A different preset is linked. Updating applies it live — no reload needed.'}
          </p>
        )}

        {/* HF link */}
        {hfRepo && (
          <a
            className="model-detail-panel__hf-link"
            href={`https://huggingface.co/${hfRepo}`}
            target="_blank"
            rel="noopener noreferrer"
            aria-label={`View ${name} on Hugging Face (opens in new tab)`}
          >
            <Icon name="globe" size={12} /> Hugging Face
          </a>
        )}
      </div>

      {/* Tablist */}
      <div
        className="detail-tabs__tablist"
        role="tablist"
        aria-label="Model details sections"
        aria-labelledby="detail-panel-heading"
      >
        {detailTabs.map((tab, i) => (
          <button
            key={tab.id}
            ref={el => { tabRefs.current[i] = el; }}
            role="tab"
            id={`detail-tab-${tab.id}`}
            aria-selected={activeTab === tab.id}
            aria-controls={`detail-panel-${tab.id}`}
            tabIndex={activeTab === tab.id ? 0 : -1}
            className={`detail-tabs__tab${activeTab === tab.id ? ' detail-tabs__tab--active' : ''}`}
            onClick={() => setActiveTab(tab.id)}
            onKeyDown={e => handleTabKeyDown(e, i)}
          >
            {tab.label}
          </button>
        ))}
      </div>

      {/* Tab panels */}
      {detailTabs.map(tab => (
        <div
          key={tab.id}
          id={`detail-panel-${tab.id}`}
          role="tabpanel"
          aria-labelledby={`detail-tab-${tab.id}`}
          className={`detail-tabs__panel${activeTab === tab.id ? ' detail-tabs__panel--active' : ''}`}
          hidden={activeTab !== tab.id}
        >
          {tab.id === 'settings' && model && (
            <CustomCollectionSettingsTab model={model} onEdit={onEditCustomCollection} />
          )}
          {tab.id === 'readme' && (
            <ModelReadmeTab model={model} isActive={activeTab === 'readme'} />
          )}
          {tab.id === 'presets' && (
            <ModelPresetsTab model={model} isActive={activeTab === 'presets'} />
          )}
          {tab.id === 'tuning' && (
            <ModelTuningTab
              model={model}
              loadedModel={loadedModel}
              isActive={activeTab === 'tuning'}
              serverDefaultCtxSize={serverDefaultCtxSize}
              onReloadModel={onReloadModel}
            />
          )}
          {tab.id === 'files' && (
            <ModelFilesTab model={model} isActive={activeTab === 'files'} />
          )}
        </div>
      ))}
    </div>
  );
};

export default ModelDetailPanel;
