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

export function listRecipeBadgeText(recipe: string): string {
  const n = String(recipe || '').toLowerCase();
  switch (n) {
    case 'llamacpp': return 'llama.cpp';
    case 'vllm': return 'vLLM';
    case 'flm': return 'FLM';
    case 'ryzenai-llm': return 'RyzenAI';
    case 'sd-cpp': return 'SD.cpp';
    case 'whispercpp': return 'Whisper';
    case 'moonshine': return 'Moonshine';
    case 'kokoro': return 'Kokoro';
    case 'acestep': return 'ACE-Step';
    case 'thinksound': return 'ThinkSound';
    case 'openmoss': return 'OpenMOSS';
    case 'trellis': return 'TRELLIS.2';
    case 'collection.omni': return 'Omni';
    case 'collection': return 'Collection';
    default: return recipe || 'Backend';
  }
}

function listRecipeColor(recipe: string): string {
  const n = String(recipe || '').toLowerCase();
  switch (n) {
    case 'llamacpp': return '#facc15';
    case 'vllm': return '#60a5fa';
    case 'flm': return '#34d399';
    case 'ryzenai-llm': return '#f97316';
    case 'sd-cpp': return '#c084fc';
    case 'whispercpp': return '#38bdf8';
    case 'moonshine': return '#22d3ee';
    case 'kokoro': return '#f472b6';
    case 'acestep': return '#fb7185';
    case 'thinksound': return '#f59e0b';
    case 'openmoss': return '#ec4899';
    case 'trellis': return '#818cf8';
    case 'collection.omni': return '#a78bfa';
    case 'collection': return '#94a3b8';
    default: return 'var(--text-tertiary)';
  }
}

type FilterTab = 'all' | 'llm' | 'omni' | 'image' | 'audio' | 'audio-generation' | 'tts' | 'model3d' | 'embedding';

