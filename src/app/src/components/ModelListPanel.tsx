/**
 * ModelListPanel — left panel of the master-detail model view.
 * Compact, searchable, filterable list of models with keyboard navigation.
 *
 * Part of the master-detail layout introduced in #2355 Slice 1.
 */
import React, { useCallback, useEffect, useRef, useMemo, useState } from 'react';
import type { ModelInfo, LoadedModel } from '../api';
import {
  capabilityFromModelInfo,
  modelCapabilityTags,
  modelMatchesCapabilityTags,
  CAPABILITY_TAG_ORDER,
  CAPABILITY_TAG_LABELS,
  type CapabilityTag,
} from '../modelCapabilities';
import { Icon, CapabilityIcon } from './Icon';
import type { IconName } from './Icon';
import type { CapabilityIconTarget } from './Icon';
import { activeDownloadForModel, type DownloadListItem } from '../features/downloadManager/downloadStore';
import { WorkspaceActionButton, WorkspaceActionGroup, WorkspaceListPanel } from './WorkspacePanels';
import { backendColor, backendCompactLabel, backendLabel } from '../modelPresentation';

/* ── Helpers ─────────────────────────────────────────────────── */

export function listModelName(m: ModelInfo): string {
  return String((m as any).model_name ?? m.name ?? m.id ?? '').trim();
}

function listModelDisplayName(m: ModelInfo): string {
  return String(m.display_name || listModelName(m));
}

function listFmtSize(gb: number): string {
  if (!Number.isFinite(gb) || gb <= 0) return '';
  if (gb >= 1) return `${gb.toFixed(1)} GB`;
  if (gb >= 0.01) return `${(gb * 1000).toFixed(0)} MB`;
  return '< 1 MB';
}

export const listRecipeBadgeText = backendCompactLabel;

function modelListBackendLabel(recipe: string): string {
  return backendCompactLabel(recipe);
}

type BackendReadinessTone = 'ready' | 'attention' | 'unknown';

export interface ModelBackendReadiness {
  tone: BackendReadinessTone;
  label: string;
  backend?: string;
  state?: string;
}

const BACKEND_MANAGED_RECIPES = new Set([
  'llamacpp',
  'vllm',
  'flm',
  'ryzenai-llm',
  'sd-cpp',
  'whispercpp',
  'moonshine',
  'kokoro',
  'acestep',
  'thinksound',
  'openmoss',
  'trellis',
]);

const BACKEND_OPTION_FIELD: Record<string, string> = {
  llamacpp: 'llamacpp_backend',
  vllm: 'vllm_backend',
  'sd-cpp': 'sd-cpp_backend',
  whispercpp: 'whispercpp_backend',
  moonshine: 'moonshine_backend',
  acestep: 'acestep_backend',
  thinksound: 'thinksound_backend',
  openmoss: 'openmoss_backend',
  trellis: 'trellis_backend',
};

function asRecord(value: unknown): Record<string, any> | null {
  return value && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, any>
    : null;
}

function normalizedBackend(value: unknown): string {
  return typeof value === 'string' ? value.trim().toLowerCase() : '';
}

function configuredBackendForModel(model: ModelInfo, recipe: string, recipeInfo: Record<string, any>): string {
  const raw = model as any;
  const recipeOptions = asRecord(raw.recipe_options);
  const options = asRecord(raw.options);
  const field = BACKEND_OPTION_FIELD[recipe];
  const configured = normalizedBackend(
    (field ? recipeOptions?.[field] : undefined)
      ?? recipeOptions?.backend
      ?? (field ? options?.[field] : undefined)
      ?? options?.backend
      ?? (field ? raw[field] : undefined)
      ?? raw.backend
      ?? raw.default_backend
      ?? raw.recommended_backend,
  );
  if (configured && configured !== 'auto') return configured;
  return normalizedBackend(recipeInfo.default_backend);
}

/**
 * A downloaded model is only ready when its selected/default backend is also
 * installed and usable. Missing or updateable backends deliberately surface as
 * attention instead of presenting the model as fully ready.
 */
export function modelBackendReadiness(
  model: ModelInfo,
  systemInfo?: Record<string, unknown> | null,
): ModelBackendReadiness {
  const recipe = normalizedBackend((model as any).recipe);
  if (!recipe) {
    return { tone: 'unknown', label: 'Model downloaded; backend could not be determined.' };
  }

  const recipes = asRecord(systemInfo?.recipes);
  if (!recipes) {
    return { tone: 'unknown', label: 'Model downloaded; backend status is not available.' };
  }

  const recipeInfo = asRecord(recipes[recipe]);
  if (!recipeInfo && !BACKEND_MANAGED_RECIPES.has(recipe)) {
    return { tone: 'ready', label: 'Model downloaded and ready.' };
  }
  if (!recipeInfo) {
    return {
      tone: 'attention',
      label: `${backendLabel(recipe)} backend is not installed on this server.`,
      state: 'missing',
    };
  }

  const backends = asRecord(recipeInfo.backends);
  if (!backends || Object.keys(backends).length === 0) {
    return {
      tone: 'attention',
      label: `${backendLabel(recipe)} backend must be installed before loading this model.`,
      state: 'missing',
    };
  }

  const configuredBackend = configuredBackendForModel(model, recipe, recipeInfo);
  let backend = configuredBackend;
  let backendInfo: Record<string, any> | null = null;

  if (backend) {
    const match = Object.entries(backends).find(([name]) => normalizedBackend(name) === backend);
    if (match) {
      backend = match[0];
      backendInfo = asRecord(match[1]);
    } else {
      return {
        tone: 'attention',
        backend,
        state: 'missing',
        label: `${backendLabel(recipe)} backend “${backend}” must be installed before loading this model.`,
      };
    }
  } else {
    const entries = Object.entries(backends);
    const preferred = entries.find(([, info]) => normalizedBackend((info as any)?.state) === 'installed')
      ?? entries.find(([, info]) => ['update_required', 'update_available'].includes(normalizedBackend((info as any)?.state)))
      ?? entries[0];
    backend = preferred?.[0] || '';
    backendInfo = preferred ? asRecord(preferred[1]) : null;
  }

  const state = normalizedBackend(backendInfo?.state);
  const backendSuffix = backend ? ` (${backend})` : '';
  if (state === 'installed') {
    return {
      tone: 'ready',
      backend,
      state,
      label: `${backendLabel(recipe)}${backendSuffix} is installed; model is ready.`,
    };
  }
  if (state === 'update_required') {
    return {
      tone: 'attention',
      backend,
      state,
      label: `${backendLabel(recipe)}${backendSuffix} requires an update before use.`,
    };
  }
  if (state === 'update_available') {
    return {
      tone: 'attention',
      backend,
      state,
      label: `${backendLabel(recipe)}${backendSuffix} has an update available.`,
    };
  }
  if (state === 'installable') {
    return {
      tone: 'attention',
      backend,
      state,
      label: `${backendLabel(recipe)}${backendSuffix} must be downloaded before loading this model.`,
    };
  }
  if (state === 'action_required') {
    return {
      tone: 'attention',
      backend,
      state,
      label: `${backendLabel(recipe)}${backendSuffix} needs attention before loading this model.`,
    };
  }
  if (state === 'unsupported') {
    return {
      tone: 'attention',
      backend,
      state,
      label: `${backendLabel(recipe)}${backendSuffix} is not supported on this system.`,
    };
  }

  return {
    tone: 'unknown',
    backend,
    state: state || undefined,
    label: `${backendLabel(recipe)}${backendSuffix} status could not be verified.`,
  };
}

