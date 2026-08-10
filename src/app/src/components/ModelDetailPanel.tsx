/**
 * ModelDetailPanel — right-side detail view for the selected model.
 * Contains: header (title, metadata, primary actions) + tablist (README / Configuration / Files).
 *
 * Part of the master-detail layout introduced in #2355 Slice 1.
 */
import React, { useState, useEffect, useMemo, useRef, useCallback } from 'react';
import MarkdownIt from 'markdown-it';
import DOMPurify from 'dompurify';
import type { ModelInfo, LoadedModel, ModelFileInfo, HFModelResult, ModelRegistryProvider, PullVariantsResult, CloudProviderRow } from '../api';
import api from '../api';
import { capabilityFromModelInfo, capabilityLabel } from '../modelCapabilities';
import {
  modelBaseTuningForModel, loadModelTuning, saveModelTuning, resetModelTuning,
  sanitizeRecipeOptions,
  type RecipeOptions,
} from '../modelConfiguration';
import { Icon, CapabilityIcon } from './Icon';
import { modelIsCustom } from './ModelListPanel';
import {
  WorkspaceActionButton,
  WorkspaceActionGroup,
  WorkspaceActionLink,
  WorkspaceDetailPanel,
  WorkspaceMetadataChip,
} from './WorkspacePanels';
import { getCollectionComponents, isCollectionModel } from '../features/collections/collectionModels';
import { TTS_VOICES } from '../features/audio/ttsSettings';
import {
  describeRouterModelConnection,
  providerEndpointNeedsInsecureOptIn,
  validateProviderEndpoint,
} from '../features/router/routerConnections';

/* ── Helpers (local copies to keep component self-contained) ──── */

function mdName(m: ModelInfo | null | undefined): string {
  if (!m) return '';
  return String((m as any).model_name ?? m.name ?? m.id ?? '').trim();
}

function isImageOnlyModel(model: ModelInfo | null | undefined): boolean {
  return model ? capabilityFromModelInfo(model) === 'image' : false;
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
    case 'collection.router': return 'Router';
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

interface VoiceOption {
  id: string;
  label: string;
}

function voiceOptionsFromValue(value: unknown): VoiceOption[] {
  const isArray = Array.isArray(value);
  const rawEntries: Array<[string, unknown]> = isArray
    ? value.map((item, index) => [String(index), item])
    : value && typeof value === 'object'
      ? Object.entries(value as Record<string, unknown>)
      : [];

  const options: VoiceOption[] = [];
  for (const [fallbackId, item] of rawEntries) {
    if (typeof item === 'string') {
      const text = item.trim();
      if (!text) continue;
      options.push(isArray ? { id: text, label: text } : { id: fallbackId, label: text });
      continue;
    }
    if (!item || typeof item !== 'object') continue;
    const entry = item as Record<string, unknown>;
    const id = String(entry.id ?? entry.value ?? entry.name ?? fallbackId).trim();
    if (!id) continue;
    const label = String(entry.label ?? entry.display_name ?? entry.name ?? id).trim() || id;
    options.push({ id, label });
  }
  return options;
}

function knownVoiceOptionsForModel(model: ModelInfo): VoiceOption[] {
  const modelRecord = model as Record<string, unknown>;
  const recipes = Array.isArray(model.recipes) ? model.recipes : [];
  const records = [
    modelRecord,
    modelRecord.recipe_options,
    modelRecord.options,
    modelRecord.tts,
    ...recipes,
  ].filter((value): value is Record<string, unknown> => !!value && typeof value === 'object' && !Array.isArray(value));

  const discovered: VoiceOption[] = [];
  for (const record of records) {
    for (const key of ['voices', 'available_voices', 'voice_options']) {
      discovered.push(...voiceOptionsFromValue(record[key]));
    }
  }

  if (discovered.length === 0 && recipesForDisplay(model).includes('kokoro')) {
    discovered.push(...TTS_VOICES);
  }

  const seen = new Set<string>();
  return discovered.filter(option => {
    const normalized = option.id.toLowerCase();
    if (!normalized || seen.has(normalized)) return false;
    seen.add(normalized);
    return true;
  });
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

function stringRecordEqual(left: Record<string, string>, right: Record<string, string>): boolean {
  const leftKeys = Object.keys(left).sort();
  const rightKeys = Object.keys(right).sort();
  if (leftKeys.length !== rightKeys.length) return false;
  return leftKeys.every((key, index) => key === rightKeys[index] && left[key] === right[key]);
}

const TUNING_FIELD_LABELS: Partial<Record<keyof RecipeOptions, string>> = {
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
  'sd-cpp_backend': 'Backend',
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
  'sd-cpp_backend': 'Stable Diffusion accelerator backend for this image model. Switching back restores the last draft args for that backend in this browser session.',
  llamacpp_device: 'Optional device selector for the selected backend.',
  llamacpp_args: 'CLI-style flags for llama-server, applied on Load/Reload. E.g. --gpu-layers 35 --threads 8 --batch-size 512. See llama-server --help for all options.',
  sdcpp_args: 'Raw backend args for this image model only.',
  whispercpp_args: 'Raw backend args for this transcription model only.',
  moonshine_args: 'Raw backend args for this transcription model only.',
  vllm_args: 'Raw backend args for this model only.',
  flm_args: 'Raw backend args for this model only.',
};

const NUMERIC_TUNING_KEYS = new Set<keyof RecipeOptions>(['steps', 'cfg_scale', 'width', 'height', 'flow_shift', 'speed']);
const BOOLEAN_TUNING_KEYS = new Set<keyof RecipeOptions>();
const BACKEND_TUNING_KEYS = new Set<keyof RecipeOptions>(['llamacpp_backend', 'vllm_backend', 'sd-cpp_backend', 'whispercpp_backend', 'moonshine_backend', 'acestep_backend', 'thinksound_backend', 'openmoss_backend', 'trellis_backend']);
const DEVICE_TUNING_KEYS = new Set<keyof RecipeOptions>(['llamacpp_device']);
const ARGS_TUNING_KEYS = new Set<keyof RecipeOptions>(['llamacpp_args', 'sdcpp_args', 'whispercpp_args', 'moonshine_args', 'vllm_args', 'flm_args']);
const BACKEND_ARGS_KEY: Partial<Record<keyof RecipeOptions, keyof RecipeOptions>> = {
  llamacpp_backend: 'llamacpp_args',
  vllm_backend: 'vllm_args',
  'sd-cpp_backend': 'sdcpp_args',
  whispercpp_backend: 'whispercpp_args',
  moonshine_backend: 'moonshine_args',
};

const LLAMACPP_RECIPE_KEYS: Array<keyof RecipeOptions> = ['llamacpp_backend', 'llamacpp_device', 'llamacpp_args'];
const VLLM_RECIPE_KEYS: Array<keyof RecipeOptions> = ['vllm_backend', 'vllm_args'];
const FLM_RECIPE_KEYS: Array<keyof RecipeOptions> = ['flm_args'];
const RYZENAI_RECIPE_KEYS: Array<keyof RecipeOptions> = [];
const IMAGE_RECIPE_KEYS: Array<keyof RecipeOptions> = ['sd-cpp_backend', 'steps', 'cfg_scale', 'width', 'height', 'sampling_method', 'flow_shift', 'sdcpp_args'];
const WHISPER_RECIPE_KEYS: Array<keyof RecipeOptions> = ['whispercpp_backend', 'whispercpp_args'];
const MOONSHINE_RECIPE_KEYS: Array<keyof RecipeOptions> = ['moonshine_backend', 'moonshine_args'];
const TTS_RECIPE_KEYS: Array<keyof RecipeOptions> = ['voice', 'speed'];
const ACESTEP_RECIPE_KEYS: Array<keyof RecipeOptions> = ['acestep_backend'];
const THINKSOUND_RECIPE_KEYS: Array<keyof RecipeOptions> = ['thinksound_backend'];
const OPENMOSS_RECIPE_KEYS: Array<keyof RecipeOptions> = ['openmoss_backend', 'voice', 'speed'];
const TRELLIS_RECIPE_KEYS: Array<keyof RecipeOptions> = ['trellis_backend'];



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
    if (key !== 'ctx_size' && key !== 'merge_args' && key !== 'mmproj_enabled') set.add(key as keyof RecipeOptions);
  });
  set.delete('ctx_size');
  set.delete('merge_args');
  set.delete('mmproj_enabled');
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
    case 'sd-cpp_backend': return 'sd-cpp';
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
  ALLOWED_ATTR: ['class', 'href', 'target', 'rel', 'src', 'srcset', 'alt', 'title', 'width', 'height', 'align', 'colspan', 'rowspan', 'loading', 'decoding', 'hidden', 'aria-hidden', 'data-readme-asset-failed'],
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
const failedReadmeAssets = new Set<string>();

