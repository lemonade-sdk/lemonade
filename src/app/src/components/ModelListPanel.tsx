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
  rowCapability,
  type CapabilityTag,
} from '../modelCapabilities';
import { Icon } from './Icon';
import type { IconName } from './Icon';
import type { CapabilityIconTarget } from './Icon';
import { activeDownloadForModel, type DownloadListItem } from '../features/downloadManager/downloadStore';
import {
  WorkspaceActionButton,
  WorkspaceActionGroup,
  WorkspaceList,
  WorkspaceListPanel,
  WorkspaceListGroup,
  WorkspaceListRow,
  type WorkspaceListRowStatus,
} from './WorkspacePanels';
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

/** Short enough to ride the row's meta line; `label` keeps the full sentence
    for the status marker's tooltip and the row's accessible name. */
const BACKEND_STATE_MESSAGES: Record<string, string> = {
  missing: 'Engine not installed',
  update_required: 'Engine update required',
  update_available: 'Engine update available',
  installable: 'Engine download required',
  action_required: 'Engine needs attention',
  unsupported: 'Engine unsupported',
};

function backendReadinessMessage(readiness: ModelBackendReadiness): string {
  return BACKEND_STATE_MESSAGES[readiness.state || ''] || 'Engine needs attention';
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
export const TAG_CHIPS: string[] = ['Recommended', 'Hot', 'Llama', 'Qwen', 'Phi', 'Mistral', 'Gemma', 'Bonsai', 'Small'];

export function modelIsRecommended(m: ModelInfo): boolean {
  const raw = m as any;
  if (raw.recommended === true || raw.is_recommended === true || raw.featured === true || raw.suggested === true) return true;
  const labels = [
    ...(Array.isArray(raw.labels) ? raw.labels : []),
    ...(Array.isArray(raw.tags) ? raw.tags : []),
  ].map(value => String(value).trim().toLowerCase());
  return labels.some(label => ['recommended', 'featured', 'suggested'].includes(label));
}

/**
 * Hot is server metadata, not a fuzzy family/name tag. Current lemond builds
 * expose it as a label; capability-aware sources may expose it through the
 * capabilities array. Match both exact metadata forms so a model named
 * something like "hotpot" does not become Hot accidentally.
 */
export function modelIsHot(m: ModelInfo): boolean {
  const raw = m as any;
  const metadata = [
    ...(Array.isArray(raw.capabilities) ? raw.capabilities : []),
    ...(Array.isArray(raw.labels) ? raw.labels : []),
    ...(Array.isArray(raw.tags) ? raw.tags : []),
  ].map(value => String(value).trim().toLowerCase());
  return metadata.includes('hot');
}

/** A tag matches model metadata, labels, or its name/family. */
export function modelMatchesTag(m: ModelInfo, tag: string | null): boolean {
  if (!tag) return true;
  const t = tag.trim().toLowerCase();
  if (!t) return true;
  if (t === 'recommended') return modelIsRecommended(m);
  if (t === 'hot') return modelIsHot(m);
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

    return result;
  }, [allModels, loadedNames, pulling, downloadItems, searchQuery, taskFilters, sortBy, pinnedNames, favoriteNames, primaryFilter, backendFilters, tagFilters]);

  /* Whether a model is on this machine is the first thing someone new to local
     AI needs, and it used to be repeated as a status on every single row. As a
     section it reads once. Grouping is structural rather than a by-product of
     the default sort, so the headings stay true under every sort order — the
     chosen sort now orders rows within each section. */
  const listSections = useMemo(() => {
    const sections: { key: string; label: string; entries: FlatModelEntry[] }[] = [
      { key: 'pinned', label: 'Pinned', entries: [] },
      { key: 'downloaded', label: 'Downloaded', entries: [] },
      { key: 'available', label: 'Not downloaded', entries: [] },
    ];
    const byKey = Object.fromEntries(sections.map(s => [s.key, s]));
    for (const entry of flatList) {
      /* A download in flight has not arrived yet, so it stays with the rows that
         are not on this machine; its own busy status and progress hairline are
         what say it is on the way. */
      const onThisMachine = entry.status !== 'available' && entry.status !== 'downloading';
      const key = entry.pinned ? 'pinned' : onThisMachine ? 'downloaded' : 'available';
      byKey[key].entries.push(entry);
    }
    return sections.filter(section => section.entries.length > 0);
  }, [flatList]);

  // Arrow/Home/End and Enter/Space live in WorkspaceList; the catalog only adds
  // its own pin shortcut.
  const handleItemKeyDown = useCallback((e: React.KeyboardEvent<HTMLElement>, modelId: string) => {
    if ((e.key === 'p' || e.key === 'P') && onTogglePin) { e.preventDefault(); onTogglePin(modelId); }
  }, [onTogglePin]);

  /* One catalog row. Hoisted out of the section map so the body reads at its
     own indentation rather than three levels in. */
  const renderModelRow = ({ model, status, downloadPct, pinned }: FlatModelEntry) => {
    const mId = listModelName(model);
    const displayName = listModelDisplayName(model);
    const recipe = String((model as any).recipe || '');
    const primaryCapability = rowCapability(model);
    // A collection routes to backends rather than being one, so it has no
    // engine to name on the meta line.
    const neutralCollectionGuide = primaryCapability === 'omni' || primaryCapability === 'router';
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

    // Only a row doing something, or asking for something, says so. Being
    // downloaded is a fact about the section it sits in, not about the row.
    const rowStatus: WorkspaceListRowStatus | undefined = status === 'running'
      ? 'live'
      : status === 'downloading'
        ? 'busy'
        : backendReadiness?.tone === 'attention'
          ? 'attention'
          : undefined;
    const statusText = rowStatus === 'busy'
      ? `Downloading${downloadPct != null ? ` ${downloadPct.toFixed(0)}%` : '…'}`
      : rowStatus === 'attention'
        ? backendReadinessMessage(backendReadiness!)
        : rowStatus === 'live'
          ? 'Running'
          : undefined;
    const meta = model.size != null && model.size > 0 ? listFmtSize(model.size) : undefined;
    const secondaryTags = capTags.filter(tag => tag !== (primaryCapability as string));

    return (
      <WorkspaceListRow
        key={mId}
        rowId={mId}
        capability={primaryCapability}
        title={displayName}
        meta={meta}
        glyphs={secondaryTags.map(capabilityTagIconTarget)}
        anchor={recipe && !neutralCollectionGuide ? displayedBackend : undefined}
        anchorTitle={recipe ? backendLabel(recipe) : undefined}
        status={rowStatus}
        statusText={statusText}
        statusLabel={readinessLabel}
        progress={status === 'downloading' ? downloadPct : undefined}
        selected={isSelected}
        tabIndex={isSelected ? 0 : -1}
        dataAttributes={{ 'data-model-id': mId }}
        className={pinned ? 'workspace-list-row--pinned' : undefined}
        ariaKeyShortcuts={onTogglePin ? 'P' : undefined}
        ariaLabel={`${displayName}${pinned ? ', pinned' : ''}${status === 'running' ? ', running' : status === 'downloading' ? ', downloading' : ''}${displayedBackend ? `, ${displayedBackend}` : ''}${readinessLabel ? `, ${readinessLabel}` : ''}`}
        onClick={() => onSelectModel(mId)}
        onKeyDown={e => handleItemKeyDown(e, mId)}
        action={onTogglePin ? {
          icon: 'pin',
          label: pinned ? `Unpin ${displayName} (P)` : `Pin ${displayName} (P)`,
          onClick: () => onTogglePin(mId),
          // A pinned model's pin outranks its engine as the fact worth showing,
          // and keeps the row's state visible without hovering. The row owns the
          // advertised P keyboard shortcut, so the nested affordance is pointer-only.
          pointerOnly: true,
          latched: pinned,
        } : undefined}
      />
    );
  };

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
      <WorkspaceList
        listRef={listRef}
        className="model-list-panel__list"
        label="Model list"
        tabIndex={flatList.some(e => e.model && listModelName(e.model) === selectedModelId) ? -1 : 0}
        onRowActivate={onSelectModel}
        activateOnMove
      >
        {listSections.map(section => (
          <WorkspaceListGroup key={section.key} label={section.label} count={section.entries.length}>
            {section.entries.map(renderModelRow)}
          </WorkspaceListGroup>
        ))}

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
      </WorkspaceList>
      {registryZone}
      </div>
    </WorkspaceListPanel>
  );
};

export type { FilterTab };
export default ModelListPanel;