type FilterTab = 'all' | 'llm' | 'omni' | 'router' | 'image' | 'audio' | 'audio-generation' | 'tts' | 'model3d' | 'embedding';

const FILTER_TABS: Array<{ key: FilterTab; label: string; iconName: IconName }> = [
  { key: 'all', label: 'All', iconName: 'globe' },
  { key: 'llm', label: 'Chat', iconName: 'chat' },
  { key: 'omni', label: 'Omni', iconName: 'omni' },
  { key: 'router', label: 'Router', iconName: 'router' },
  { key: 'image', label: 'Image', iconName: 'image' },
  { key: 'audio', label: 'Audio', iconName: 'audio' },
  { key: 'audio-generation', label: 'Music & SFX', iconName: 'audio' },
  { key: 'tts', label: 'TTS', iconName: 'tts' },
  { key: 'model3d', label: '3D', iconName: 'box' },
  { key: 'embedding', label: 'Embed', iconName: 'embedding' },
];

type TextFilterField = 'any' | 'name' | 'backend' | 'type' | 'capability' | 'label' | 'status';
type TextFilterMode = 'include' | 'exclude';

interface TextFilterRule {
  id: string;
  field: TextFilterField;
  mode: TextFilterMode;
  value: string;
}

interface FilterPopoverStyle extends React.CSSProperties {
  left: number;
  top: number;
  width: number;
  maxHeight: number;
}

const TEXT_FILTER_FIELDS: Array<{ key: TextFilterField; label: string }> = [
  { key: 'any', label: 'All text' },
  { key: 'name', label: 'Name' },
  { key: 'backend', label: 'Backend' },
  { key: 'type', label: 'Type' },
  { key: 'capability', label: 'Capability' },
  { key: 'label', label: 'Label' },
  { key: 'status', label: 'Status' },
];


let textFilterRuleCounter = 0;

function createTextFilterRule(overrides: Partial<TextFilterRule> = {}): TextFilterRule {
  textFilterRuleCounter += 1;
  return {
    id: `text-filter-${textFilterRuleCounter}`,
    field: 'any',
    mode: 'include',
    value: '',
    ...overrides,
  };
}

function normalizeFilterText(value: unknown): string {
  return String(value ?? '').trim().toLowerCase();
}

function compactTextFilters(filters: TextFilterRule[]): TextFilterRule[] {
  return filters.filter(rule => normalizeFilterText(rule.value).length > 0);
}

function listCapabilityLabelText(m: ModelInfo): string {
  return modelCapabilityTags(m).map(tag => CAPABILITY_TAG_LABELS[tag]).join(' ');
}

function listModelTypeText(m: ModelInfo): string {
  const cap = capabilityFromModelInfo(m);
  if (cap === 'chat' || cap === 'unknown') return 'llm chat text';
  if (cap === 'audio') return 'audio transcription speech-to-text asr stt';
  if (cap === 'tts') return 'tts speech text-to-speech';
  if (cap === 'image') return 'image diffusion image-generation';
  if (cap === 'embedding') return 'embedding embed';
  if (cap === 'reranking') return 'reranking reranker';
  return String(cap);
}

function listModelStatusText(status: ModelStatus): string {
  switch (status) {
    case 'running': return 'running loaded active';
    case 'downloaded': return 'downloaded local ready';
    case 'downloading': return 'downloading pulling download';
    case 'available':
    default: return 'available remote';
  }
}

function modelTextFilterTarget(m: ModelInfo, status: ModelStatus, field: TextFilterField): string {
  const nameText = `${listModelName(m)} ${m.display_name || ''}`;
  const recipe = String((m as any).recipe || '');
  const backendText = recipe ? `${recipe} ${listRecipeBadgeText(recipe)}` : '';
  const typeText = listModelTypeText(m);
  const labelText = (m.labels || []).join(' ');
  const capabilityText = `${listCapabilityLabelText(m)} ${modelCapabilityTags(m).join(' ')}`;
  const statusText = listModelStatusText(status);

  switch (field) {
    case 'name': return nameText;
    case 'backend': return backendText;
    case 'type': return typeText;
    case 'capability': return capabilityText;
    case 'label': return labelText;
    case 'status': return statusText;
    case 'any':
    default:
      return `${nameText} ${backendText} ${typeText} ${labelText} ${capabilityText} ${statusText}`;
  }
}

