/**
 * ModelListPanel — left panel of the master-detail model view.
 * Compact, searchable, filterable list of models with keyboard navigation.
 *
 * Part of the master-detail layout introduced in #2355 Slice 1.
 */
import React, { useCallback, useRef, useMemo, useState } from 'react';
import type { ModelInfo, LoadedModel } from '../api';
import { capabilityFromModelInfo } from '../modelCapabilities';
import { Icon, CapabilityIcon } from './Icon';
import type { IconName } from './Icon';
import { activeDownloadForModel, type DownloadListItem } from '../features/downloadManager/downloadStore';

/* ── Helpers ─────────────────────────────────────────────────── */

function listModelName(m: ModelInfo): string {
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

function listRecipeBadgeText(recipe: string): string {
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
    case 'collection.omni': return '#a78bfa';
    case 'collection': return '#94a3b8';
    default: return 'var(--text-tertiary)';
  }
}

type FilterTab = 'all' | 'llm' | 'omni' | 'image' | 'audio' | 'tts' | 'embedding';

const FILTER_TABS: Array<{ key: FilterTab; label: string; iconName: IconName }> = [
  { key: 'all', label: 'All', iconName: 'globe' },
  { key: 'llm', label: 'LLM', iconName: 'chat' },
  { key: 'omni', label: 'Omni', iconName: 'omni' },
  { key: 'image', label: 'Image', iconName: 'image' },
  { key: 'audio', label: 'Audio', iconName: 'audio' },
  { key: 'tts', label: 'TTS', iconName: 'tts' },
  { key: 'embedding', label: 'Embed', iconName: 'embedding' },
];

function modelMatchesFilter(m: ModelInfo, filter: FilterTab): boolean {
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

/* ── Types ───────────────────────────────────────────────────── */

export type SortBy = 'name' | 'size' | 'last-used' | 'downloads';

export type ModelStatus = 'running' | 'downloaded' | 'available' | 'downloading';

export interface FlatModelEntry {
  model: ModelInfo;
  status: ModelStatus;
  downloadPct?: number;
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
  searchInputRef?: React.RefObject<HTMLInputElement | null>;
  onAddCustomModel?: () => void;
  onAddOmniCollection?: () => void;
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
  searchInputRef,
  onAddCustomModel,
  onAddOmniCollection,
}) => {
  const [filterOpen, setFilterOpen] = useState(false);
  const [sortBy, setSortBy] = useState<SortBy>('name');
  const filterBtnRef = useRef<HTMLButtonElement>(null);
  const filterPopoverRef = useRef<HTMLDivElement>(null);
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

      // Filter by type
      if (!modelMatchesFilter(m, filterTab)) continue;

      // Filter by search
      if (q) {
        const haystack = `${mName} ${m.display_name || ''} ${(m as any).recipe || ''} ${(m.labels || []).join(' ')}`.toLowerCase();
        if (!haystack.includes(q)) continue;
      }

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

      result.push({ model: m, status, downloadPct: pullPct });
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
  }, [allModels, loadedNames, pulling, downloadItems, searchQuery, filterTab, sortBy]);

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
  }, [onSelectModel]);

  // Close filter popover on outside click
  const handleFilterBtnClick = () => setFilterOpen(v => !v);

  return (
    <div className="model-list-panel">
      {/* Title */}
      <div className="model-list-panel__title manager__title">
        <h1>Models</h1>
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
            className={`model-list-panel__filter-btn${filterOpen ? ' model-list-panel__filter-btn--open' : ''}${filterTab !== 'all' ? ' model-list-panel__filter-btn--active' : ''}`}
            onClick={handleFilterBtnClick}
            aria-label="Filter models"
            aria-expanded={filterOpen}
            aria-haspopup="dialog"
          >
            <Icon name="funnel" size={14} aria-hidden="true" />
          </button>

          {filterOpen && (
            <div
              ref={filterPopoverRef}
              className="model-list-panel__filter-popover"
              role="dialog"
              aria-label="Model filters"
              aria-modal="false"
            >
              <div className="model-list-panel__filter-popover-head">
                <span>Filter by type</span>
                <button
                  type="button"
                  className="model-list-panel__filter-popover-close"
                  onClick={() => setFilterOpen(false)}
                  aria-label="Close filter panel"
                >
                  <Icon name="x" size={13} />
                </button>
              </div>
              <div className="model-list-panel__filter-options" role="group" aria-label="Model type filter">
                {FILTER_TABS.map(tab => (
                  <button
                    key={tab.key}
                    type="button"
                    className={`model-list-panel__filter-option${filterTab === tab.key ? ' model-list-panel__filter-option--active' : ''}`}
                    onClick={() => { onFilterChange(tab.key); setFilterOpen(false); }}
                    aria-pressed={filterTab === tab.key}
                  >
                    <CapabilityIcon capability={tab.key === 'llm' ? 'chat' : tab.key === 'embedding' ? 'embedding' : tab.key as any} size={12} />
                    {tab.label}
                  </button>
                ))}
              </div>
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
        {flatList.map(({ model, status, downloadPct }) => {
          const mId = listModelName(model);
          const displayName = listModelDisplayName(model);
          const recipe = String((model as any).recipe || '');
          const isSelected = mId === selectedModelId;
          const cap = capabilityFromModelInfo(model);

          return (
            <li
              key={mId}
              role="option"
              tabIndex={isSelected ? 0 : -1}
              aria-selected={isSelected}
              data-model-id={mId}
              className={`model-list-item${isSelected ? ' model-list-item--selected' : ''} model-list-item--${status}`}
              onClick={() => onSelectModel(mId)}
              onKeyDown={e => handleItemKeyDown(e, mId)}
              aria-label={`${displayName}${status === 'running' ? ', running' : status === 'downloaded' ? ', downloaded' : status === 'downloading' ? ', downloading' : ', available'}${recipe ? `, ${listRecipeBadgeText(recipe)}` : ''}`}
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
                  <span className="model-list-item__cap">
                    <CapabilityIcon capability={cap} size={10} aria-hidden="true" />
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
            </li>
          );
        })}

        {flatList.length === 0 && (
          <li className="model-list-panel__empty manager__empty" aria-live="polite">
            <Icon name="search" size={18} aria-hidden="true" />
            <span>{searchQuery ? 'No models match your search.' : 'No models found.'}</span>
          </li>
        )}
      </ul>

      {/* Custom model / Omni collection actions */}
      {(onAddCustomModel || onAddOmniCollection) && (
        <div className="model-list-panel__footer" aria-label="Add custom models">
          {onAddCustomModel && (
            <button
              type="button"
              className="btn btn--ghost btn--tiny manager__custom-btn"
              onClick={onAddCustomModel}
            >
              + Custom model
            </button>
          )}
          {onAddOmniCollection && (
            <button
              type="button"
              className="btn btn--ghost btn--tiny manager__custom-btn manager__custom-btn--omni"
              onClick={onAddOmniCollection}
            >
              + Omni collection
            </button>
          )}
        </div>
      )}
    </div>
  );
};

export type { FilterTab };
export default ModelListPanel;