function isExternalReadmeUrl(value: string): boolean {
  return /^(?:[a-z][a-z\d+.-]*:|\/\/|#)/i.test(value);
}

function resolveHfReadmeUrl(repo: string, value: string, kind: 'asset' | 'link'): string {
  const trimmed = String(value || '').trim();
  if (!trimmed || isExternalReadmeUrl(trimmed)) return trimmed;
  // HF READMEs commonly use both `assets/foo.png` and `/assets/foo.png` for
  // repository-local files. A leading slash must therefore not resolve against
  // Lemonade's own web-server root.
  const repoRelative = trimmed.replace(/^\/+/, '');
  const base = kind === 'asset'
    ? `https://huggingface.co/${repo}/resolve/main/`
    : `https://huggingface.co/${repo}/blob/main/`;
  try { return new URL(repoRelative, base).toString(); } catch { return trimmed; }
}

function rewriteReadmeSrcset(repo: string, value: string): string {
  if (/^\s*data:/i.test(value)) return value;
  return String(value || '')
    .split(',')
    .map(candidate => {
      const trimmed = candidate.trim();
      if (!trimmed) return '';
      const match = trimmed.match(/^(\S+)(\s+.+)?$/);
      if (!match) return trimmed;
      const resolved = resolveHfReadmeUrl(repo, match[1], 'asset');
      if (failedReadmeAssets.has(resolved)) return '';
      return `${resolved}${match[2] || ''}`;
    })
    .filter(Boolean)
    .join(', ');
}

function renderHfReadmeHtml(source: string, repo: string): string {
  const sanitized = DOMPurify.sanitize(readmeMd.render(stripFrontmatter(source)), README_PURIFY_CONFIG);
  const parsed = new DOMParser().parseFromString(`<div id="readme-root">${sanitized}</div>`, 'text/html');
  const root = parsed.getElementById('readme-root');
  if (!root) return sanitized;

  root.querySelectorAll<HTMLImageElement>('img').forEach(image => {
    const rawSrc = image.getAttribute('src');
    let assetFailed = false;
    if (rawSrc) {
      const src = resolveHfReadmeUrl(repo, rawSrc, 'asset');
      assetFailed = failedReadmeAssets.has(src);
      if (assetFailed) {
        image.setAttribute('data-readme-asset-failed', 'true');
        image.classList.add('detail-readme__asset--failed');
        const fallback = parsed.createElement('span');
        fallback.className = 'detail-readme__asset-fallback';
        fallback.textContent = image.alt ? `Image unavailable: ${image.alt}` : 'README image unavailable';
        image.insertAdjacentElement('afterend', fallback);
      } else {
        image.setAttribute('src', src);
      }
    }
    const rawSrcset = image.getAttribute('srcset');
    if (rawSrcset && !assetFailed) {
      const srcset = rewriteReadmeSrcset(repo, rawSrcset);
      if (srcset) image.setAttribute('srcset', srcset);
      else image.removeAttribute('srcset');
    }
    image.setAttribute('loading', 'lazy');
    image.setAttribute('decoding', 'async');
  });

  root.querySelectorAll<HTMLSourceElement>('source').forEach(sourceNode => {
    const rawSrc = sourceNode.getAttribute('src');
    if (rawSrc) sourceNode.setAttribute('src', resolveHfReadmeUrl(repo, rawSrc, 'asset'));
    const rawSrcset = sourceNode.getAttribute('srcset');
    if (rawSrcset) {
      const srcset = rewriteReadmeSrcset(repo, rawSrcset);
      if (srcset) sourceNode.setAttribute('srcset', srcset);
      else sourceNode.removeAttribute('srcset');
    }
  });

  root.querySelectorAll<HTMLAnchorElement>('a[href]').forEach(anchor => {
    const rawHref = anchor.getAttribute('href');
    if (!rawHref) return;
    anchor.setAttribute('href', resolveHfReadmeUrl(repo, rawHref, 'link'));
    if (!rawHref.startsWith('#')) {
      anchor.setAttribute('target', '_blank');
      anchor.setAttribute('rel', 'noreferrer noopener');
    }
  });

  return DOMPurify.sanitize(root.innerHTML, README_PURIFY_CONFIG);
}

const ReadmeContent: React.FC<{ readme: string; hfRepo: string }> = ({ readme, hfRepo }) => {
  const ref = useRef<HTMLDivElement>(null);
  const html = useMemo(() => renderHfReadmeHtml(readme, hfRepo), [readme, hfRepo]);

  useEffect(() => {
    const node = ref.current;
    if (!node) return;
    const handleAssetError = (event: Event) => {
      const target = event.target;
      if (!(target instanceof HTMLImageElement)) return;
      if (target.dataset.readmeAssetFailed === 'true') return;
      const failedUrl = target.currentSrc || target.src || target.getAttribute('src') || '';
      if (failedUrl) failedReadmeAssets.add(failedUrl);
      target.dataset.readmeAssetFailed = 'true';
      target.classList.add('detail-readme__asset--failed');
      const fallback = document.createElement('span');
      fallback.className = 'detail-readme__asset-fallback';
      fallback.textContent = target.alt ? `Image unavailable: ${target.alt}` : 'README image unavailable';
      target.insertAdjacentElement('afterend', fallback);
    };
    node.addEventListener('error', handleAssetError, true);
    return () => node.removeEventListener('error', handleAssetError, true);
  }, [html]);

  return <div ref={ref} className="detail-tab-content detail-readme" dangerouslySetInnerHTML={{ __html: html }} />;
};

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

  return <ReadmeContent readme={readme} hfRepo={hfRepo!} />;
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
  return <ReadmeContent readme={readme} hfRepo={hfId} />;
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
          ) : hfVariants.recipe !== 'llamacpp' ? (
            <div className="hf-detail__overview-section">
              <span className="hf-detail__overview-label">Repository download</span>
              <div className="hf-detail__gguf-list">
                <button
                  className="hf-detail__gguf-btn"
                  aria-label={`Download ${hfModel.id}`}
                  disabled={isPulling}
                  onClick={() => onHfPull?.(hfModel.id, '', hfVariants.recipe)}
                >
                  <span className="hf-detail__gguf-name">{hfModel.id}</span>
                  <span className="hf-detail__gguf-action">Download</span>
                </button>
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

const REMOTE_PROVIDER_META: Record<ModelRegistryProvider, { label: string; url: (id: string) => string }> = {
  huggingface: { label: 'Hugging Face', url: id => `https://huggingface.co/${id}` },
  modelscope: { label: 'ModelScope', url: id => `https://modelscope.cn/models/${id}` },
};

const HfDetailView: React.FC<{
  hfModel: HFModelResult;
  provider: ModelRegistryProvider;
  hfVariants?: PullVariantsResult;
  onFetchHfVariants?: (hfId: string) => void;
  onHfPull?: (hfId: string, variantName: string, recipe: string) => void;
  pullingHf?: Record<string, number>;
  onCancelHfPull?: (hfId: string) => void;
  onBack?: () => void;
  onClose?: () => void;
}> = ({ hfModel, provider, hfVariants, onFetchHfVariants, onHfPull, pullingHf, onCancelHfPull, onBack, onClose }) => {
  const [activeTab, setActiveTab] = useState<HfDetailTab>('overview');
  const tabRefs = useRef<Array<HTMLButtonElement | null>>([]);
  const panelHeadingRef = useRef<HTMLHeadingElement>(null);

  const repoName = hfModel.id;
  const providerMeta = REMOTE_PROVIDER_META[provider];
  const remoteDetailTabs = provider === 'huggingface' ? HF_DETAIL_TABS : HF_DETAIL_TABS.filter(tab => tab.id === 'overview');
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
    const count = remoteDetailTabs.length;
    let next = -1;
    if (e.key === 'ArrowRight') { e.preventDefault(); next = (index + 1) % count; }
    else if (e.key === 'ArrowLeft') { e.preventDefault(); next = (index - 1 + count) % count; }
    else if (e.key === 'Home') { e.preventDefault(); next = 0; }
    else if (e.key === 'End') { e.preventDefault(); next = count - 1; }
    if (next >= 0) {
      setActiveTab(remoteDetailTabs[next].id);
      tabRefs.current[next]?.focus();
    }
  };

  return (
    <WorkspaceDetailPanel
      className={`model-detail-panel model-detail-panel--hf model-detail-panel--${provider}`}
      ariaLabel={`${providerMeta.label} model: ${repoName}`}
      title={(
        <h2 className="workspace-detail-panel__title model-detail-panel__name" ref={panelHeadingRef} tabIndex={-1} id="detail-panel-heading">
          {repoName}
        </h2>
      )}
      metadata={(
        <>
          <WorkspaceMetadataChip emphasis="high" tone="accent" icon="cloud">{providerMeta.label}</WorkspaceMetadataChip>
          {pipelineTag && (
            <WorkspaceMetadataChip emphasis="medium" tone="accent" className="model-detail-panel__badge--pipeline">{pipelineTag}</WorkspaceMetadataChip>
          )}
          {hfVariants?.recipe && (
            <WorkspaceMetadataChip emphasis="medium">{recipeDisplayLabel(hfVariants.recipe)}</WorkspaceMetadataChip>
          )}
          <WorkspaceMetadataChip emphasis="low">{fmtDownloads(hfModel.downloads)} downloads</WorkspaceMetadataChip>
          <WorkspaceMetadataChip emphasis="low">{fmtDownloads(hfModel.likes)} likes</WorkspaceMetadataChip>
          {hfModel.createdAt && (
            <WorkspaceMetadataChip emphasis="low">{new Date(hfModel.createdAt).toLocaleDateString()}</WorkspaceMetadataChip>
          )}
          {displayTags.map(tag => <WorkspaceMetadataChip key={tag} emphasis="low">{tag}</WorkspaceMetadataChip>)}
        </>
      )}
      actions={(
        <WorkspaceActionGroup className="model-detail-panel__actions" label={`Actions for ${repoName}`}>
          {isPulling && (
            <>
              <div className="row__progress">
                <div className="row__progress-bar">
                  <div className="row__progress-fill" style={{ width: `${pullPct}%` }} />
                </div>
                <span className="row__progress-text">{pullPct.toFixed(0)}%</span>
              </div>
              {onCancelHfPull && (
                <WorkspaceActionButton appearance="secondary" onClick={() => onCancelHfPull(hfModel.id)} aria-label={`Cancel download of ${repoName}`}>
                  Cancel
                </WorkspaceActionButton>
              )}
            </>
          )}
          <WorkspaceActionLink
            appearance="secondary"
            icon="globe"
            href={providerMeta.url(repoName)}
            target="_blank"
            rel="noopener noreferrer"
            aria-label={`View ${repoName} on ${providerMeta.label} (opens in new tab)`}
          >
            Open on {providerMeta.label}
          </WorkspaceActionLink>
        </WorkspaceActionGroup>
      )}
      onBack={onBack}
      backLabel="Back to models"
      backClassName="model-detail-panel__back-btn"
      onClose={onClose}
      closeClassName="model-detail-panel__close-btn"
    >

      <div
        className="detail-tabs__tablist"
        role="tablist"
        aria-label="Model details sections"
        aria-labelledby="detail-panel-heading"
      >
        {remoteDetailTabs.map((tab, i) => (
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

      {remoteDetailTabs.map(tab => (
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
          {tab.id === 'readme' && provider === 'huggingface' && (
            <HfReadmeTab hfId={hfModel.id} isActive={activeTab === 'readme'} />
          )}
        </div>
      ))}
    </WorkspaceDetailPanel>
  );
};



const ModelConfigurationTab: React.FC<{
  model: ModelInfo;
  loadedModel: LoadedModel | null;
  isActive: boolean;
  serverDefaultCtxSize: number;
  isLoadingThis?: boolean;
  onReloadModel?: (model: LoadedModel, recipeOptions?: Record<string, unknown>) => Promise<void>;
  onDirtyChange?: (dirty: boolean) => void;
}> = ({ model, loadedModel, isActive, serverDefaultCtxSize, isLoadingThis, onReloadModel, onDirtyChange }) => {
  const name = mdName(model);
  const supportsContextSize = capabilityFromModelInfo(model) === 'chat';
  const [notice, setNotice] = useState<string | null>(null);
  const [isReloading, setIsReloading] = useState(false);
  const [systemInfo, setSystemInfo] = useState<Record<string, unknown> | null>(() => api.systemInfoData);


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

  const baseTuning = useMemo(
    () => modelBaseTuningForModel(model, serverDefaultCtxSize),
    [model, serverDefaultCtxSize],
  );
  const recipeKeys = useMemo(() => tuningKeysForModel(model), [model]);
  const knownVoiceOptions = useMemo(() => knownVoiceOptionsForModel(model), [model]);
  const knownVoiceIds = useMemo(() => new Set(knownVoiceOptions.map(option => option.id.toLowerCase())), [knownVoiceOptions]);

  const [ctxSizeDraft, setCtxSizeDraft] = useState(() => {
    const ctxRaw = loadModelTuning(name)?.recipe_options?.ctx_size;
    return ctxRaw != null ? String(ctxRaw) : '-1';
  });
  const [recipeDraft, setRecipeDraft] = useState<Record<string, string>>({});
  const [customVoiceMode, setCustomVoiceMode] = useState(false);

  const loadSettingsDraftFromStore = useCallback(() => {
    const userTuning = loadModelTuning(name);
    const nextRecipe: Record<string, string> = {};
    for (const [key, value] of Object.entries(userTuning?.recipe_options || {})) {
      if (key !== 'ctx_size' && key !== 'merge_args' && key !== 'mmproj_enabled') nextRecipe[key] = fieldValue(value);
    }
    const ctxRaw = userTuning?.recipe_options?.ctx_size;
    return {
      ctxSize: ctxRaw != null ? String(ctxRaw) : '-1',
      recipe: nextRecipe,
    };
  }, [name]);

  const loadFromStore = useCallback(() => {
    const next = loadSettingsDraftFromStore();
    setCtxSizeDraft(next.ctxSize);
    setRecipeDraft(next.recipe);
    const storedVoice = next.recipe.voice || '';
    setCustomVoiceMode(Boolean(storedVoice && !knownVoiceIds.has(storedVoice.toLowerCase())));
  }, [knownVoiceIds, loadSettingsDraftFromStore]);

  useEffect(() => { loadFromStore(); }, [loadFromStore]);
  useEffect(() => { setNotice(null); }, [name]);

  const savedLoadSettings = loadSettingsDraftFromStore();
  const hasLoadSettingChanges = ctxSizeDraft !== savedLoadSettings.ctxSize
    || !stringRecordEqual(recipeDraft, savedLoadSettings.recipe);

  useEffect(() => {
    onDirtyChange?.(hasLoadSettingChanges);
  }, [hasLoadSettingChanges, onDirtyChange]);

  const ctxSizeId = `config-${name}-ctx_size`.replace(/[^a-zA-Z0-9_-]/g, '-');
  const ctxSliderId = `${ctxSizeId}-slider`;
  const info = systemInfo || {};
  const ctxMin = 512;
  const ctxStep = 512;
  const positiveCtxValue = (value: unknown): number | undefined => {
    const n = parseNumberOrUndefined(String(value ?? ''));
    return n !== undefined && n >= ctxMin ? n : undefined;
  };
  const loadedCtxSize = positiveCtxValue(loadedModel?.recipe_options?.ctx_size);
  const baseCtxSize = loadedCtxSize
    ?? positiveCtxValue(baseTuning.recipe_options.ctx_size)
    ?? positiveCtxValue(serverDefaultCtxSize)
    ?? 4096;
  const isAutoTuning = ctxSizeDraft === '-1';
  const currentCtxSize = positiveCtxValue(ctxSizeDraft) ?? baseCtxSize;
  const ctxMax = Math.max(
    ctxMin,
    currentCtxSize,
    Number(model?.max_context_window) || 131072,
  );
  const stepContextSize = (direction: -1 | 1) => {
    if (isAutoTuning) return;
    const nextValue = Math.min(
      ctxMax,
      Math.max(ctxMin, currentCtxSize + direction * ctxStep),
    );
    setCtxSizeDraft(String(nextValue));
  };
  const selectorKeys = recipeKeys.filter(key => BACKEND_TUNING_KEYS.has(key) || DEVICE_TUNING_KEYS.has(key));
  const argsKeys = recipeKeys.filter(key => ARGS_TUNING_KEYS.has(key));
  const otherRecipeKeys = recipeKeys.filter(key => !selectorKeys.includes(key) && !argsKeys.includes(key));

  const buildConfigOptions = (): RecipeOptions => {
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
    const ctxNum = parseNumberOrUndefined(ctxSizeDraft);
    if (supportsContextSize && ctxNum !== undefined) (raw as Record<string, unknown>).ctx_size = ctxNum;
    return sanitizeRecipeOptions(raw);
  };

  const saveConfig = (showNotice = true) => {
    const existingTuning = loadModelTuning(name);
    saveModelTuning(name, {
      ...(existingTuning || {}),
      recipe_options: buildConfigOptions(),
      sampling: existingTuning?.sampling || {},
    });
    if (showNotice) setNotice('Defaults saved for future loads.');
  };

  const resetConfig = () => {
    const existingTuning = loadModelTuning(name);
    if (existingTuning) {
      saveModelTuning(name, {
        ...existingTuning,
        recipe_options: {},
        sampling: existingTuning.sampling || {},
      });
    } else {
      resetModelTuning(name);
    }
    setCtxSizeDraft('-1');
    setRecipeDraft({});
    setCustomVoiceMode(false);
    setNotice('Load settings reset to built-in defaults.');
  };

  const discardConfig = () => {
    loadFromStore();
    onDirtyChange?.(false);
    setNotice('Unsaved load setting changes discarded.');
  };

  const reloadViaPanel = async () => {
    if (!loadedModel || !onReloadModel) return;
    saveConfig(false);
    setIsReloading(true);
    try {
      await onReloadModel(loadedModel, buildConfigOptions() as Record<string, unknown>);
      setNotice('Model reloaded with current configuration.');
    } catch {
      setNotice('Reload failed. Check the server logs.');
    } finally {
      setIsReloading(false);
    }
  };

  const renderConfigRecipeField = (key: keyof RecipeOptions) => {
    const fieldId = `config-${name}-${String(key)}`.replace(/[^a-zA-Z0-9_-]/g, '-');
    const label = TUNING_FIELD_LABELS[key] || String(key);
    const draftValue = recipeDraft[String(key)] || '';
    const baseValue = baseTuning.recipe_options[key];

    if (BACKEND_TUNING_KEYS.has(key)) {
      const activeBackend = activeBackendValue(key, baseValue, model, info);
      const options = backendOptionsForKey(key, draftValue || undefined, model, info).filter(opt => opt !== activeBackend);
      return (
        <label key={String(key)} className="detail-tuning__field detail-configuration__field" htmlFor={fieldId}>
          <span>{label}</span>
          <select
            id={fieldId}
            className="select detail-configuration__select"
            value={draftValue}
            onChange={e => setRecipeDraft(prev => ({ ...prev, [String(key)]: e.target.value }))}
          >
            <option value="">{activeBackend}</option>
            {options.map(opt => <option key={opt} value={opt}>{opt}</option>)}
          </select>
        </label>
      );
    }

    if (DEVICE_TUNING_KEYS.has(key)) {
      const backendKey: keyof RecipeOptions = 'llamacpp_backend';
      const selectedBackend = recipeDraft[String(backendKey)] || activeBackendValue(backendKey, baseTuning.recipe_options[backendKey], model, info);
      const activeDevice = optionalDisplayValue(baseValue) || 'auto';
      const options = deviceOptionsForKey(key, draftValue || undefined, selectedBackend, model, info).filter(opt => opt !== activeDevice);
      return (
        <label key={String(key)} className="detail-tuning__field detail-configuration__field" htmlFor={fieldId}>
          <span>{label}</span>
          <select
            id={fieldId}
            className="select detail-configuration__select"
            value={draftValue}
            onChange={e => setRecipeDraft(prev => ({ ...prev, [String(key)]: e.target.value }))}
          >
            <option value="">{activeDevice}</option>
            {options.map(opt => <option key={opt} value={opt}>{opt}</option>)}
          </select>
        </label>
      );
    }

    if (key === 'voice' && knownVoiceOptions.length > 0) {
      const customVoiceSentinel = '__custom_voice__';
      const isUnknownDraft = Boolean(draftValue && !knownVoiceIds.has(draftValue.toLowerCase()));
      const showCustomVoice = customVoiceMode || isUnknownDraft;
      const defaultVoice = optionalDisplayValue(baseValue);
      return (
        <div key={String(key)} className="detail-tuning__field detail-configuration__field">
          <span id={`${fieldId}-label`}>{label}</span>
          <select
            id={fieldId}
            className="select detail-configuration__select"
            value={showCustomVoice ? customVoiceSentinel : draftValue}
            aria-labelledby={`${fieldId}-label`}
            onChange={event => {
              const value = event.target.value;
              if (value === customVoiceSentinel) {
                setCustomVoiceMode(true);
                setRecipeDraft(previous => ({
                  ...previous,
                  [String(key)]: previous[String(key)] && !knownVoiceIds.has(previous[String(key)].toLowerCase())
                    ? previous[String(key)]
                    : '',
                }));
                return;
              }
              setCustomVoiceMode(false);
              setRecipeDraft(previous => ({ ...previous, [String(key)]: value }));
            }}
          >
            <option value="">{defaultVoice ? `Model default (${defaultVoice})` : 'Model default'}</option>
            {knownVoiceOptions.map(option => (
              <option key={option.id} value={option.id}>{option.label}</option>
            ))}
            <option value={customVoiceSentinel}>Custom voice…</option>
          </select>
          {showCustomVoice && (
            <input
              id={`${fieldId}-custom`}
              className="input detail-configuration__voice-custom"
              type="text"
              value={isUnknownDraft ? draftValue : ''}
              placeholder="Enter custom voice ID"
              aria-label="Custom voice ID"
              onChange={event => setRecipeDraft(previous => ({ ...previous, [String(key)]: event.target.value }))}
            />
          )}
          <small>Choose a known voice, or use a custom voice ID when the backend supports one.</small>
        </div>
      );
    }

    if (ARGS_TUNING_KEYS.has(key)) {
      const hint = TUNING_FIELD_HINTS[key];
      const defaultPlaceholders: Partial<Record<keyof RecipeOptions, string>> = {
        llamacpp_args: '--gpu-layers 35 --threads 8 --batch-size 512',
        vllm_args: '--tensor-parallel-size 1 --max-model-len 8192',
        sdcpp_args: '--steps 20 --cfg-scale 7.5',
        whispercpp_args: '--threads 4 --beam-size 5',
        moonshine_args: '--threads 4',
        flm_args: '--threads 4',
      };
      const draftKey = String(key);
      const hasDraftValue = Object.prototype.hasOwnProperty.call(recipeDraft, draftKey);
      const effectiveArgsValue = (hasDraftValue ? draftValue : optionalDisplayValue(baseValue)) || '';
      const argsPlaceholder = defaultPlaceholders[key] || 'Space-separated CLI flags';
      return (
        <label key={String(key)} className="detail-tuning__field detail-tuning__field--wide detail-configuration__field detail-configuration__args-field" htmlFor={fieldId}>
          <span>{label}</span>
          <textarea
            id={fieldId}
            className="detail-tuning__args"
            rows={4}
            value={effectiveArgsValue}
            placeholder={argsPlaceholder}
            onChange={e => setRecipeDraft(prev => ({ ...prev, [draftKey]: e.target.value }))}
          />
          <small>
            {hasDraftValue && draftValue.trim()
              ? 'Custom args saved for this model.'
              : 'Using the current backend args. Edit to customize load/reload flags.'}
          </small>
        </label>
      );
    }

    if (NUMERIC_TUNING_KEYS.has(key)) {
      const stepNumericInput = (direction: -1 | 1) => {
        const input = document.getElementById(fieldId);
        if (!(input instanceof HTMLInputElement)) return;
        if (direction > 0) input.stepUp();
        else input.stepDown();
        setRecipeDraft(prev => ({ ...prev, [String(key)]: input.value }));
      };

      return (
        <div key={String(key)} className="detail-tuning__field">
          <span id={`${fieldId}-label`}>{label}</span>
          <div className="detail-configuration__number-control">
            <input
              id={fieldId}
              className="input detail-configuration__number-input"
              type="number"
              value={draftValue}
              placeholder={optionalDisplayValue(baseValue) || ''}
              aria-labelledby={`${fieldId}-label`}
              onChange={e => setRecipeDraft(prev => ({ ...prev, [String(key)]: e.target.value }))}
            />
            <span className="detail-configuration__context-stepper">
              <button type="button" onClick={() => stepNumericInput(1)} aria-label={`Increase ${label}`}>
                <Icon name="chevron-up" size={11} aria-hidden="true" />
              </button>
              <button type="button" onClick={() => stepNumericInput(-1)} aria-label={`Decrease ${label}`}>
                <Icon name="chevron-down" size={11} aria-hidden="true" />
              </button>
            </span>
          </div>
        </div>
      );
    }

    return (
      <label key={String(key)} className="detail-tuning__field" htmlFor={fieldId}>
        <span>{label}</span>
        <input
          id={fieldId}
          className="input"
          type="text"
          value={draftValue}
          placeholder={optionalDisplayValue(baseValue) || ''}
          onChange={e => setRecipeDraft(prev => ({ ...prev, [String(key)]: e.target.value }))}
        />
      </label>
    );
  };

  return (
    <div className="detail-tab-content detail-configuration">
      <section className="detail-configuration__section" aria-label="Load settings">
        <div className="detail-configuration__section-head">
          <div>
            <h3 className="detail-configuration__section-heading">Load settings</h3>
            <p className="detail-configuration__section-copy">
              Settings used when this model loads or reloads.
            </p>
          </div>
          <span className={`detail-configuration__status ${loadedModel ? 'is-loaded' : ''}`}>
            {loadedModel ? 'Loaded now' : 'Saved defaults'}
          </span>
        </div>

        <div className="detail-configuration__load-controls">
          {supportsContextSize && (
            <div className="detail-configuration__context-card">
              <div className="detail-configuration__control-head">
                <label htmlFor={ctxSliderId}>Context size</label>
                {!isAutoTuning && (
                  <div className="detail-configuration__context-number">
                    <input
                      id={ctxSizeId}
                      className="input detail-configuration__context-input"
                      type="number"
                      min={ctxMin}
                      max={ctxMax}
                      step={ctxStep}
                      value={ctxSizeDraft}
                      placeholder={String(baseCtxSize)}
                      onChange={e => setCtxSizeDraft(e.target.value)}
                      aria-label="Context size tokens"
                    />
                    <span className="detail-configuration__context-stepper">
                      <button
                        type="button"
                        onClick={() => stepContextSize(1)}
                        disabled={currentCtxSize >= ctxMax}
                        aria-label={`Increase context size by ${ctxStep} tokens`}
                      >
                        <Icon name="chevron-up" size={11} aria-hidden="true" />
                      </button>
                      <button
                        type="button"
                        onClick={() => stepContextSize(-1)}
                        disabled={currentCtxSize <= ctxMin}
                        aria-label={`Decrease context size by ${ctxStep} tokens`}
                      >
                        <Icon name="chevron-down" size={11} aria-hidden="true" />
                      </button>
                    </span>
                  </div>
                )}
              </div>
              <label
                className="detail-configuration__autotune"
                title="Lemonade estimates a context size from available memory at load time. The result can differ if other models are loaded or memory use changes."
              >
                <input
                  type="checkbox"
                  checked={isAutoTuning}
                  onChange={e => setCtxSizeDraft(e.target.checked ? '-1' : String(currentCtxSize))}
                  aria-describedby={`${ctxSizeId}-autotune-help`}
                />
                <span>Auto tune context size</span>
                <Icon name="info" size={14} aria-hidden="true" />
              </label>
              <small id={`${ctxSizeId}-autotune-help`} className="detail-configuration__autotune-help">
                Estimates a safe context size from available memory when the model loads. Turn it off to choose a fixed value.
              </small>
              {!isAutoTuning && (
                <div className="detail-configuration__slider-row">
                  <span>{formatContextSize(ctxMin)}</span>
                  <input
                    id={ctxSliderId}
                    className="slider detail-configuration__context-slider"
                    type="range"
                    min={ctxMin}
                    max={ctxMax}
                    step={ctxStep}
                    value={currentCtxSize}
                    onChange={e => setCtxSizeDraft(e.target.value)}
                  />
                  <span>{formatContextSize(ctxMax)}</span>
                </div>
              )}
              <small>{isAutoTuning ? 'Estimated at load time' : `${currentCtxSize.toLocaleString()} tokens`}</small>
            </div>
          )}

          {selectorKeys.length > 0 && (
            <div className="detail-configuration__selector-grid">
              {selectorKeys.map(key => renderConfigRecipeField(key))}
            </div>
          )}

          {otherRecipeKeys.length > 0 && (
            <div className="detail-configuration__selector-grid">
              {otherRecipeKeys.map(key => renderConfigRecipeField(key))}
            </div>
          )}

          {argsKeys.length > 0 && (
            <div className="detail-configuration__args-grid">
              {argsKeys.map(key => renderConfigRecipeField(key))}
            </div>
          )}
        </div>

        <div className="detail-tuning__actions detail-configuration__actions">
          {loadedModel && onReloadModel && (
            <button
              type="button"
              className="btn btn--primary btn--sm"
              onClick={reloadViaPanel}
              disabled={isReloading || isLoadingThis}
              aria-busy={isReloading}
            >
              <Icon name="rotate-ccw" size={13} aria-hidden="true" /> {isReloading ? 'Reloading\u2026' : 'Reload model'}
            </button>
          )}
          <button type="button" className={`btn ${hasLoadSettingChanges ? 'btn--primary' : 'btn--ghost'} btn--sm`} onClick={() => saveConfig()}>Save defaults</button>
          {hasLoadSettingChanges && (
            <button type="button" className="btn btn--ghost btn--sm" onClick={discardConfig}>Discard changes</button>
          )}
          <button type="button" className="btn btn--ghost btn--sm" onClick={resetConfig}>Reset to defaults</button>
        </div>

        {notice && (
          <p className="detail-tuning__notice" role="status">{notice}</p>
        )}
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

/** Title-case a role slug for display (e.g. "mmproj" → "Mmproj", "main" → "Main", "flash_attn" → "Flash Attn"). */
function roleLabel(role: string): string {
  const r = String(role || '').trim();
  if (!r) return 'File';
  return r
    .replace(/[_-]+/g, ' ')
    .replace(/\b\w/g, ch => ch.toUpperCase());
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

function recordValue(value: unknown): Record<string, unknown> {
  return value && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown>
    : {};
}

const RouterCollectionSettingsTab: React.FC<{
  model: ModelInfo;
  models: ModelInfo[];
  onEdit?: (model: ModelInfo) => void;
}> = ({ model, models, onEdit }) => {
  const [providers, setProviders] = useState<CloudProviderRow[]>([]);
  const [providerError, setProviderError] = useState<string | null>(null);
  const [editingProvider, setEditingProvider] = useState<string | null>(null);
  const [editingConnectionModel, setEditingConnectionModel] = useState<string | null>(null);
  const [endpointDraft, setEndpointDraft] = useState('');
  const [allowInsecureDraft, setAllowInsecureDraft] = useState(false);
  const [savingProvider, setSavingProvider] = useState(false);

  const refreshProviders = useCallback(async () => {
    try {
      setProviders(await api.cloudProviders());
      setProviderError(null);
    } catch (error) {
      setProviderError(error instanceof Error ? error.message : 'Could not load external providers.');
    }
  }, []);

  useEffect(() => {
    void refreshProviders();
  }, [refreshProviders]);

  const routing = recordValue((model as any).routing);
  const candidates = Array.isArray(routing.candidates)
    ? routing.candidates.filter((item): item is string => typeof item === 'string' && Boolean(item.trim()))
    : [];
  const defaultModel = String(routing.default_model || '').trim();
  const nlRouter = recordValue(routing.router);
  const classifiers = Array.isArray(routing.classifiers)
    ? routing.classifiers.filter(item => item && typeof item === 'object') as Array<Record<string, unknown>>
    : [];
  const connectedRoles = new Map<string, string[]>();
  const addRole = (nameValue: unknown, role: string) => {
    const name = String(nameValue || '').trim();
    if (!name) return;
    const current = connectedRoles.get(name) || [];
    if (!current.includes(role)) current.push(role);
    connectedRoles.set(name, current);
  };
  candidates.forEach(name => addRole(name, 'Routing target'));
  if (String(nlRouter.type || '').toLowerCase() === 'llm') {
    addRole(nlRouter.model, 'Natural-language router');
  }
  classifiers.forEach(classifier => addRole(classifier.model, `Classifier · ${String(classifier.id || classifier.type || 'model')}`));
  getCollectionComponents(model).forEach(name => addRole(name, 'Collection component'));

  const connections = [...connectedRoles.keys()].map(name =>
    describeRouterModelConnection(name, models, providers)
  );
  const mode = Object.keys(nlRouter).length ? 'Natural-language router' : 'Ordered rules';

  const saveEndpoint = async () => {
    if (!editingProvider) return;
    const validationError = validateProviderEndpoint(endpointDraft, allowInsecureDraft);
    if (validationError) {
      setProviderError(validationError);
      return;
    }
    setSavingProvider(true);
    setProviderError(null);
    try {
      const provider = editingProvider;
      await api.installCloudProvider(provider, endpointDraft.trim(), undefined, allowInsecureDraft);
      await refreshProviders();
      setEditingProvider(null);
      setEditingConnectionModel(null);
      setEndpointDraft('');
      setAllowInsecureDraft(false);
    } catch (error) {
      setProviderError(error instanceof Error ? error.message : 'Could not update provider endpoint.');
    } finally {
      setSavingProvider(false);
    }
  };

  return (
    <div className="detail-tab-content custom-collection-settings router-collection-settings">
      <div className="custom-collection-settings__intro">
        <div>
          <h3>Router settings</h3>
          <p>Review every model connected to this virtual model, including provider endpoints used by external candidates.</p>
        </div>
        {onEdit && (
          <button
            type="button"
            className="btn btn--primary btn--sm custom-collection-settings__edit-button"
            aria-label="Edit router settings"
            title="Edit router settings"
            onClick={(event) => {
              event.preventDefault();
              event.stopPropagation();
              onEdit(model);
            }}
          >
            <Icon name="edit" size={13} aria-hidden="true" />
            <span>Edit router</span>
          </button>
        )}
      </div>

      <section className="custom-collection-settings__section" aria-label="Router strategy summary">
        <h4>Routing</h4>
        <div className="custom-collection-settings__summary">
          <span>Strategy</span><strong>{mode}</strong>
          <span>Default model</span><strong>{defaultModel || 'Not set'}</strong>
          <span>Routing targets</span><strong>{candidates.length}</strong>
        </div>
      </section>

      <section className="custom-collection-settings__section" aria-label="Connected router models">
        <h4>Connected models</h4>
        <p className="router-collection-settings__scope-note">Endpoint changes are provider-wide and apply to all models registered through that provider.</p>
        <div className="router-collection-settings__connections">
          {connections.map(connection => (
            <article className="router-collection-settings__connection" key={connection.modelName}>
              <div className="router-collection-settings__identity">
                <div>
                  <strong>{connection.displayName}</strong>
                  {connection.modelName === defaultModel && <span className="router-editor__default-badge">Default</span>}
                </div>
                <small>{connection.modelName}</small>
                <small>{(connectedRoles.get(connection.modelName) || []).join(' · ')}</small>
              </div>
              <div className="router-collection-settings__source">
                <span className={`router-editor__source-badge router-editor__source-badge--${connection.kind}`}>
                  {connection.kind === 'external' ? 'External' : connection.kind === 'internal' ? 'Internal' : 'Unresolved'}
                </span>
                <small>{connection.kind === 'external' ? (connection.provider || 'Unknown provider') : (connection.backend || connection.recipe || 'Local model')}</small>
              </div>
              <div className="router-collection-settings__endpoint">
                {connection.kind === 'external' ? (
                  editingProvider === connection.provider && editingConnectionModel === connection.modelName ? (
                    <div className="router-editor__endpoint-editor">
                      <input
                        className="input"
                        value={endpointDraft}
                        aria-label={`${connection.provider} endpoint`}
                        onChange={event => setEndpointDraft(event.target.value)}
                      />
                      {providerEndpointNeedsInsecureOptIn(endpointDraft) && (
                        <label className="router-editor__insecure-opt-in">
                          <input type="checkbox" checked={allowInsecureDraft} onChange={event => setAllowInsecureDraft(event.target.checked)} />
                          <span>Allow insecure HTTP</span>
                        </label>
                      )}
                      <WorkspaceActionButton appearance="primary" size="small" disabled={savingProvider} onClick={() => { void saveEndpoint(); }}>
                        {savingProvider ? 'Saving…' : 'Save'}
                      </WorkspaceActionButton>
                      <WorkspaceActionButton size="small" onClick={() => { setEditingProvider(null); setEditingConnectionModel(null); setAllowInsecureDraft(false); setProviderError(null); }}>Cancel</WorkspaceActionButton>
                    </div>
                  ) : (
                    <>
                      <span title={connection.endpoint || 'Endpoint unavailable'}>{connection.endpoint || 'Endpoint not configured'}</span>
                      <small>{connection.authConfigured ? 'Authentication configured' : 'Authentication required'}</small>
                      {connection.provider && (
                        <WorkspaceActionButton size="small" icon="edit" onClick={() => { setEditingProvider(connection.provider); setEditingConnectionModel(connection.modelName); setEndpointDraft(connection.endpoint); setAllowInsecureDraft(connection.allowInsecureHttp); setProviderError(null); }}>
                          Edit endpoint
                        </WorkspaceActionButton>
                      )}
                    </>
                  )
                ) : (
                  <><span>Managed by Lemonade</span><small>Local registered model</small></>
                )}
              </div>
            </article>
          ))}
          {connections.length === 0 && <div className="router-editor__empty">No connected models were found in this router definition.</div>}
        </div>
        {providerError && connections.some(connection => connection.kind === 'external') && <div className="router-editor__message router-editor__message--error"><Icon name="alert" size={14} /> {providerError}</div>}
      </section>
    </div>
  );
};

const CustomCollectionSettingsTab: React.FC<{
  model: ModelInfo;
  models: ModelInfo[];
  onEdit?: (model: ModelInfo) => void;
}> = ({ model, models, onEdit }) => {
  if (activeRecipeForModel(model) === 'collection.router') {
    return <RouterCollectionSettingsTab model={model} models={models} onEdit={onEdit} />;
  }

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
          <button
            type="button"
            className="btn btn--primary btn--sm custom-collection-settings__edit-button"
            aria-label="Edit collection settings"
            title="Edit collection settings"
            onClick={(event) => {
              event.preventDefault();
              event.stopPropagation();
              onEdit(model);
            }}
          >
            <Icon name="edit" size={13} aria-hidden="true" />
            <span>Edit settings</span>
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
          <span>Custom model tools</span>
          <strong>{tools.length}</strong>
        </div>
      </section>
    </div>
  );
};

/* ── ModelDetailPanel ────────────────────────────────────────── */

export interface ModelDetailPanelProps {
  model: ModelInfo | null;
  /** Registry models used to resolve collection component sources. */
  models?: ModelInfo[];
  loadedModel: LoadedModel | null;
  loadingModel: string | null;
  pulling: Record<string, number>;
  loadError: { modelName: string; message: string } | null;
  onLoad: (model: ModelInfo) => void;
  onUnload: (model: LoadedModel) => void;
  /** Reload an already-loaded model with updated load-time configuration. */
  onReloadModel?: (
    model: LoadedModel,
    recipeOptions?: Record<string, unknown>,
  ) => Promise<void>;
  onPull: (model: ModelInfo) => void;
  onPullAndLoad: (model: ModelInfo) => void;
  onDelete: (model: ModelInfo) => void;
  onCancelPull: (name: string) => void;
  serverDefaultCtxSize: number;
  /** Whether this model is currently pinned in the client-local model list. */
  isPinned?: boolean;
  /** Toggle this model's pinned state. Receives the model name. */
  onTogglePin?: (name: string) => void;
  /** Whether this model is currently marked as a favorite. */
  isFavorite?: boolean;
  /** Toggle this model's favorite state. Receives the model name. */
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
  /** Registry source for the remote model. */
  hfProvider?: ModelRegistryProvider;
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

type DetailTab = 'settings' | 'readme' | 'config' | 'files';

const TABS: Array<{ id: DetailTab; label: string }> = [
  { id: 'config', label: 'Configuration' },
  { id: 'readme', label: 'README' },
  { id: 'files', label: 'Files' },
];

const CUSTOM_COLLECTION_TABS: Array<{ id: DetailTab; label: string }> = [
  { id: 'settings', label: 'Settings' },
  { id: 'files', label: 'Files' },
];

const IMAGE_MODEL_TABS: Array<{ id: DetailTab; label: string }> = TABS;

export const ModelDetailPanel: React.FC<ModelDetailPanelProps> = ({
  model,
  models = [],
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
  isPinned = false,
  onTogglePin,
  isFavorite = false,
  onToggleFavorite,
  onEditCustomCollection,
  onBack,
  onClose,
  noModelsAvailable = false,
  hfModel,
  hfProvider = 'huggingface',
  hfVariants,
  onFetchHfVariants,
  onHfPull,
  pullingHf,
  onCancelHfPull,
}) => {
  const [activeTab, setActiveTab] = useState<DetailTab>('config');
  const tabRefs = useRef<Array<HTMLButtonElement | null>>([]);
  const panelHeadingRef = useRef<HTMLHeadingElement>(null);
  const [configHasUnsavedChanges, setConfigHasUnsavedChanges] = useState(false);

  const detailName = model ? mdName(model) : '';
  const isRouterCollection = Boolean(model && activeRecipeForModel(model) === 'collection.router');
  const isCustomCollection = Boolean(model && modelIsCustom(model) && (isCollectionModel(model) || isRouterCollection));
  const imageOnly = isImageOnlyModel(model);
  const detailTabs = isCustomCollection ? CUSTOM_COLLECTION_TABS : (imageOnly ? IMAGE_MODEL_TABS : TABS);

  // Move focus to heading when model changes.
  useEffect(() => {
    if (model) panelHeadingRef.current?.focus();
    setActiveTab(isCustomCollection ? 'settings' : 'config');
    setConfigHasUnsavedChanges(false);
  }, [detailName, isCustomCollection, imageOnly]);

  const confirmDiscardLoadSettings = useCallback(() => (
    !configHasUnsavedChanges || window.confirm('Discard unsaved load setting changes?')
  ), [configHasUnsavedChanges]);

  const handleDetailClose = useCallback(() => {
    if (!confirmDiscardLoadSettings()) return;
    onClose?.();
  }, [confirmDiscardLoadSettings, onClose]);

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
        provider={hfProvider}
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
      <div className="model-detail-panel workspace-detail-panel workspace-detail-panel--empty model-detail-panel--empty" aria-label="Model detail">
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
  const isDownloaded = Boolean((model as any).downloaded) && !isPulling;
  const cap = capabilityFromModelInfo(model);

  const detailMetadata = (
    <>
      {recipe && (
        <WorkspaceMetadataChip emphasis="medium" tone="accent">{recipeDisplayLabel(recipe)}</WorkspaceMetadataChip>
      )}
      {model.size != null && model.size > 0 && (
        <WorkspaceMetadataChip emphasis="low">{fmtSize(model.size)}</WorkspaceMetadataChip>
      )}
      {cap && (
        <WorkspaceMetadataChip emphasis="medium">
          <CapabilityIcon capability={cap} size={12} aria-hidden="true" />
          {capabilityLabel(cap)}
        </WorkspaceMetadataChip>
      )}
      {isLoaded && (
        <WorkspaceMetadataChip emphasis="high" tone="success">
          <span className="row__pulse" aria-hidden="true" /> Running
        </WorkspaceMetadataChip>
      )}
      {isDownloaded && !isLoaded && (
        <WorkspaceMetadataChip emphasis="high" tone="success">Ready</WorkspaceMetadataChip>
      )}
      {hfRepo && (
        <WorkspaceMetadataChip
          emphasis="low"
          icon="globe"
          href={`https://huggingface.co/${hfRepo}`}
          target="_blank"
          rel="noopener noreferrer"
          title={`View ${name} on Hugging Face`}
        >
          Hugging Face
        </WorkspaceMetadataChip>
      )}
    </>
  );

  const detailActions = (
    <WorkspaceActionGroup className="model-detail-panel__actions" label={`Actions for ${name}`}>
      {isPulling ? (
        <>
          <div className="row__progress">
            <div className="row__progress-bar">
              <div className="row__progress-fill" style={{ width: `${pullPct}%` }} />
            </div>
            <span className="row__progress-text">{pullPct.toFixed(0)}%</span>
          </div>
          <WorkspaceActionButton appearance="secondary" icon="x" onClick={() => onCancelPull(name)} aria-label={`Cancel download of ${name}`}>
            Cancel
          </WorkspaceActionButton>
        </>
      ) : isLoaded ? (
        <>
          <WorkspaceActionButton
            appearance="primary"
            icon="eject"
            onClick={() => onUnload(loadedModel!)}
            disabled={isLoadingThis}
            aria-label={isLoadingThis ? `Working on ${name}…` : `Unload ${name}`}
          >
            {isLoadingThis ? 'Working…' : 'Unload'}
          </WorkspaceActionButton>
        </>
      ) : isDownloaded ? (
        <WorkspaceActionButton
          appearance="primary"
          icon="play"
          onClick={() => onLoad(model)}
          disabled={isLoadingThis}
          aria-label={isLoadingThis ? `Loading ${name}…` : `Load ${name}`}
        >
          {isLoadingThis ? 'Loading…' : 'Load'}
        </WorkspaceActionButton>
      ) : (
        <>
          <WorkspaceActionButton appearance="primary" icon="download" onClick={() => onPullAndLoad(model)} aria-label={`Get and load ${name}`}>
            Get & Load
          </WorkspaceActionButton>
          <WorkspaceActionButton appearance="secondary" icon="download" onClick={() => onPull(model)} aria-label={`Download ${name}`}>
            Download
          </WorkspaceActionButton>
        </>
      )}
      {onTogglePin && (
        <WorkspaceActionButton
          className={`model-detail-panel__fav-btn${isPinned ? ' model-detail-panel__fav-btn--on' : ''}`}
          appearance="quiet"
          icon="pin"
          onClick={() => onTogglePin(name)}
          aria-pressed={isPinned}
          aria-label={isPinned ? `Unpin ${name}` : `Pin ${name}`}
          title={isPinned ? 'Unpin model' : 'Pin model'}
        >
          {isPinned ? 'Pinned' : 'Pin'}
        </WorkspaceActionButton>
      )}
      {onToggleFavorite && (
        <WorkspaceActionButton
          className={`model-detail-panel__fav-btn${isFavorite ? ' model-detail-panel__fav-btn--on' : ''}`}
          appearance="quiet"
          icon="star"
          onClick={() => onToggleFavorite(name)}
          aria-pressed={isFavorite}
          aria-label={isFavorite ? `Remove ${name} from favorites` : `Add ${name} to favorites`}
          title={isFavorite ? 'Remove from favorites' : 'Add to favorites'}
        >
          {isFavorite ? 'Favorited' : 'Favorite'}
        </WorkspaceActionButton>
      )}
      {(isDownloaded || isLoaded) && (
        <WorkspaceActionButton
          appearance="secondary"
          icon="trash"
          onClick={() => onDelete(model)}
          disabled={isLoadingThis}
          aria-label={(model as any).custom ? `Delete custom model definition for ${name}` : `Delete downloaded files for ${name}`}
          title={(model as any).custom ? 'Delete model definition' : 'Delete downloaded files'}
        >
          Delete
        </WorkspaceActionButton>
      )}
    </WorkspaceActionGroup>
  );

  const detailHeaderExtras = (
    <>
      {loadError?.modelName === name && (
        <div className="model-detail-panel__error" role="alert">
          <Icon name="alert" size={13} /> {loadError.message}
        </div>
      )}
    </>
  );

  return (
    <WorkspaceDetailPanel
      className="model-detail-panel"
      ariaLabel={`Model details: ${name}`}
      title={(
        <h2 className="workspace-detail-panel__title model-detail-panel__name" ref={panelHeadingRef} tabIndex={-1} id="detail-panel-heading">
          {model.display_name || name}
        </h2>
      )}
      metadata={detailMetadata}
      actions={detailActions}
      headerExtras={detailHeaderExtras}
      onBack={onBack}
      backLabel="Back to models"
      backClassName="model-detail-panel__back-btn"
      onClose={onClose ? handleDetailClose : undefined}
      closeClassName="model-detail-panel__close-btn"
    >

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
            <CustomCollectionSettingsTab model={model} models={models} onEdit={onEditCustomCollection} />
          )}
          {tab.id === 'readme' && (
            <ModelReadmeTab model={model} isActive={activeTab === 'readme'} />
          )}
          {          tab.id === 'config' && (
                      <ModelConfigurationTab
                        model={model}
                        loadedModel={loadedModel}
                        isActive={activeTab === 'config'}
                        serverDefaultCtxSize={serverDefaultCtxSize}
                        isLoadingThis={isLoadingThis}
                        onReloadModel={onReloadModel}
                        onDirtyChange={setConfigHasUnsavedChanges}
                      />
                    )}
                    {tab.id === 'files' && (
                      <ModelFilesTab model={model} isActive={activeTab === 'files'} />
          )}
        </div>
      ))}
    </WorkspaceDetailPanel>
  );
};

export default ModelDetailPanel;