function modelMatchesTextFilters(m: ModelInfo, status: ModelStatus, filters: TextFilterRule[]): boolean {
  for (const rule of filters) {
    const needle = normalizeFilterText(rule.value);
    if (!needle) continue;
    const haystack = modelTextFilterTarget(m, status, rule.field).toLowerCase();
    const matches = haystack.includes(needle);
    if (rule.mode === 'include' && !matches) return false;
    if (rule.mode === 'exclude' && matches) return false;
  }
  return true;
}

function modelRecipe(m: ModelInfo): string {
  return String((m as any).recipe || '').trim().toLowerCase();
}

export function modelIsRouter(m: ModelInfo): boolean {
  const recipe = modelRecipe(m);
  return recipe === 'collection.router' || recipe.startsWith('collection.router.');
}

export function modelIsOmniCollection(m: ModelInfo): boolean {
  const recipe = modelRecipe(m);
  return recipe === 'collection.omni' || recipe.startsWith('collection.omni.') || recipe === 'collection';
}

/** Omni is a task identity, not a concrete backend identity. */
export function modelIsOmni(m: ModelInfo): boolean {
  return modelIsOmniCollection(m) || capabilityFromModelInfo(m) === 'omni';
}

export function modelMatchesFilter(m: ModelInfo, filter: FilterTab): boolean {
  if (filter === 'all') return true;
  if (filter === 'router') return modelIsRouter(m);
  if (filter === 'omni') return modelIsOmni(m);

  const cap = capabilityFromModelInfo(m);
  if (filter === 'embedding') return cap === 'embedding' || cap === 'reranking';
  // Router collections intentionally have their own task and must not also be
  // counted as Chat even though they ultimately route chat-capable models.
  if (filter === 'llm') return cap === 'chat' && !modelIsRouter(m);
  return (cap as string) === filter;
}

/** Empty task selection means "all". Multiple selected tasks are OR-ed. */
export function modelMatchesTasks(m: ModelInfo, tasks?: ReadonlySet<FilterTab>): boolean {
  if (!tasks || tasks.size === 0 || tasks.has('all')) return true;
  for (const task of tasks) {
    if (modelMatchesFilter(m, task)) return true;
  }
  return false;
}

/* ── Left-nav-rail filter dimensions ─────────────────────────────
   These predicates are the single source of truth shared by the
   middle list (filtering) and the left nav rail (deriving counts),
   so both stay perfectly in sync. All derivation is client-side from
   the model list the prototype already loads — no lemond calls. */

/** Primary nav buckets in the left rail. */
export type PrimaryFilter = 'all' | 'downloaded' | 'my-models' | 'favorites';

/** A model counts as "downloaded" if it is locally present or running. */
export function modelIsDownloaded(m: ModelInfo, loadedNames: Set<string>): boolean {
  const name = listModelName(m);
  return loadedNames.has(name) || Boolean((m as any).downloaded);
}

/** Custom / user-registered models from either the client store or lemond. */
export function modelIsCustom(m: ModelInfo): boolean {
  if ((m as any).custom === true) return true;
  const name = listModelName(m).toLowerCase();
  const labels = Array.isArray(m.labels) ? m.labels.map(label => String(label).trim().toLowerCase()) : [];
  const source = String((m as any).source || (m as any).registry_source || '').trim().toLowerCase();
  return name.startsWith('user.')
    || labels.includes('custom')
    || source === 'user'
    || source === 'user_models'
    || source === 'custom';
}

export function modelMatchesPrimary(
  m: ModelInfo,
  primary: PrimaryFilter,
  loadedNames: Set<string>,
  favoriteNames?: Set<string>,
): boolean {
  switch (primary) {
    case 'downloaded': return modelIsDownloaded(m, loadedNames);
    case 'my-models': return modelIsCustom(m);
    case 'favorites': return favoriteNames?.has(listModelName(m).toLowerCase()) ?? false;
    case 'all':
    default: return true;
  }
}

/** Map a functional capability tag onto its icon target (tags reuse the
    capability icon set; 'tool' shares the wrench glyph). */
export function capabilityTagIconTarget(tag: CapabilityTag): CapabilityIconTarget {
  return tag as CapabilityIconTarget;
}

/** A backend group is not meaningful for virtual Omni/Router collections. */
export function modelHasFilterableBackend(m: ModelInfo): boolean {
  return !modelIsOmni(m) && !modelIsRouter(m) && Boolean(modelRecipe(m));
}

/** Empty backend selection means "all". Multiple selected backends are OR-ed. */
export function modelMatchesBackends(m: ModelInfo, backends?: ReadonlySet<string>): boolean {
  if (!backends || backends.size === 0 || backends.has('all')) return true;
  return backends.has(modelRecipe(m));
}

/** Compatibility helper for callers that still need a single backend check. */
export function modelMatchesBackend(m: ModelInfo, backend: string): boolean {
  return modelMatchesBackends(m, backend && backend !== 'all' ? new Set([backend]) : new Set());
}

/** Curated tag chips (model families + size hints) shown in the left rail. */
export const TAG_CHIPS: string[] = ['Recommended', 'Llama', 'Qwen', 'Phi', 'Mistral', 'Gemma', 'Bonsai', 'Small'];

export function modelIsRecommended(m: ModelInfo): boolean {
  const raw = m as any;
  if (raw.recommended === true || raw.is_recommended === true || raw.featured === true || raw.suggested === true) return true;
  const labels = [
    ...(Array.isArray(raw.labels) ? raw.labels : []),
    ...(Array.isArray(raw.tags) ? raw.tags : []),
  ].map(value => String(value).trim().toLowerCase());
  return labels.some(label => ['recommended', 'featured', 'suggested'].includes(label));
}

