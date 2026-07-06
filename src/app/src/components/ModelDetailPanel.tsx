/**
 * ModelDetailPanel — right-side detail view for the selected model.
 * Contains: header (title, metadata, primary actions) + tablist (README / Presets / Model Tuning / Files).
 *
 * Part of the master-detail layout introduced in #2355 Slice 1.
 */
import React, { useState, useEffect, useMemo, useRef, useCallback } from 'react';
import MarkdownIt from 'markdown-it';
import DOMPurify from 'dompurify';
import type { ModelInfo, LoadedModel, ModelFileInfo } from '../api';
import api from '../api';
import { capabilityFromModelInfo, capabilityLabel } from '../modelCapabilities';
import {
  DEFAULT_PRESET, PRESET_STORE_EVENT, Preset, PresetChangeKind,
  allStoredPresets, isCompatible, loadApplied, saveApplied,
  effectivePresetParamPreviewLines, activePresetForModel,
  runningPresetIdForModel, setRunningPreset, clearRunningPreset,
  classifyPresetChange,
  effectiveModelTuningForModel, modelBaseTuningForModel, loadModelTuning,
  saveModelTuning, resetModelTuning, sanitizeRecipeOptions, sanitizeSamplingParams,
  type RecipeOptions, type SamplingParams,
} from '../presetStore';
import { Icon, CapabilityIcon, PresetIcon } from './Icon';

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
  vllm_backend: 'Backend',
  vllm_args: 'Backend args',
  flm_args: 'Backend args',
  voice: 'Voice',
  speed: 'Speed',
  merge_args: 'Backend args behavior',
};

const TUNING_FIELD_HINTS: Partial<Record<keyof RecipeOptions, string>> = {
  ctx_size: 'Runtime context window for this exact model.',
  llamacpp_backend: 'Backend for this model recipe. Switching back restores the last draft args for that backend in this browser session.',
  vllm_backend: 'Backend for this model recipe. Switching back restores the last draft args for that backend in this browser session.',
  whispercpp_backend: 'Backend for this model recipe. Switching back restores the last draft args for that backend in this browser session.',
  moonshine_backend: 'Backend for this model recipe. Switching back restores the last draft args for that backend in this browser session.',
  llamacpp_device: 'Optional device selector for the selected backend.',
  llamacpp_args: 'Raw backend args for this model and selected backend only.',
  sdcpp_args: 'Raw backend args for this image model only.',
  whispercpp_args: 'Raw backend args for this transcription model only.',
  moonshine_args: 'Raw backend args for this transcription model only.',
  vllm_args: 'Raw backend args for this model only.',
  flm_args: 'Raw backend args for this model only.',
  merge_args: 'Choose whether backend defaults, model args, or both should be used for this model.',
};

const NUMERIC_TUNING_KEYS = new Set<keyof RecipeOptions>(['ctx_size', 'steps', 'cfg_scale', 'width', 'height', 'flow_shift', 'speed']);
const BOOLEAN_TUNING_KEYS = new Set<keyof RecipeOptions>(['merge_args']);
const BACKEND_TUNING_KEYS = new Set<keyof RecipeOptions>(['llamacpp_backend', 'vllm_backend', 'whispercpp_backend', 'moonshine_backend']);
const DEVICE_TUNING_KEYS = new Set<keyof RecipeOptions>(['llamacpp_device']);
const ARGS_TUNING_KEYS = new Set<keyof RecipeOptions>(['llamacpp_args', 'sdcpp_args', 'whispercpp_args', 'moonshine_args', 'vllm_args', 'flm_args']);
const BACKEND_ARGS_KEY: Partial<Record<keyof RecipeOptions, keyof RecipeOptions>> = {
  llamacpp_backend: 'llamacpp_args',
  vllm_backend: 'vllm_args',
  whispercpp_backend: 'whispercpp_args',
  moonshine_backend: 'moonshine_args',
};

