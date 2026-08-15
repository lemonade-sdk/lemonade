/**
 * ModelNavRail — the LEFT navigation rail of the three-pane model view.
 *
 * Pane layout (GUI3, #2355 follow-up requested by fl0rianr):
 *   ┌──────────────┬──────────────────┬─────────────────────────┐
 *   │ ModelNavRail │  ModelListPanel  │     ModelDetailPanel     │
 *   │   (left)     │     (middle)     │         (right)          │
 *   └──────────────┴──────────────────┴─────────────────────────┘
 *
 * The rail surfaces filter dimensions that drive the middle list:
 *   1. Primary nav: All Models / Downloaded / My Models / Favorites.
 *   2. TASK (collapsible multi-select): Chat / Omni / Router / media tasks.
 *   3. BACKENDS (collapsible multi-select): filter by concrete runtime recipe.
 *   4. TAGS (collapsible multi-select): built-in and user-defined chips.
 *   5. Storage meter (role="progressbar").
 *
 * All counts/tasks/tags/backends are derived CLIENT-SIDE from the model
 * list the prototype already loads — no lemond calls. Storage uses derived
 * download sizes where available, falling back to MOCK placeholder values.
 */
import React, { useMemo, useState } from 'react';
import { storageKey } from '../storage';
import type { ModelInfo, ModelRegistryProvider, StorageInfo } from '../api';
import { Icon } from './Icon';
import type { IconName } from './Icon';
import WorkspaceRailHeader from './WorkspaceRailHeader';
import { useI18n } from '../i18n';
import {
  listModelName,
  listRecipeBadgeText,
  modelHasFilterableBackend,
  modelMatchesFilter,
  modelMatchesPrimary,
  modelMatchesTag,
  TAG_CHIPS,
  type FilterTab,
  type PrimaryFilter,
} from './ModelListPanel';

/* ── Static config ───────────────────────────────────────────── */

const PRIMARY_ITEMS: Array<{ key: PrimaryFilter; labelKey: string; iconName: IconName }> = [
  { key: 'all', labelKey: 'nav.primary.all', iconName: 'library' },
  { key: 'downloaded', labelKey: 'nav.primary.downloaded', iconName: 'download' },
  { key: 'my-models', labelKey: 'nav.primary.myModels', iconName: 'box' },
  // Favorites is a DISTINCT concept from Pinned (#2424): star icon, own store.
  { key: 'favorites', labelKey: 'nav.primary.favorites', iconName: 'star' },
];

const TASK_ITEMS: Array<{ key: FilterTab; labelKey: string; iconName: IconName; color: string }> = [
  { key: 'all', labelKey: 'nav.tasks.all', iconName: 'globe', color: 'var(--text-tertiary)' },
  { key: 'llm', labelKey: 'nav.tasks.llm', iconName: 'chat', color: 'var(--cap-chat)' },
  { key: 'omni', labelKey: 'nav.tasks.omni', iconName: 'omni', color: 'var(--cap-omni)' },
  { key: 'router', labelKey: 'nav.tasks.router', iconName: 'router', color: 'var(--cap-router)' },
  { key: 'image', labelKey: 'nav.tasks.image', iconName: 'image', color: 'var(--cap-image)' },
  { key: 'audio', labelKey: 'nav.tasks.audio', iconName: 'audio', color: 'var(--cap-audio)' },
  { key: 'audio-generation', labelKey: 'nav.tasks.audioGeneration', iconName: 'audio', color: 'var(--cap-audio-generation)' },
  { key: 'tts', labelKey: 'nav.tasks.tts', iconName: 'tts', color: 'var(--cap-tts)' },
  { key: 'model3d', labelKey: 'nav.tasks.model3d', iconName: 'box', color: 'var(--cap-model3d)' },
  { key: 'embedding', labelKey: 'nav.tasks.embedding', iconName: 'embedding', color: 'var(--cap-embedding)' },
  { key: 'classification', labelKey: 'nav.tasks.classification', iconName: 'search-check', color: 'var(--cap-classification)' },
];

const MODEL_PROVIDERS: Array<{ key: ModelRegistryProvider; label: string }> = [
  { key: 'huggingface', label: 'Hugging Face' },
  { key: 'modelscope', label: 'ModelScope' },
];


