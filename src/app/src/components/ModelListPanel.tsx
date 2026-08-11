/**
 * ModelListPanel — left panel of the master-detail model view.
 * Compact, searchable list of models with keyboard navigation.
 *
 * Part of the master-detail layout introduced in #2355 Slice 1.
 */
import React, { useCallback, useRef, useMemo, useState } from 'react';
import type { ModelInfo, LoadedModel } from '../api';
import {
  capabilityFromModelInfo,
  modelCapabilityTags,
  CAPABILITY_TAG_LABELS,
  type CapabilityTag,
} from '../modelCapabilities';
import { Icon, CapabilityIcon } from './Icon';
import type { IconName } from './Icon';
import type { CapabilityIconTarget } from './Icon';
import { activeDownloadForModel, type DownloadListItem } from '../features/downloadManager/downloadStore';
import { WorkspaceActionButton, WorkspaceActionGroup, WorkspaceListPanel } from './WorkspacePanels';
import { backendCompactLabel, backendLabel } from '../modelPresentation';

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
  onlineSearchEnabled: boolean;
  /** Selected left-rail tasks. Empty means all; selections are OR-ed. */
  taskFilters?: ReadonlySet<FilterTab>;
  /** Primary nav bucket selected in the left rail. */
  primaryFilter?: PrimaryFilter;
  /** Selected backend recipes. Empty means all; selections are OR-ed. */
  backendFilters?: ReadonlySet<string>;
  /** Selected built-in/custom tags. Empty means all; selections are OR-ed. */
  tagFilters?: ReadonlySet<string>;
  searchInputRef?: React.RefObject<HTMLInputElement | null>;
  onOpenCustomModels?: () => void;
  onOpenRouter?: () => void;
  onUpdateAllModels?: () => void;
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
  onlineSearchEnabled,
  taskFilters,
  primaryFilter = 'all',
  backendFilters,
  tagFilters,
  searchInputRef,
  onOpenCustomModels,
  onOpenRouter,
  onUpdateAllModels,
  pinnedNames,
  onTogglePin,
  favoriteNames,
  registryZone,
  registryZoneTop,
  registryResultCount = 0,
  systemInfo = null,
}) => {
  const [sortBy, setSortBy] = useState<SortBy>('name');
  const listRef = useRef<HTMLUListElement>(null);
  const defaultSearchRef = useRef<HTMLInputElement>(null);
  const inputRef = (searchInputRef ?? defaultSearchRef) as React.RefObject<HTMLInputElement>;

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
  }, [allModels, loadedNames, pulling, downloadItems, searchQuery, taskFilters, sortBy, pinnedNames, favoriteNames, primaryFilter, backendFilters, tagFilters]);

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
          {onUpdateAllModels && (
            <WorkspaceActionButton
              size="toolbar"
              icon="rotate-ccw"
              iconOnly
              onClick={onUpdateAllModels}
              aria-label="Update all models"
              title="Update all downloaded models"
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
            placeholder={onlineSearchEnabled ? 'Search built-in and online catalogs…' : 'Search built-in catalogs…'}
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

              {/* The backend identity lives on the lower guide line. A hollow
                  marker means remote/available; every concrete readiness state
                  replaces that ring with one solid status point. */}
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
                />
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