const LLAMACPP_RECIPE_KEYS: Array<keyof RecipeOptions> = ['ctx_size', 'llamacpp_backend', 'llamacpp_device', 'llamacpp_args', 'merge_args'];
const VLLM_RECIPE_KEYS: Array<keyof RecipeOptions> = ['ctx_size', 'vllm_backend', 'vllm_args', 'merge_args'];
const FLM_RECIPE_KEYS: Array<keyof RecipeOptions> = ['ctx_size', 'flm_args', 'merge_args'];
const RYZENAI_RECIPE_KEYS: Array<keyof RecipeOptions> = ['ctx_size', 'merge_args'];
const IMAGE_RECIPE_KEYS: Array<keyof RecipeOptions> = ['steps', 'cfg_scale', 'width', 'height', 'sampling_method', 'flow_shift', 'sdcpp_args', 'merge_args'];
const WHISPER_RECIPE_KEYS: Array<keyof RecipeOptions> = ['whispercpp_backend', 'whispercpp_args', 'merge_args'];
const MOONSHINE_RECIPE_KEYS: Array<keyof RecipeOptions> = ['moonshine_backend', 'moonshine_args', 'merge_args'];
const TTS_RECIPE_KEYS: Array<keyof RecipeOptions> = ['voice', 'speed', 'merge_args'];

const CONTEXT_OPTIONS = [1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576];

function formatContextSize(value: number): string {
  if (!Number.isFinite(value) || value <= 0) return 'auto';
  if (value >= 1024 && value % 1024 === 0) return `${Math.round(value / 1024)}K`;
  return value.toLocaleString();
}

function contextOptionsFor(...values: Array<number | undefined>): number[] {
  const maxInput = Math.max(0, ...values.filter((v): v is number => typeof v === 'number' && Number.isFinite(v) && v > 0));
  const max = Math.max(262144, maxInput);
  const options = CONTEXT_OPTIONS.filter(v => v <= max || v === CONTEXT_OPTIONS[0]);
  for (const value of values) {
    if (typeof value === 'number' && Number.isFinite(value) && value > 0 && !options.includes(Math.round(value))) options.push(Math.round(value));
  }
  return [...new Set(options)].sort((a, b) => a - b);
}