const BUILT_IN_TAG_LABEL_KEYS: Record<string, string> = {
  Recommended: 'nav.builtInTags.recommended',
  Hot: 'nav.builtInTags.hot',
  Small: 'nav.builtInTags.small',
};

const CUSTOM_TAGS_STORAGE_KEY = 'model_filter_custom_tags_v1';

function loadCustomFilterTags(): string[] {
  if (typeof localStorage === 'undefined') return [];
  try {
    const raw = JSON.parse(localStorage.getItem(storageKey(CUSTOM_TAGS_STORAGE_KEY)) || '[]');
    if (!Array.isArray(raw)) return [];
    return Array.from(new Set(raw.map((value: unknown) => String(value).trim()).filter(Boolean))) as string[];
  } catch {
    return [];
  }
}

function saveCustomFilterTags(tags: string[]): void {
  if (typeof localStorage === 'undefined') return;
  try {
    localStorage.setItem(storageKey(CUSTOM_TAGS_STORAGE_KEY), JSON.stringify(tags));
  } catch {
    // Filtering must remain usable when storage is unavailable or full.
  }
}

/* ── Storage (POC) ───────────────────────────────────────────────
   Preferred source is real disk stats via `storageInfo` (api.getStorageInfo()).
   When lemond exposes no disk-usage endpoint (current POC reality), the meter
   falls back to a derived estimate: used = sum of downloaded model sizes, total
   = a headroom-rounded capacity above that — never a hardcoded literal. */
const BYTES_PER_GB = 1024 * 1024 * 1024;

/** Round up to a "nice" capacity (GB) that leaves headroom above `usedGb`. */
function deriveFallbackTotalGb(usedGb: number): number {
  const steps = [16, 32, 64, 128, 256, 512, 1024, 2048, 4096];
  const target = Math.max(usedGb * 2, usedGb + 8, 16);
  for (const step of steps) {
    if (step >= target) return step;
  }
  return Math.ceil(target / 1024) * 1024;
}

/* ── Props ───────────────────────────────────────────────────── */

export interface ModelNavRailProps {
  allModels: ModelInfo[];
  loadedNames: Set<string>;
  pinnedNames: Set<string>;
  /** Favorited model names (distinct from pinned) — drives the Favorites count. */
  favoriteNames: Set<string>;
  primaryFilter: PrimaryFilter;
  onPrimaryFilterChange: (f: PrimaryFilter) => void;
  taskFilters: ReadonlySet<FilterTab>;
  onTaskFiltersChange: (next: Set<FilterTab>) => void;
  backendFilters: ReadonlySet<string>;
  onBackendFiltersChange: (next: Set<string>) => void;
  tagFilters: ReadonlySet<string>;
  onTagFiltersChange: (next: Set<string>) => void;
  providerEnabled: Record<ModelRegistryProvider, boolean>;
  providerCounts: Record<ModelRegistryProvider, number>;
  searchActive: boolean;
  onToggleProvider: (provider: ModelRegistryProvider) => void;
  /** Real disk usage of the model-storage drive, when available (else null →
      derived fallback). Sourced from api.getStorageInfo(). */
  storageInfo?: StorageInfo | null;
  /** id used by the responsive nav toggle's aria-controls. */
  id?: string;
  collapsed: boolean;
  onToggleCollapsed: () => void;
  mobileOpen: boolean;
  onMobileClose: () => void;
  railRef: React.Ref<HTMLElement>;
}

/* ── Component ───────────────────────────────────────────────── */