/** A tag matches model metadata, labels, or its name/family. */
export function modelMatchesTag(m: ModelInfo, tag: string | null): boolean {
  if (!tag) return true;
  const t = tag.trim().toLowerCase();
  if (!t) return true;
  if (t === 'recommended') return modelIsRecommended(m);
  const labels = [
    ...(Array.isArray(m.labels) ? m.labels : []),
    ...(Array.isArray((m as any).tags) ? (m as any).tags : []),
  ].map(value => String(value).trim().toLowerCase());
  if (labels.includes(t)) return true;
  const hay = `${listModelName(m)} ${m.display_name || ''}`.toLowerCase();
  return hay.includes(t);
}

/** Empty tag selection means "all". Multiple selected tags are OR-ed. */
export function modelMatchesTags(m: ModelInfo, tags?: ReadonlySet<string>): boolean {
  if (!tags || tags.size === 0) return true;
  for (const tag of tags) {
    if (modelMatchesTag(m, tag)) return true;
  }
  return false;
}

/* ── Types ───────────────────────────────────────────────────── */

export type SortBy = 'name' | 'size' | 'last-used' | 'downloads';

export type ModelStatus = 'running' | 'downloaded' | 'available' | 'downloading';

export interface FlatModelEntry {
  model: ModelInfo;
  status: ModelStatus;
  downloadPct?: number;
  pinned?: boolean;
}

export interface ModelListPanelProps {
  allModels: ModelInfo[];
  loadedNames: Set<string>;
  pulling: Record<string, number>;
  downloadItems: DownloadListItem[];
  selectedModelId: string | null;
  onSelectModel: (id: string) => void;
  searchQuery: string;
  onSearchChange: (q: string) => void;
  /** Selected left-rail tasks. Empty means all; selections are OR-ed. */
  taskFilters?: ReadonlySet<FilterTab>;
  /** Selected functional capability tags (multi-select funnel). Empty = no filter. */
  capabilityFilter?: Set<string>;
  onCapabilityFilterChange?: (next: Set<string>) => void;
  /** Primary nav bucket selected in the left rail. */
  primaryFilter?: PrimaryFilter;
  /** Selected backend recipes. Empty means all; selections are OR-ed. */
  backendFilters?: ReadonlySet<string>;
  /** Selected built-in/custom tags. Empty means all; selections are OR-ed. */
  tagFilters?: ReadonlySet<string>;
  searchInputRef?: React.RefObject<HTMLInputElement | null>;
  onOpenCustomModels?: () => void;
  onOpenRouter?: () => void;
  onOpenGlobalSettings?: () => void;
  /** Lowercased set of pinned model names. Pinned rows float to the top. Client-local. */
  pinnedNames?: Set<string>;
  /** Toggle a model's pinned state. Receives the model name. */
  onTogglePin?: (name: string) => void;
  /** Lowercased set of favorited model names (distinct from pinned). Client-local. */
  favoriteNames?: Set<string>;
  /** Optional remote-provider results rendered below the local model list. */
  registryZone?: React.ReactNode;
  /** Elevated remote-provider results rendered above the list when no local results match. */
  registryZoneTop?: React.ReactNode;
  /** Total visible remote-provider results for the anchor bar. */
  registryResultCount?: number;
  /** Latest /system-info snapshot used to join model and backend readiness. */
  systemInfo?: Record<string, unknown> | null;
}

/* ── ModelListPanel ──────────────────────────────────────────── */