function nearestOptionIndex(options: number[], value: number): number {
  let best = 0;
  let bestDelta = Number.POSITIVE_INFINITY;
  options.forEach((option, index) => {
    const delta = Math.abs(option - value);
    if (delta < bestDelta) { best = index; bestDelta = delta; }
  });
  return best;
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
  else if (cap === 'tts') add(TTS_RECIPE_KEYS);

  const base = modelBaseTuningForModel(model).recipe_options;
  Object.keys(base).forEach(key => set.add(key as keyof RecipeOptions));
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

/* ── Presets tab ─────────────────────────────────────────────── */

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

  const userTuning = useMemo(() => loadModelTuning(name), [name, storeVersion]);
  const baseTuning = useMemo(() => modelBaseTuningForModel(model, serverDefaultCtxSize), [model, serverDefaultCtxSize]);
  const effectiveTuning = useMemo(() => effectiveModelTuningForModel(name, model, serverDefaultCtxSize), [name, model, serverDefaultCtxSize, storeVersion]);
  const recipeKeys = useMemo(() => tuningKeysForModel(model), [model]);
  const activeArgsKey = useMemo(() => recipeKeys.find(key => ARGS_TUNING_KEYS.has(key)) as keyof RecipeOptions | undefined, [recipeKeys]);
  const allowSampling = samplingAllowedForModel(model);

  const [recipeDraft, setRecipeDraft] = useState<Record<string, string>>({});
  const [samplingDraft, setSamplingDraft] = useState<Record<string, string>>({});
  const [backendArgsDrafts, setBackendArgsDrafts] = useState<Record<string, string>>({});

  useEffect(() => {
    const nextUser = loadModelTuning(name);
    const nextRecipe: Record<string, string> = {};
    for (const [key, value] of Object.entries(nextUser?.recipe_options || {})) nextRecipe[key] = fieldValue(value);
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
    setRecipeDraft(nextRecipe);
    setSamplingDraft(nextSampling);
    setBackendArgsDrafts(nextArgMemory);
    setNotice(null);
  }, [name, storeVersion]);

  if (!isActive) return null;

  const recipes = recipesForDisplay(model);
  const cap = capabilityFromModelInfo(model);
  const linkedPreset = activePresetForModel(name);
  const hasUserTuning = !!userTuning && (
    Object.keys(userTuning.recipe_options).length > 0 ||
    Object.keys(userTuning.sampling).length > 0 ||
    !!userTuning.engine_hint
  );
  const hasDraftValues = Object.values(recipeDraft).some(value => value.trim()) || Object.values(samplingDraft).some(value => value.trim());

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
      if (!value.trim()) continue;
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

  const buildSampling = (): SamplingParams => {
    const raw: Partial<SamplingParams> = {};
    for (const key of ['temperature', 'top_p', 'top_k', 'repeat_penalty'] as Array<keyof SamplingParams>) {
      const n = parseNumberOrUndefined(samplingDraft[key] || '');
      if (n !== undefined) raw[key] = n;
    }
    return sanitizeSamplingParams(raw);
  };

  const saveDraft = () => {
    saveModelTuning(name, { recipe_options: buildRecipeOptions(), sampling: buildSampling() });
    setNotice('Model tuning saved. Sampling applies to the next request; runtime fields apply on next load or reload.');
  };

  const resetDraft = () => {
    resetModelTuning(name);
    setRecipeDraft({});
    setSamplingDraft({});
    setBackendArgsDrafts({});
    setNotice('Model tuning reset.');
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

  const renderRecipeField = (key: keyof RecipeOptions) => {
    const baseValue = baseTuning.recipe_options[key];
    const draftValue = recipeDraft[key] || '';
    const inputId = `tuning-${name}-${key}`.replace(/[^a-zA-Z0-9_-]/g, '-');
    const label = TUNING_FIELD_LABELS[key] || key;
    const hint = TUNING_FIELD_HINTS[key];

    if (key === 'ctx_size') {
      const baseNumber = parseNumberOrUndefined(fieldValue(baseValue)) || serverDefaultCtxSize || 4096;
      const draftNumber = parseNumberOrUndefined(draftValue);
      const currentValue = draftNumber || baseNumber;
      const options = contextOptionsFor(baseNumber, serverDefaultCtxSize, draftNumber);
      const sliderIndex = nearestOptionIndex(options, currentValue);
      const sliderValue = options[sliderIndex] || currentValue;
      return (
        <label key={key} className="detail-tuning__field detail-tuning__field--wide" htmlFor={inputId}>
          <span>{label}</span>
          <div className="field__row detail-tuning__control-row">
            <input
              id={inputId}
              className="slider"
              type="range"
              min={0}
              max={Math.max(0, options.length - 1)}
              step={1}
              value={sliderIndex}
              onChange={e => setRecipeField(key, String(options[Number(e.target.value)] || sliderValue))}
            />
            <span className="field__value">{formatContextSize(sliderValue)}</span>
            {renderClearOverrideButton(() => clearRecipeField(key), !draftValue)}
          </div>
          <small>{draftValue ? `Override: ${Number(draftValue).toLocaleString()} tokens` : `Current: ${formatContextSize(baseNumber)}`}</small>
        </label>
      );
    }

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
          Presets describe the intent. Model Tuning is the per-model runtime layer that implements that intent using values from the built-in definition or GGUF-derived metadata, with optional local overrides for this model only.
        </p>
      </section>

      <section className="detail-tuning__summary" aria-label="Effective runtime summary">
        <div className="detail-tuning__summary-card">
          <span className="detail-tuning__summary-label">Preset intent</span>
          <strong>{linkedPreset.name}</strong>
        </div>
        <div className="detail-tuning__summary-card">
          <span className="detail-tuning__summary-label">Capability</span>
          <strong>{capabilityLabel(cap)}</strong>
        </div>
        <div className="detail-tuning__summary-card">
          <span className="detail-tuning__summary-label">Recipe</span>
          <strong>{recipes.length ? recipes.map(recipeDisplayLabel).join(' / ') : 'Auto'}</strong>
        </div>
        <div className="detail-tuning__summary-card">
          <span className="detail-tuning__summary-label">Source</span>
          <strong>{hasUserTuning ? 'Customized for this model' : 'Built-in / GGUF values'}</strong>
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
              </div>
            ))}
            {effectiveSamplingEntries.map(([key, value]) => (
              <div className="detail-tuning__kv" key={`sp-${key}`}>
                <span>{key}</span>
                <code>{tuningValue(value)}</code>
              </div>
            ))}
          </div>
        )}
      </section>

      <section className="detail-tuning__editor" aria-label="Customize model tuning">
        <div className="detail-tuning__section-head">
          <div>
            <h3 className="detail-tuning__section-title">Customize this model</h3>
            <p className="detail-tuning__hint">Leave a field blank to keep the current value shown in the control.</p>
          </div>
          {notice && <p className="detail-tuning__notice">{notice}</p>}
        </div>

        <div className="detail-tuning__field-grid">
          {recipeKeys.map(renderRecipeField)}
        </div>

        {allowSampling && (
          <div className="detail-tuning__sampling">
            <h4>Sampling defaults</h4>
            <div className="detail-tuning__field-grid">
              {(['temperature', 'top_p', 'top_k', 'repeat_penalty'] as Array<keyof SamplingParams>).map(renderSamplingField)}
            </div>
          </div>
        )}

        <div className="detail-tuning__actions">
          <button type="button" className="btn btn--primary btn--sm" onClick={saveDraft}>Save tuning</button>
          <button type="button" className="btn btn--ghost btn--sm" onClick={resetDraft} disabled={!hasUserTuning && !hasDraftValues}>Reset tuning</button>
          {loadedModel && onReloadModel && (
            <button type="button" className="btn btn--ghost btn--sm" onClick={reloadWithTuning} disabled={isReloading} aria-busy={isReloading}>
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
  /** Called when the "Back to models" button is clicked (narrow viewports). */
  onBack?: () => void;
  /** True when the registry has no models at all (empty state guidance differs
      from the normal "nothing selected yet" copy). */
  noModelsAvailable?: boolean;
}

type DetailTab = 'readme' | 'presets' | 'tuning' | 'files';

const TABS: Array<{ id: DetailTab; label: string }> = [
  { id: 'readme', label: 'README' },
  { id: 'presets', label: 'Presets' },
  { id: 'tuning', label: 'Model Tuning' },
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
  onBack,
  noModelsAvailable = false,
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

  // Move focus to heading when model changes
  useEffect(() => {
    if (model) panelHeadingRef.current?.focus();
  }, [model?.id]);

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
  useEffect(() => { setUpdateStatus({ phase: 'idle', msg: '' }); }, [model?.id]);

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
    const count = TABS.length;
    let next = -1;
    if (e.key === 'ArrowRight') { e.preventDefault(); next = (index + 1) % count; }
    else if (e.key === 'ArrowLeft') { e.preventDefault(); next = (index - 1 + count) % count; }
    else if (e.key === 'Home') { e.preventDefault(); next = 0; }
    else if (e.key === 'End') { e.preventDefault(); next = count - 1; }
    if (next >= 0) {
      setActiveTab(TABS[next].id);
      tabRefs.current[next]?.focus();
    }
  };

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
        {TABS.map((tab, i) => (
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
      {TABS.map(tab => (
        <div
          key={tab.id}
          id={`detail-panel-${tab.id}`}
          role="tabpanel"
          aria-labelledby={`detail-tab-${tab.id}`}
          className={`detail-tabs__panel${activeTab === tab.id ? ' detail-tabs__panel--active' : ''}`}
          hidden={activeTab !== tab.id}
        >
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