export const ModelNavRail: React.FC<ModelNavRailProps> = ({
  allModels,
  loadedNames,
  pinnedNames,
  favoriteNames,
  primaryFilter,
  onPrimaryFilterChange,
  taskFilters,
  onTaskFiltersChange,
  backendFilters,
  onBackendFiltersChange,
  tagFilters,
  onTagFiltersChange,
  providerEnabled,
  providerCounts,
  searchActive,
  onToggleProvider,
  storageInfo,
  id = 'model-nav-rail',
  collapsed,
  onToggleCollapsed,
  mobileOpen,
  onMobileClose,
  railRef,
}) => {
  const { t } = useI18n('models');
  const [tasksOpen, setTasksOpen] = useState(true);
  const [backendsOpen, setBackendsOpen] = useState(true);
  const [catalogsOpen, setCatalogsOpen] = useState(true);
  const [tagsOpen, setTagsOpen] = useState(true);
  const [customTags, setCustomTags] = useState<string[]>(loadCustomFilterTags);
  const [customTagDraft, setCustomTagDraft] = useState('');

  // ── Client-side derived counts ──────────────────────────────
  const primaryCounts = useMemo<Record<PrimaryFilter, number>>(() => {
    const counts: Record<PrimaryFilter, number> = { all: 0, downloaded: 0, 'my-models': 0, favorites: 0 };
    for (const m of allModels) {
      if (!listModelName(m)) continue;
      counts.all += 1;
      if (modelMatchesPrimary(m, 'downloaded', loadedNames, favoriteNames)) counts.downloaded += 1;
      if (modelMatchesPrimary(m, 'my-models', loadedNames, favoriteNames)) counts['my-models'] += 1;
      if (modelMatchesPrimary(m, 'favorites', loadedNames, favoriteNames)) counts.favorites += 1;
    }
    return counts;
  }, [allModels, loadedNames, favoriteNames]);

  const taskCounts = useMemo<Record<string, number>>(() => {
    const counts: Record<string, number> = {};
    for (const item of TASK_ITEMS) counts[item.key] = 0;
    for (const m of allModels) {
      if (!listModelName(m)) continue;
      for (const item of TASK_ITEMS) {
        if (modelMatchesFilter(m, item.key)) counts[item.key] += 1;
      }
    }
    return counts;
  }, [allModels]);

  // Distinct backends (recipes) with counts, derived from the model list.
  const backends = useMemo<Array<{ value: string; label: string; count: number }>>(() => {
    const counts = new Map<string, number>();
    for (const m of allModels) {
      const recipe = String((m as any).recipe || '').toLowerCase();
      if (!recipe || !modelHasFilterableBackend(m)) continue;
      counts.set(recipe, (counts.get(recipe) ?? 0) + 1);
    }
    return Array.from(counts.entries())
      .map(([value, count]) => ({ value, label: listRecipeBadgeText(value), count }))
      .sort((a, b) => b.count - a.count || a.label.localeCompare(b.label));
  }, [allModels]);

  const allTagChips = useMemo(
    () => [...TAG_CHIPS, ...customTags.filter(tag => !TAG_CHIPS.some(builtIn => builtIn.toLowerCase() === tag.toLowerCase()))],
    [customTags],
  );

  // Counts stay honest for both built-in and user-defined filters.
  const tagCounts = useMemo<Record<string, number>>(() => {
    const counts: Record<string, number> = {};
    for (const tag of allTagChips) counts[tag] = 0;
    for (const m of allModels) {
      if (!listModelName(m)) continue;
      for (const tag of allTagChips) {
        if (modelMatchesTag(m, tag)) counts[tag] += 1;
      }
    }
    return counts;
  }, [allModels, allTagChips]);

  const toggleTask = (task: FilterTab) => {
    if (task === 'all') {
      onTaskFiltersChange(new Set());
      return;
    }
    const next = new Set(taskFilters);
    if (next.has(task)) next.delete(task); else next.add(task);
    next.delete('all');
    onTaskFiltersChange(next);
  };

  const toggleBackend = (backend: string) => {
    const next = new Set(backendFilters);
    if (next.has(backend)) next.delete(backend); else next.add(backend);
    onBackendFiltersChange(next);
  };

  const toggleTag = (tag: string) => {
    const next = new Set(tagFilters);
    if (next.has(tag)) next.delete(tag); else next.add(tag);
    onTagFiltersChange(next);
  };

  const addCustomTag = () => {
    const tag = customTagDraft.trim();
    if (!tag) return;
    const existing = allTagChips.find(value => value.toLowerCase() === tag.toLowerCase());
    const canonical = existing || tag;
    if (!existing) {
      const nextCustomTags = [...customTags, tag];
      setCustomTags(nextCustomTags);
      saveCustomFilterTags(nextCustomTags);
    }
    const nextFilters = new Set(tagFilters);
    nextFilters.add(canonical);
    onTagFiltersChange(nextFilters);
    setCustomTagDraft('');
  };

  const removeCustomTag = (tag: string) => {
    const nextCustomTags = customTags.filter(value => value.toLowerCase() !== tag.toLowerCase());
    setCustomTags(nextCustomTags);
    saveCustomFilterTags(nextCustomTags);
    const nextFilters = new Set(tagFilters);
    for (const selected of nextFilters) {
      if (selected.toLowerCase() === tag.toLowerCase()) nextFilters.delete(selected);
    }
    onTagFiltersChange(nextFilters);
  };

  // ── Storage meter ───────────────────────────────────────────
  // Prefer REAL disk stats (storageInfo). When unavailable in the POC, derive
  // a graceful estimate from downloaded model sizes — no hardcoded capacity.
  const storage = useMemo(() => {
    if (storageInfo && storageInfo.totalBytes > 0) {
      const usedGb = Math.max(0, Math.round(storageInfo.usedBytes / BYTES_PER_GB));
      const totalGb = Math.max(1, Math.round(storageInfo.totalBytes / BYTES_PER_GB));
      const pct = Math.min(100, Math.round((storageInfo.usedBytes / storageInfo.totalBytes) * 100));
      return { used: usedGb, total: totalGb, pct, real: true };
    }
    let usedGb = 0;
    for (const m of allModels) {
      const downloaded = modelMatchesPrimary(m, 'downloaded', loadedNames, favoriteNames);
      const size = Number((m as any).size);
      if (downloaded && Number.isFinite(size) && size > 0) usedGb += size;
    }
    const used = Math.max(1, Math.round(usedGb));
    const total = deriveFallbackTotalGb(used);
    const pct = Math.min(100, Math.round((used / total) * 100));
    return { used, total, pct, real: false };
  }, [storageInfo, allModels, loadedNames, favoriteNames]);

  return (
    <nav
      ref={railRef}
      className={`model-nav-rail workspace-rail mobile-context-panel${collapsed && !mobileOpen ? ' is-collapsed' : ''}${mobileOpen ? ' is-mobile-open' : ''}`}
      id={id}
      aria-label={t('nav.filters')}
      role={mobileOpen ? 'dialog' : undefined}
      aria-modal={mobileOpen ? true : undefined}
    >
      <WorkspaceRailHeader
        title={t('nav.title')}
        sidebarLabel={t('nav.sidebarLabel')}
        purpose="filter"
        collapsed={collapsed && !mobileOpen}
        onToggle={onToggleCollapsed}
        onMobileClose={mobileOpen ? onMobileClose : undefined}
      />
      <div className="model-nav-rail__scroll">
      {/* 1. Primary nav */}
      <ul className="model-nav-rail__primary workspace-filter-list" role="list">
        {PRIMARY_ITEMS.map(item => {
          const active = primaryFilter === item.key;
          return (
            <li key={item.key}>
              <button
                type="button"
                className={`workspace-filter-list__item model-nav-rail__nav-item${active ? ' is-active model-nav-rail__nav-item--active' : ''}`}
                aria-current={active ? 'true' : undefined}
                onClick={() => onPrimaryFilterChange(item.key)}
              >
                <Icon name={item.iconName} size={14} aria-hidden="true" className="workspace-filter-list__icon model-nav-rail__nav-icon" />
                <span className="workspace-filter-list__label model-nav-rail__nav-label">{t(item.labelKey)}</span>
                <span className="workspace-filter-list__count model-nav-rail__nav-count" aria-hidden="true">{primaryCounts[item.key]}</span>
                <span className="sr-only">{t('nav.countModels', { count: primaryCounts[item.key] })}</span>
              </button>
            </li>
          );
        })}

      </ul>


      <section className="model-nav-rail__section model-nav-rail__section--tasks">
        <h2 className="model-nav-rail__section-head">
          <button
            type="button"
            className="model-nav-rail__section-toggle"
            aria-expanded={tasksOpen}
            aria-controls="nav-tasks"
            onClick={() => setTasksOpen(value => !value)}
          >
            <Icon name={tasksOpen ? 'chevron-down' : 'chevron-right'} size={13} aria-hidden="true" />
            <span>{t(mobileOpen ? 'nav.categories' : 'nav.task')}</span>
          </button>
        </h2>
        {tasksOpen && (
          <div className="model-nav-rail__chip-list model-nav-rail__task-list" id="nav-tasks" role="group" aria-label={t('nav.tasks.filter')}>
            {TASK_ITEMS.map(item => {
              const active = item.key === 'all' ? taskFilters.size === 0 : taskFilters.has(item.key);
              return (
                <button
                  key={item.key}
                  type="button"
                  className={`model-nav-rail__filter-chip model-nav-rail__task-chip${active ? ' is-active' : ''}`}
                  style={{ '--filter-chip-color': item.color } as React.CSSProperties}
                  aria-pressed={active}
                  onClick={() => toggleTask(item.key)}
                >
                  <Icon name={item.iconName} size={13} aria-hidden="true" className="model-nav-rail__task-icon" />
                  <span>{t(item.labelKey)}</span>
                  <span className="model-nav-rail__chip-count" aria-hidden="true">{taskCounts[item.key]}</span>
                  <span className="sr-only">{t('nav.countModels', { count: taskCounts[item.key] })}</span>
                </button>
              );
            })}
          </div>
        )}
      </section>

      <section className="model-nav-rail__section model-nav-rail__section--backends">
        <h2 className="model-nav-rail__section-head">
          <button
            type="button"
            className="model-nav-rail__section-toggle"
            aria-expanded={backendsOpen}
            aria-controls="nav-backends"
            onClick={() => setBackendsOpen(value => !value)}
          >
            <Icon name={backendsOpen ? 'chevron-down' : 'chevron-right'} size={13} aria-hidden="true" />
            <span>{t('nav.backends')}</span>
          </button>
        </h2>
        {backendsOpen && (
          <div className="model-nav-rail__chip-list model-nav-rail__backend-list" id="nav-backends" role="group" aria-label={t('nav.filterBackend')}>
            {backends.map(backend => {
              const active = backendFilters.has(backend.value);
              return (
                <button
                  key={backend.value}
                  type="button"
                  className={`model-nav-rail__filter-chip model-nav-rail__backend-chip${active ? ' is-active' : ''}`}
                  aria-pressed={active}
                  onClick={() => toggleBackend(backend.value)}
                >
                  <span>{backend.label}</span>
                  <span className="model-nav-rail__chip-count" aria-hidden="true">{backend.count}</span>
                  <span className="sr-only">{t('nav.countModels', { count: backend.count })}</span>
                </button>
              );
            })}
          </div>
        )}
      </section>

      <section className="model-nav-rail__section model-nav-rail__section--providers">
        <h2 className="model-nav-rail__section-head">
          <button
            type="button"
            className="model-nav-rail__section-toggle"
            aria-expanded={catalogsOpen}
            aria-controls="nav-online-catalogs"
            onClick={() => setCatalogsOpen(value => !value)}
          >
            <Icon name={catalogsOpen ? 'chevron-down' : 'chevron-right'} size={13} aria-hidden="true" />
            <span>{t('nav.catalogs')}</span>
          </button>
        </h2>
        {catalogsOpen && (
          <ul className="model-nav-rail__provider-list" id="nav-online-catalogs" role="list">
            {MODEL_PROVIDERS.map(provider => {
              const enabled = providerEnabled[provider.key];
              const count = providerCounts[provider.key];
              const showCount = searchActive && primaryFilter === 'all' && enabled;
              const title = t(enabled ? 'nav.providerSearchOn' : 'nav.providerSearchOff', { provider: provider.label });
              return (
                <li key={provider.key}>
                  <label className="backends__toggle model-nav-rail__provider-option" title={title}>
                    <input
                      type="checkbox"
                      checked={enabled}
                      onChange={() => onToggleProvider(provider.key)}
                    />
                    <span className="model-nav-rail__provider-label">{provider.label}</span>
                    {showCount && (
                      <>
                        <span className="model-nav-rail__nav-count" aria-hidden="true">{count}</span>
                        <span className="sr-only">{t('nav.searchResults', { count })}</span>
                      </>
                    )}
                  </label>
                </li>
              );
            })}
          </ul>
        )}
      </section>

      <section className="model-nav-rail__section">
        <h2 className="model-nav-rail__section-head">
          <button
            type="button"
            className="model-nav-rail__section-toggle"
            aria-expanded={tagsOpen}
            aria-controls="nav-tags"
            onClick={() => setTagsOpen(value => !value)}
          >
            <Icon name={tagsOpen ? 'chevron-down' : 'chevron-right'} size={13} aria-hidden="true" />
            <span>{t('nav.tags')}</span>
          </button>
        </h2>
        {tagsOpen && (
          <div id="nav-tags">
            <div className="model-nav-rail__chip-list model-nav-rail__tag-list" role="group" aria-label={t('nav.filterTag')}>
              {allTagChips.map(tag => {
                const active = tagFilters.has(tag);
                const isCustom = customTags.some(value => value.toLowerCase() === tag.toLowerCase());
                const labelKey = BUILT_IN_TAG_LABEL_KEYS[tag];
                const displayTag = labelKey ? t(labelKey) : tag;
                return (
                  <span key={tag} className={`model-nav-rail__tag-wrap${isCustom ? ' is-custom' : ''}`}>
                    <button
                      type="button"
                      className={`model-nav-rail__filter-chip model-nav-rail__tag-chip${active ? ' is-active' : ''}`}
                      aria-pressed={active}
                      onClick={() => toggleTag(tag)}
                    >
                      <span>{displayTag}</span>
                      <span className="model-nav-rail__chip-count" aria-hidden="true">{tagCounts[tag] || 0}</span>
                      <span className="sr-only">{t('nav.countModels', { count: tagCounts[tag] || 0 })}</span>
                    </button>
                    {isCustom && (
                      <button
                        type="button"
                        className="model-nav-rail__custom-tag-remove"
                        onClick={() => removeCustomTag(tag)}
                        aria-label={t('nav.removeCustomTag', { tag })}
                        title={t('nav.removeTag', { tag })}
                      >
                        <Icon name="x" size={10} aria-hidden="true" />
                      </button>
                    )}
                  </span>
                );
              })}
            </div>
            <div className="model-nav-rail__custom-tag-entry">
              <label className="sr-only" htmlFor="nav-custom-tag">{t('nav.addTag')}</label>
              <input
                id="nav-custom-tag"
                type="text"
                value={customTagDraft}
                onChange={event => setCustomTagDraft(event.target.value)}
                onKeyDown={event => {
                  if (event.key === 'Enter') {
                    event.preventDefault();
                    addCustomTag();
                  }
                }}
                placeholder={t('nav.customTag')}
                autoComplete="off"
              />
              <button type="button" onClick={addCustomTag} disabled={!customTagDraft.trim()} aria-label={t('nav.addTag')}>
                <Icon name="plus" size={12} aria-hidden="true" />
              </button>
            </div>
          </div>
        )}
      </section>

      {/* 5. Storage meter */}
      <div className="model-nav-rail__storage">
        <div className="model-nav-rail__storage-row">
          <span className="model-nav-rail__storage-label">{t(storage.real ? 'nav.storage' : 'nav.storageEstimated')}</span>
          <span className="model-nav-rail__storage-value">{t('nav.storageValue', { used: storage.used, total: storage.total })}</span>
        </div>
        <div
          className="model-nav-rail__storage-bar"
          role="progressbar"
          aria-label={t(storage.real ? 'nav.storageUsed' : 'nav.storageUsedEstimated')}
          aria-valuenow={storage.used}
          aria-valuemin={0}
          aria-valuemax={storage.total}
          aria-valuetext={t('nav.storageValueText', { used: storage.used, total: storage.total, estimated: storage.real ? '' : t('nav.estimatedSuffix') })}
        >
          <span className="model-nav-rail__storage-fill" style={{ width: `${storage.pct}%` }} aria-hidden="true" />
        </div>
      </div>
      </div>
    </nav>
  );
};

export default ModelNavRail;