export const ModelListPanel: React.FC<ModelListPanelProps> = ({
  allModels,
  loadedNames,
  pulling,
  downloadItems,
  selectedModelId,
  onSelectModel,
  searchQuery,
  onSearchChange,
  taskFilters,
  capabilityFilter,
  onCapabilityFilterChange,
  primaryFilter = 'all',
  backendFilters,
  tagFilters,
  searchInputRef,
  onOpenCustomModels,
  onOpenRouter,
  onOpenGlobalSettings,
  pinnedNames,
  onTogglePin,
  favoriteNames,
  registryZone,
  registryZoneTop,
  registryResultCount = 0,
  systemInfo = null,
}) => {
  const [filterOpen, setFilterOpen] = useState(false);
  const [filterPopoverStyle, setFilterPopoverStyle] = useState<FilterPopoverStyle | null>(null);
  const [sortBy, setSortBy] = useState<SortBy>('name');
  const [textFilters, setTextFilters] = useState<TextFilterRule[]>(() => [createTextFilterRule()]);
  const filterBtnRef = useRef<HTMLButtonElement>(null);
  const filterPopoverRef = useRef<HTMLDivElement>(null);
  const listRef = useRef<HTMLUListElement>(null);
  const defaultSearchRef = useRef<HTMLInputElement>(null);
  const inputRef = (searchInputRef ?? defaultSearchRef) as React.RefObject<HTMLInputElement>;
  const activeTextFilters = useMemo(() => compactTextFilters(textFilters), [textFilters]);

  // Build flat list filtered by search + type; sort based on sortBy
  const flatList = useMemo((): FlatModelEntry[] => {
    const q = searchQuery.trim().toLowerCase();
    const result: FlatModelEntry[] = [];

    for (const m of allModels) {
      const mName = listModelName(m);
      if (!mName) continue;

      // Left-rail dimensions: OR within Task/Backend/Tags, AND across groups.
      if (!modelMatchesTasks(m, taskFilters)) continue;
      if (!modelMatchesPrimary(m, primaryFilter, loadedNames, favoriteNames)) continue;
      if (!modelMatchesBackends(m, backendFilters)) continue;
      if (!modelMatchesTags(m, tagFilters)) continue;

      // Funnel: functional capability tags (multi-select). Empty = no filter.
      if (!modelMatchesCapabilityTags(m, capabilityFilter ?? new Set())) continue;

      const activeDownload = activeDownloadForModel(downloadItems, mName);
      const pullPct = activeDownload?.percent ?? pulling[mName];

      let status: ModelStatus;
      if (loadedNames.has(mName)) {
        status = 'running';
      } else if (pullPct !== undefined) {
        status = 'downloading';
      } else if (Boolean((m as any).downloaded)) {
        status = 'downloaded';
      } else {
        status = 'available';
      }

      // Funnel: custom text rules. All active rules are combined with AND.
      if (!modelMatchesTextFilters(m, status, activeTextFilters)) continue;

      // Filter by search
      if (q) {
        const haystack = `${mName} ${m.display_name || ''} ${(m as any).recipe || ''} ${(m.labels || []).join(' ')}`.toLowerCase();
        if (!haystack.includes(q)) continue;
      }

      result.push({ model: m, status, downloadPct: pullPct, pinned: pinnedNames?.has(mName.toLowerCase()) ?? false });
    }

    if (sortBy === 'name') {
      // Default: running → downloaded → available, then alphabetical within group
      const rank: Record<ModelStatus, number> = { running: 0, downloaded: 1, downloading: 1, available: 2 };
      result.sort((a, b) => {
        const r = rank[a.status] - rank[b.status];
        if (r !== 0) return r;
        return listModelDisplayName(a.model).localeCompare(listModelDisplayName(b.model));
      });
    } else if (sortBy === 'size') {
      result.sort((a, b) => {
        const sa = a.model.size ?? -1;
        const sb = b.model.size ?? -1;
        if (sa !== sb) return sb - sa; // largest first; unknown size (-1) sinks to bottom
        return listModelDisplayName(a.model).localeCompare(listModelDisplayName(b.model));
      });
    } else if (sortBy === 'last-used') {
      // Graceful fallback to name if last_used absent
      result.sort((a, b) => {
        const la: string | null = (a.model as any).last_used ?? null;
        const lb: string | null = (b.model as any).last_used ?? null;
        if (la && lb) return new Date(lb).getTime() - new Date(la).getTime();
        if (la) return -1;
        if (lb) return 1;
        return listModelDisplayName(a.model).localeCompare(listModelDisplayName(b.model));
      });
    } else if (sortBy === 'downloads') {
      // Graceful fallback to name if download_count absent
      result.sort((a, b) => {
        const da: number | null = (a.model as any).downloads ?? (a.model as any).download_count ?? null;
        const db: number | null = (b.model as any).downloads ?? (b.model as any).download_count ?? null;
        if (da !== null && db !== null) return db - da; // most downloads first
        if (da !== null) return -1;
        if (db !== null) return 1;
        return listModelDisplayName(a.model).localeCompare(listModelDisplayName(b.model));
      });
    }

    // Pinned models always float to the top, preserving the chosen sort order
    // within the pinned and unpinned groups. Client-local only; distinct from
    // favorites (which is a separate filter/count, not a sort).
    if (pinnedNames && pinnedNames.size > 0) {
      result.sort((a, b) => Number(b.pinned ?? false) - Number(a.pinned ?? false));
    }

    return result;
  }, [allModels, loadedNames, pulling, downloadItems, searchQuery, taskFilters, sortBy, pinnedNames, favoriteNames, primaryFilter, backendFilters, tagFilters, capabilityFilter, activeTextFilters]);

  // Funnel options: the union of functional capability tags present across the
  // models, in canonical order. Derived client-side from the model labels —
  // "Define the capability set from the mock model data" (fl0rianr #2424).
  const availableCapabilityTags = useMemo<CapabilityTag[]>(() => {
    const present = new Set<CapabilityTag>();
    for (const m of allModels) {
      if (!listModelName(m)) continue;
      for (const tag of modelCapabilityTags(m)) present.add(tag);
    }
    return CAPABILITY_TAG_ORDER.filter(tag => present.has(tag));
  }, [allModels]);

  const capabilityFilterSize = capabilityFilter?.size ?? 0;
  const activeFilterCount = capabilityFilterSize + activeTextFilters.length;

  const toggleCapability = useCallback((tag: CapabilityTag) => {
    if (!onCapabilityFilterChange) return;
    const next = new Set(capabilityFilter ?? new Set<string>());
    if (next.has(tag)) next.delete(tag); else next.add(tag);
    onCapabilityFilterChange(next);
  }, [capabilityFilter, onCapabilityFilterChange]);

  const addTextFilter = useCallback(() => {
    setTextFilters(prev => [...prev, createTextFilterRule()]);
  }, []);

  const updateTextFilter = useCallback((id: string, patch: Partial<TextFilterRule>) => {
    setTextFilters(prev => prev.map(rule => rule.id === id ? { ...rule, ...patch } : rule));
  }, []);

  const removeTextFilter = useCallback((id: string) => {
    setTextFilters(prev => prev.filter(rule => rule.id !== id));
  }, []);

  const clearAllFilters = useCallback(() => {
    onCapabilityFilterChange?.(new Set());
    setTextFilters([createTextFilterRule()]);
  }, [onCapabilityFilterChange]);

  const updateFilterPopoverPosition = useCallback(() => {
    const button = filterBtnRef.current;
    if (!button) return;

    const buttonRect = button.getBoundingClientRect();
    const viewportPadding = 12;
    const viewportWidth = window.innerWidth;
    const viewportHeight = window.innerHeight;
    const maxUsableWidth = Math.max(320, viewportWidth - viewportPadding * 2);
    const width = Math.min(440, maxUsableWidth);
    const top = Math.max(viewportPadding, buttonRect.bottom + 4);

    let left = buttonRect.right - width;
    if (left < viewportPadding) left = viewportPadding;
    if (left + width > viewportWidth - viewportPadding) {
      left = viewportWidth - viewportPadding - width;
    }

    setFilterPopoverStyle({
      left,
      top,
      width,
      maxHeight: Math.max(220, viewportHeight - top - viewportPadding),
    });
  }, []);

  useEffect(() => {
    if (!filterOpen) return;

    updateFilterPopoverPosition();
    window.addEventListener('resize', updateFilterPopoverPosition);
    window.addEventListener('scroll', updateFilterPopoverPosition, true);

    return () => {
      window.removeEventListener('resize', updateFilterPopoverPosition);
      window.removeEventListener('scroll', updateFilterPopoverPosition, true);
    };
  }, [filterOpen, updateFilterPopoverPosition]);

  useEffect(() => {
    if (!filterOpen) return;

    const handlePointerDown = (event: PointerEvent) => {
      const target = event.target as Node;
      if (filterBtnRef.current?.contains(target)) return;
      if (filterPopoverRef.current?.contains(target)) return;
      setFilterOpen(false);
    };

    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setFilterOpen(false);
    };

    document.addEventListener('pointerdown', handlePointerDown);
    document.addEventListener('keydown', handleKeyDown);

    return () => {
      document.removeEventListener('pointerdown', handlePointerDown);
      document.removeEventListener('keydown', handleKeyDown);
    };
  }, [filterOpen]);

  // Keyboard navigation on the list (ArrowUp/Down/Home/End)
  const handleListKeyDown = useCallback((e: React.KeyboardEvent) => {
    const options = listRef.current?.querySelectorAll<HTMLElement>('[role="option"]');
    if (!options?.length) return;

    const focusedEl = document.activeElement as HTMLElement;
    const items = Array.from(options);
    const currentIdx = items.indexOf(focusedEl);

    let next = -1;
    if (e.key === 'ArrowDown') { e.preventDefault(); next = currentIdx < 0 ? 0 : Math.min(currentIdx + 1, items.length - 1); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); next = currentIdx <= 0 ? 0 : currentIdx - 1; }
    else if (e.key === 'Home') { e.preventDefault(); next = 0; }
    else if (e.key === 'End') { e.preventDefault(); next = items.length - 1; }

    if (next >= 0) {
      items[next].focus();
      // Single-select listbox: arrow key navigation also selects (ARIA APG)
      const modelId = items[next].getAttribute('data-model-id');
      if (modelId) onSelectModel(modelId);
    }
  }, [onSelectModel]);

  const handleItemKeyDown = useCallback((e: React.KeyboardEvent<HTMLElement>, modelId: string) => {
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); onSelectModel(modelId); }
    else if ((e.key === 'p' || e.key === 'P') && onTogglePin) { e.preventDefault(); onTogglePin(modelId); }
  }, [onSelectModel, onTogglePin]);

  // Close filter popover on outside click
  const handleFilterBtnClick = () => setFilterOpen(v => !v);

  return (
    <WorkspaceListPanel
      className="model-list-panel"
      headerClassName="manager__title"
      title="Models"
      subtitle={`${flatList.length} ${flatList.length === 1 ? 'model' : 'models'}`}
      actions={(
        <WorkspaceActionGroup label="Model list actions">
          {onOpenCustomModels && (
            <WorkspaceActionButton
              appearance="primary"
              size="toolbar"
              icon="compose"
              iconOnly
              onClick={onOpenCustomModels}
              aria-label="Open custom models"
              title="Manage custom models"
            />
          )}
          {onOpenRouter && (
            <WorkspaceActionButton
              size="toolbar"
              icon="router"
              iconOnly
              onClick={onOpenRouter}
              aria-label="Open router editor"
              title="Create or edit a model router"
            />
          )}
          {onOpenGlobalSettings && (
            <WorkspaceActionButton
              size="toolbar"
              icon="settings"
              iconOnly
              onClick={onOpenGlobalSettings}
              aria-label="Open global model settings"
              title="Global model settings"
            />
          )}
        </WorkspaceActionGroup>
      )}
    >

      {/* Search bar */}
      <div className="model-list-panel__search-row">
        <label htmlFor="model-list-search" className="sr-only">Search models</label>
        <div className="model-list-panel__search-wrap">
          <Icon name="search" size={14} aria-hidden="true" className="model-list-panel__search-icon" />
          <input
            id="model-list-search"
            ref={inputRef as React.RefObject<HTMLInputElement>}
            role="searchbox"
            type="text"
              className="model-list-panel__search-input manager__search-input"
            placeholder="Search models… (Ctrl+K)"
            value={searchQuery}
            onChange={e => onSearchChange(e.target.value)}
            aria-label="Search models"
            autoComplete="off"
          />
          {searchQuery && (
            <button
              type="button"
              className="model-list-panel__search-clear"
              onClick={() => onSearchChange('')}
              aria-label="Clear search"
            >×</button>
          )}
        </div>
        {/* Funnel filter button */}
        <div className="model-list-panel__filter-wrap">
          <button
            ref={filterBtnRef}
            type="button"
            className={`model-list-panel__filter-btn${filterOpen ? ' model-list-panel__filter-btn--open' : ''}${activeFilterCount > 0 ? ' model-list-panel__filter-btn--active' : ''}`}
            onClick={handleFilterBtnClick}
            aria-label={activeFilterCount > 0 ? `Filter models (${activeFilterCount} active)` : 'Filter models'}
            aria-expanded={filterOpen}
            aria-haspopup="dialog"
          >
            <Icon name="funnel" size={14} aria-hidden="true" />
          </button>

          {filterOpen && (
            <div
              ref={filterPopoverRef}
              className="model-list-panel__filter-popover"
              style={filterPopoverStyle ?? undefined}
              role="dialog"
              aria-label="Model filters"
              aria-modal="false"
            >
              <div className="model-list-panel__filter-popover-head">
                <span>Filters</span>
                <button
                  type="button"
                  className="model-list-panel__filter-popover-close"
                  onClick={() => setFilterOpen(false)}
                  aria-label="Close filter panel"
                >
                  <Icon name="x" size={13} />
                </button>
              </div>

              <div className="model-list-panel__filter-section">
                <div className="model-list-panel__filter-section-title">Capabilities</div>
                <div className="model-list-panel__filter-options" role="group" aria-label="Model capability filter">
                  {availableCapabilityTags.length === 0 && (
                    <span className="model-list-panel__filter-empty">No capabilities available</span>
                  )}
                  {availableCapabilityTags.map(tag => {
                    const active = capabilityFilter?.has(tag) ?? false;
                    return (
                      <button
                        key={tag}
                        type="button"
                        className={`model-list-panel__filter-option${active ? ' model-list-panel__filter-option--active' : ''}`}
                        onClick={() => toggleCapability(tag)}
                        aria-pressed={active}
                      >
                        <CapabilityIcon capability={capabilityTagIconTarget(tag) as any} size={12} aria-hidden="true" />
                        {CAPABILITY_TAG_LABELS[tag]}
                      </button>
                    );
                  })}
                </div>
              </div>

              <div className="model-list-panel__filter-section">
                <div className="model-list-panel__filter-section-head">
                  <span className="model-list-panel__filter-section-title">Text</span>
                  <button
                    type="button"
                    className="model-list-panel__filter-add"
                    onClick={addTextFilter}
                  >
                    <Icon name="plus" size={13} aria-hidden="true" />
                    <span>Add</span>
                  </button>
                </div>

                <div className="model-list-panel__text-filters" role="group" aria-label="Custom text filters">
                  {textFilters.map((rule, index) => (
                    <div key={rule.id} className="model-list-panel__text-filter">
                      <label className="sr-only" htmlFor={`${rule.id}-field`}>Filter field</label>
                      <select
                        id={`${rule.id}-field`}
                        className="model-list-panel__text-filter-field"
                        value={rule.field}
                        onChange={e => updateTextFilter(rule.id, { field: e.target.value as TextFilterField })}
                        aria-label={`Filter ${index + 1} field`}
                      >
                        {TEXT_FILTER_FIELDS.map(field => (
                          <option key={field.key} value={field.key}>{field.label}</option>
                        ))}
                      </select>

                      <div
                        className="model-list-panel__text-filter-mode"
                        role="group"
                        aria-label={`Filter ${index + 1} mode`}
                      >
                        <button
                          type="button"
                          className={`model-list-panel__text-filter-mode-btn${rule.mode === 'include' ? ' model-list-panel__text-filter-mode-btn--active' : ''}`}
                          onClick={() => updateTextFilter(rule.id, { mode: 'include' })}
                          aria-label={`Filter ${index + 1}: include matching models`}
                          aria-pressed={rule.mode === 'include'}
                          title="Include matches"
                        >
                          <Icon name="eye" size={13} aria-hidden="true" />
                        </button>
                        <button
                          type="button"
                          className={`model-list-panel__text-filter-mode-btn${rule.mode === 'exclude' ? ' model-list-panel__text-filter-mode-btn--active' : ''}`}
                          onClick={() => updateTextFilter(rule.id, { mode: 'exclude' })}
                          aria-label={`Filter ${index + 1}: exclude matching models`}
                          aria-pressed={rule.mode === 'exclude'}
                          title="Exclude matches"
                        >
                          <Icon name="eye-off" size={13} aria-hidden="true" />
                        </button>
                      </div>

                      <label className="sr-only" htmlFor={`${rule.id}-value`}>Filter text</label>
                      <input
                        id={`${rule.id}-value`}
                        className="model-list-panel__text-filter-input"
                        type="text"
                        value={rule.value}
                        onChange={e => updateTextFilter(rule.id, { value: e.target.value })}
                        placeholder="e.g. qwen"
                        aria-label={`Filter ${index + 1} text`}
                        autoComplete="off"
                      />

                      <button
                        type="button"
                        className="model-list-panel__text-filter-remove"
                        onClick={() => removeTextFilter(rule.id)}
                        aria-label={`Remove filter ${index + 1}`}
                      >
                        <Icon name="x" size={13} aria-hidden="true" />
                      </button>
                    </div>
                  ))}
                </div>
              </div>

              {activeFilterCount > 0 && (
                <button
                  type="button"
                  className="model-list-panel__filter-clear"
                  onClick={clearAllFilters}
                >
                  Clear
                </button>
              )}
            </div>
          )}
        </div>
      </div>

      {/* Sort control */}
      <div className="model-list-panel__sort-row">
        <label htmlFor="model-list-sort" className="model-list-panel__sort-label">Sort</label>
        <select
          id="model-list-sort"
          className="model-list-panel__sort-select"
          value={sortBy}
          onChange={e => setSortBy(e.target.value as SortBy)}
          aria-label="Sort models by"
        >
          <option value="name">Name (A–Z)</option>
          <option value="size">Size (largest first)</option>
          <option value="last-used">Last used</option>
          <option value="downloads">Download count</option>
        </select>
      </div>

      <span className="sr-only model-list-panel__count" aria-live="polite" aria-atomic="true">
        {flatList.length} model{flatList.length !== 1 ? 's' : ''}
        {taskFilters && taskFilters.size > 0 && ` (${Array.from(taskFilters).map(task => FILTER_TABS.find(item => item.key === task)?.label || task).join(', ')})`}
      </span>

      {/* Scrollable area: model list + optional inline registry result zones */}
      <div className="model-list-panel__scroll-area">
      {/* Elevated registry zones: shown above the list when no local results match */}
      {registryZoneTop}
      {/* Model list */}
      <ul
        ref={listRef}
        className="model-list-panel__list"
        role="listbox"
        aria-label="Model list"
        aria-multiselectable="false"
        tabIndex={flatList.some(e => e.model && listModelName(e.model) === selectedModelId) ? -1 : 0}
        onKeyDown={handleListKeyDown}
      >
        {flatList.map(({ model, status, downloadPct, pinned }) => {
          const mId = listModelName(model);
          const displayName = listModelDisplayName(model);
          const recipe = String((model as any).recipe || '');
          const neutralCollectionGuide = modelIsOmni(model) || modelIsRouter(model);
          const displayedBackend = recipe && !neutralCollectionGuide ? modelListBackendLabel(recipe) : '';
          const isSelected = mId === selectedModelId;
          const capTags = modelCapabilityTags(model);
          const backendStyle = recipe && !neutralCollectionGuide
            ? ({ '--list-backend-color': backendColor(recipe) } as React.CSSProperties)
            : undefined;
          const backendReadiness = status === 'downloaded'
            ? modelBackendReadiness(model, systemInfo)
            : null;
          const readinessLabel = status === 'running'
            ? 'Backend active; model is running.'
            : status === 'downloading'
              ? `Model download in progress${downloadPct != null ? ` (${downloadPct.toFixed(0)}%).` : '.'}`
              : status === 'available'
                ? 'Model is available to download.'
                : backendReadiness?.label;
          const statusTone = status === 'running'
            ? 'running'
            : status === 'downloading'
              ? 'downloading'
              : status === 'downloaded'
                ? backendReadiness?.tone || 'unknown'
                : 'available';

          return (
            <li
              key={mId}
              role="option"
              tabIndex={isSelected ? 0 : -1}
              aria-selected={isSelected}
              data-model-id={mId}
              style={backendStyle}
              aria-keyshortcuts={onTogglePin ? 'P' : undefined}
              className={`model-list-item${isSelected ? ' model-list-item--selected' : ''}${pinned ? ' model-list-item--pinned' : ''}${neutralCollectionGuide ? ' model-list-item--neutral-guide' : ''} model-list-item--${status}`}
              onClick={() => onSelectModel(mId)}
              onKeyDown={e => handleItemKeyDown(e, mId)}
              aria-label={`${displayName}${pinned ? ', pinned' : ''}${status === 'running' ? ', running' : status === 'downloaded' ? ', downloaded' : status === 'downloading' ? ', downloading' : ', available'}${displayedBackend ? `, ${displayedBackend}` : ''}${readinessLabel ? `, ${readinessLabel}` : ''}`}
            >
              {/* Name + meta stay left-aligned across every row. */}
              <span className="model-list-item__body">
                <span className="model-list-item__name">{displayName}</span>
                <span className="model-list-item__meta">
                  {model.size != null && model.size > 0 && (
                    <span className="model-list-item__size">{listFmtSize(model.size)}</span>
                  )}
                  <span className="model-list-item__caps" role="img" aria-label={`Capabilities: ${capTags.map(t => CAPABILITY_TAG_LABELS[t]).join(', ')}`}>
                    {capTags.map(tag => (
                      <span key={tag} className="model-list-item__cap" title={CAPABILITY_TAG_LABELS[tag]}>
                        <CapabilityIcon capability={capabilityTagIconTarget(tag) as any} size={12} aria-hidden="true" />
                      </span>
                    ))}
                  </span>
                </span>
              </span>

              {/* The backend identity now lives on the lower guide line instead
                  of in a separate badge. The line terminates in the compact
                  readiness ring; the pin sits directly above it. */}
              <span className="model-list-item__footer" aria-hidden="true">
                <span className="model-list-item__footer-info">
                  {status === 'downloading' && downloadPct != null && (
                    <span className="model-list-item__pct">{downloadPct.toFixed(0)}%</span>
                  )}
                  {recipe && !neutralCollectionGuide && (
                    <span
                      className="model-list-item__backend"
                      title={backendLabel(recipe)}
                    >
                      {displayedBackend}
                    </span>
                  )}
                </span>
                <span
                  className={`model-list-item__status model-list-item__status--${statusTone}`}
                  title={readinessLabel}
                  data-backend-state={backendReadiness?.state || statusTone}
                >
                  {status !== 'available' && (
                    <span className={`model-list-item__dot model-list-item__dot--${statusTone}`} />
                  )}
                </span>
              </span>

              {/* Pointer users click the compact pin above the status terminus;
                  keyboard and assistive-technology users retain the P shortcut. */}
              {onTogglePin && (
                <span
                  className={`model-list-item__pin row__pin${pinned ? ' row__pin--active model-list-item__pin--active' : ''}`}
                  onClick={e => { e.stopPropagation(); onTogglePin(mId); }}
                  aria-hidden="true"
                  title={pinned ? `Unpin ${displayName} (P)` : `Pin ${displayName} (P)`}
                >
                  <Icon name="pin" size={12} aria-hidden="true" />
                </span>
              )}
            </li>
          );
        })}

        {/* Search-no-match feedback stays in the middle list. The "no model
            selected" / empty-registry placeholder now lives in the RIGHT detail
            pane (ModelDetailPanel) per fl0rianr #2424 — it must NOT leak into the
            top of the model list. */}
        {flatList.length === 0 && searchQuery && !registryZoneTop && (
          <li className="model-list-panel__empty manager__empty" aria-live="polite">
            <Icon name="search" size={18} aria-hidden="true" />
            <span>No models match your search.</span>
          </li>
        )}
      </ul>
      {registryZone && registryResultCount > 0 && flatList.length > 0 && (
        <button
          type="button"
          className="hf-zone-anchor"
          onClick={() => {
            document.querySelector(".zone--registry")?.scrollIntoView({ behavior: "smooth", block: "start" });
          }}
          aria-label={`Scroll to ${registryResultCount} remote model result${registryResultCount !== 1 ? "s" : ""}`}
        >
          ↓ {registryResultCount} remote result{registryResultCount !== 1 ? "s" : ""}
        </button>
      )}
      {registryZone}
      </div>
    </WorkspaceListPanel>
  );
};

export type { FilterTab };
export default ModelListPanel;