const FILTER_TABS: Array<{ key: FilterTab; label: string; iconName: IconName }> = [
  { key: 'all', label: 'All', iconName: 'globe' },
  { key: 'llm', label: 'LLM', iconName: 'chat' },
  { key: 'omni', label: 'Omni', iconName: 'omni' },
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

export function modelMatchesFilter(m: ModelInfo, filter: FilterTab): boolean {
  if (filter === 'all') return true;
  const cap = capabilityFromModelInfo(m);
  if (filter === 'omni') {
    const recipe = String((m as any).recipe || '').toLowerCase();
    return recipe === 'collection.omni' || recipe === 'collection';
  }
  if (filter === 'embedding') return cap === 'embedding' || cap === 'reranking';
  if (filter === 'llm') return cap === 'chat' || cap === 'unknown';
  return (cap as string) === filter;
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

/** Custom / user-registered models (client-local store). */
export function modelIsCustom(m: ModelInfo): boolean {
  return (m as any).custom === true;
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

/** Backend filter — `backend` is 'all' or a lowercased recipe id. */
export function modelMatchesBackend(m: ModelInfo, backend: string): boolean {
  if (!backend || backend === 'all') return true;
  return String((m as any).recipe || '').toLowerCase() === backend;
}

/** Curated tag chips (model families + size hints) shown in the left rail. */
export const TAG_CHIPS: string[] = ['Llama', 'Qwen', 'Phi', 'Mistral', 'Gemma', 'Bonsai', 'Small'];

/** A tag matches when it appears in the model's labels OR its name/family. */
export function modelMatchesTag(m: ModelInfo, tag: string | null): boolean {
  if (!tag) return true;
  const t = tag.toLowerCase();
  const labels = (m.labels || []).map(l => String(l).toLowerCase());
  if (labels.includes(t)) return true;
  const hay = `${listModelName(m)} ${m.display_name || ''}`.toLowerCase();
  return hay.includes(t);
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
  filterTab: FilterTab;
  onFilterChange: (tab: FilterTab) => void;
  /** Selected functional capability tags (multi-select funnel). Empty = no filter. */
  capabilityFilter?: Set<string>;
  onCapabilityFilterChange?: (next: Set<string>) => void;
  /** Primary nav bucket selected in the left rail. */
  primaryFilter?: PrimaryFilter;
  /** Backend filter ('all' or lowercased recipe id) from the left rail. */
  backendFilter?: string;
  /** Active tag chip from the left rail (null = no tag filter). */
  tagFilter?: string | null;
  searchInputRef?: React.RefObject<HTMLInputElement | null>;
  onOpenCustomModels?: () => void;
  /** Lowercased set of pinned model names. Pinned rows float to the top. Client-local. */
  pinnedNames?: Set<string>;
  /** Toggle a model's pinned state. Receives the model name. */
  onTogglePin?: (name: string) => void;
  /** Lowercased set of favorited model names (distinct from pinned). Client-local. */
  favoriteNames?: Set<string>;
  /** Optional content rendered below the model list in the shared scroll area (e.g. HF results). */
  hfZone?: React.ReactNode;
  /** Elevated HF zone rendered above the list when no local results match the query. */
  hfZoneTop?: React.ReactNode;
  /** Number of HF results for the anchor bar count (used when hfZone is at the bottom). */
  hfResultCount?: number;
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
  filterTab,
  onFilterChange,
  capabilityFilter,
  onCapabilityFilterChange,
  primaryFilter = 'all',
  backendFilter = 'all',
  tagFilter = null,
  searchInputRef,
  onOpenCustomModels,
  pinnedNames,
  onTogglePin,
  favoriteNames,
  hfZone,
  hfZoneTop,
  hfResultCount = 0,
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

      // Filter by type
      if (!modelMatchesFilter(m, filterTab)) continue;

      // Left-rail filter dimensions (primary bucket / backend / tag)
      if (!modelMatchesPrimary(m, primaryFilter, loadedNames, favoriteNames)) continue;
      if (!modelMatchesBackend(m, backendFilter)) continue;
      if (!modelMatchesTag(m, tagFilter)) continue;

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
  }, [allModels, loadedNames, pulling, downloadItems, searchQuery, filterTab, sortBy, pinnedNames, favoriteNames, primaryFilter, backendFilter, tagFilter, capabilityFilter, activeTextFilters]);

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
    <div className="model-list-panel">
      {/* Title */}
      <div className="model-list-panel__title manager__title">
        <h1>Models</h1>
        {onOpenCustomModels && (
          <button
            type="button"
            className="model-list-panel__custom-menu-btn"
            onClick={onOpenCustomModels}
            aria-label="Open custom models"
            title="Manage custom models"
          >
            <Icon name="user-round-cog" size={19} aria-hidden="true" />
          </button>
        )}
      </div>

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

      {/* List count */}
      <div className="model-list-panel__count" aria-live="polite" aria-atomic="true">
        {flatList.length} model{flatList.length !== 1 ? 's' : ''}
        {filterTab !== 'all' && ` (${FILTER_TABS.find(t => t.key === filterTab)?.label})`}
      </div>

      {/* Scrollable area: model list + optional inline HF results zone */}
      <div className="model-list-panel__scroll-area">
      {/* Elevated HF zone: shown above list when no local results match */}
      {hfZoneTop}
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
          const isSelected = mId === selectedModelId;
          const capTags = modelCapabilityTags(model);

          return (
            <li
              key={mId}
              role="option"
              tabIndex={isSelected ? 0 : -1}
              aria-selected={isSelected}
              data-model-id={mId}
              aria-keyshortcuts={onTogglePin ? 'P' : undefined}
              className={`model-list-item${isSelected ? ' model-list-item--selected' : ''}${pinned ? ' model-list-item--pinned' : ''} model-list-item--${status}`}
              onClick={() => onSelectModel(mId)}
              onKeyDown={e => handleItemKeyDown(e, mId)}
              aria-label={`${displayName}${pinned ? ', pinned' : ''}${status === 'running' ? ', running' : status === 'downloaded' ? ', downloaded' : status === 'downloading' ? ', downloading' : ', available'}${recipe ? `, ${listRecipeBadgeText(recipe)}` : ''}`}
            >
              {/* Backend badge */}
              {recipe && (
                <span
                  className="model-list-item__backend"
                  style={{ '--list-backend-color': listRecipeColor(recipe) } as React.CSSProperties}
                  aria-hidden="true"
                >
                  {listRecipeBadgeText(recipe)}
                </span>
              )}

              {/* Name + meta */}
              <span className="model-list-item__body">
                <span className="model-list-item__name">{displayName}</span>
                <span className="model-list-item__meta">
                  {model.size != null && model.size > 0 && (
                    <span className="model-list-item__size">{listFmtSize(model.size)}</span>
                  )}
                  <span className="model-list-item__caps" role="img" aria-label={`Capabilities: ${capTags.map(t => CAPABILITY_TAG_LABELS[t]).join(', ')}`}>
                    {capTags.map(tag => (
                      <span key={tag} className="model-list-item__cap" title={CAPABILITY_TAG_LABELS[tag]}>
                        <CapabilityIcon capability={capabilityTagIconTarget(tag) as any} size={10} aria-hidden="true" />
                      </span>
                    ))}
                  </span>
                </span>
              </span>

              {/* Status indicator */}
              <span className="model-list-item__status" aria-hidden="true">
                {status === 'running' && <span className="row__pulse" />}
                {status === 'downloading' && downloadPct != null && (
                  <span className="model-list-item__pct">{downloadPct.toFixed(0)}%</span>
                )}
                {status === 'downloaded' && <span className="model-list-item__dot model-list-item__dot--ready" />}
              </span>

              {/* Pin / favorite (client-local). Rendered as a non-button so it
                  does not nest an interactive control inside role="option"
                  (axe nested-interactive). Pointer users click it; keyboard/AT
                  users toggle via the "P" shortcut on the focused row, and the
                  pinned state is exposed in the row's aria-label. */}
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
        {flatList.length === 0 && searchQuery && !hfZoneTop && (
          <li className="model-list-panel__empty manager__empty" aria-live="polite">
            <Icon name="search" size={18} aria-hidden="true" />
            <span>No models match your search.</span>
          </li>
        )}
      </ul>
      {hfZone && hfResultCount > 0 && flatList.length > 0 && (
        <button
          type="button"
          className="hf-zone-anchor"
          onClick={() => {
            document.querySelector(".zone--hf")?.scrollIntoView({ behavior: "smooth", block: "start" });
          }}
          aria-label={`Scroll to ${hfResultCount} HuggingFace result${hfResultCount !== 1 ? "s" : ""}`}
        >
          ↓ {hfResultCount} result{hfResultCount !== 1 ? "s" : ""} on HuggingFace
        </button>
      )}
      {hfZone}
      </div>
    </div>
  );
};

export type { FilterTab };
export default ModelListPanel;
