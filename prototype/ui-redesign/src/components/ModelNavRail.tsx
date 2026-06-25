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
 *   2. CATEGORIES (collapsible): All / LLM / Omni / Image / Audio / TTS / Embed.
 *   3. Backends (collapsible select): filter by recipe.
 *   4. TAGS (collapsible): model-family + size chips.
 *   5. Storage meter (role="progressbar").
 *
 * ALL counts/categories/tags/backends are derived CLIENT-SIDE from the model
 * list the prototype already loads — no lemond calls. Storage uses derived
 * download sizes where available, falling back to MOCK placeholder values.
 */
import React, { useMemo, useState } from 'react';
import type { ModelInfo } from '../api';
import { Icon } from './Icon';
import type { IconName } from './Icon';
import {
  listModelName,
  listRecipeBadgeText,
  modelMatchesFilter,
  modelMatchesPrimary,
  modelMatchesTag,
  TAG_CHIPS,
  type FilterTab,
  type PrimaryFilter,
} from './ModelListPanel';

/* ── Static config ───────────────────────────────────────────── */

const PRIMARY_ITEMS: Array<{ key: PrimaryFilter; label: string; iconName: IconName }> = [
  { key: 'all', label: 'All Models', iconName: 'library' },
  { key: 'downloaded', label: 'Downloaded', iconName: 'download' },
  { key: 'my-models', label: 'My Models', iconName: 'box' },
  { key: 'favorites', label: 'Favorites', iconName: 'pin' },
];

const CATEGORY_ITEMS: Array<{ key: FilterTab; label: string; iconName: IconName }> = [
  { key: 'all', label: 'All', iconName: 'globe' },
  { key: 'llm', label: 'LLM', iconName: 'chat' },
  { key: 'omni', label: 'Omni', iconName: 'omni' },
  { key: 'image', label: 'Image', iconName: 'image' },
  { key: 'audio', label: 'Audio', iconName: 'audio' },
  { key: 'tts', label: 'TTS', iconName: 'tts' },
  { key: 'embedding', label: 'Embed', iconName: 'embedding' },
];

/* ── Storage (POC) ───────────────────────────────────────────────
   No lemond disk-usage endpoint is available to the client, so the
   total capacity is a MOCK placeholder. Used space is derived from the
   sizes of downloaded models when present, otherwise a MOCK value. */
const MOCK_TOTAL_STORAGE_GB = 512;
const MOCK_USED_STORAGE_GB = 128;

/* ── Props ───────────────────────────────────────────────────── */

export interface ModelNavRailProps {
  allModels: ModelInfo[];
  loadedNames: Set<string>;
  pinnedNames: Set<string>;
  primaryFilter: PrimaryFilter;
  onPrimaryFilterChange: (f: PrimaryFilter) => void;
  categoryFilter: FilterTab;
  onCategoryFilterChange: (f: FilterTab) => void;
  backendFilter: string;
  onBackendFilterChange: (b: string) => void;
  tagFilter: string | null;
  onTagFilterChange: (t: string | null) => void;
  /** id used by the responsive nav toggle's aria-controls. */
  id?: string;
}

/* ── Component ───────────────────────────────────────────────── */

export const ModelNavRail: React.FC<ModelNavRailProps> = ({
  allModels,
  loadedNames,
  pinnedNames,
  primaryFilter,
  onPrimaryFilterChange,
  categoryFilter,
  onCategoryFilterChange,
  backendFilter,
  onBackendFilterChange,
  tagFilter,
  onTagFilterChange,
  id = 'model-nav-rail',
}) => {
  const [categoriesOpen, setCategoriesOpen] = useState(true);
  const [tagsOpen, setTagsOpen] = useState(true);

  // ── Client-side derived counts ──────────────────────────────
  const primaryCounts = useMemo<Record<PrimaryFilter, number>>(() => {
    const counts: Record<PrimaryFilter, number> = { all: 0, downloaded: 0, 'my-models': 0, favorites: 0 };
    for (const m of allModels) {
      if (!listModelName(m)) continue;
      counts.all += 1;
      if (modelMatchesPrimary(m, 'downloaded', loadedNames, pinnedNames)) counts.downloaded += 1;
      if (modelMatchesPrimary(m, 'my-models', loadedNames, pinnedNames)) counts['my-models'] += 1;
      if (modelMatchesPrimary(m, 'favorites', loadedNames, pinnedNames)) counts.favorites += 1;
    }
    return counts;
  }, [allModels, loadedNames, pinnedNames]);

  const categoryCounts = useMemo<Record<string, number>>(() => {
    const counts: Record<string, number> = {};
    for (const item of CATEGORY_ITEMS) counts[item.key] = 0;
    for (const m of allModels) {
      if (!listModelName(m)) continue;
      for (const item of CATEGORY_ITEMS) {
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
      if (!recipe) continue;
      counts.set(recipe, (counts.get(recipe) ?? 0) + 1);
    }
    return Array.from(counts.entries())
      .map(([value, count]) => ({ value, label: listRecipeBadgeText(value), count }))
      .sort((a, b) => b.count - a.count || a.label.localeCompare(b.label));
  }, [allModels]);

  // Only show tag chips that match at least one model (keeps the rail honest).
  const tagCounts = useMemo<Record<string, number>>(() => {
    const counts: Record<string, number> = {};
    for (const tag of TAG_CHIPS) counts[tag] = 0;
    for (const m of allModels) {
      if (!listModelName(m)) continue;
      for (const tag of TAG_CHIPS) {
        if (modelMatchesTag(m, tag)) counts[tag] += 1;
      }
    }
    return counts;
  }, [allModels]);

  // ── Storage meter (derived where possible, else MOCK) ───────
  const storage = useMemo(() => {
    let usedGb = 0;
    for (const m of allModels) {
      const downloaded = modelMatchesPrimary(m, 'downloaded', loadedNames, pinnedNames);
      const size = Number((m as any).size);
      if (downloaded && Number.isFinite(size) && size > 0) usedGb += size;
    }
    const used = usedGb > 0 ? Math.round(usedGb) : MOCK_USED_STORAGE_GB;
    const total = MOCK_TOTAL_STORAGE_GB;
    const pct = Math.min(100, Math.round((used / total) * 100));
    return { used, total, pct };
  }, [allModels, loadedNames, pinnedNames]);

  return (
    <nav className="model-nav-rail" id={id} aria-label="Model filters">
      {/* 1. Primary nav */}
      <ul className="model-nav-rail__primary" role="list">
        {PRIMARY_ITEMS.map(item => {
          const active = primaryFilter === item.key;
          return (
            <li key={item.key}>
              <button
                type="button"
                className={`model-nav-rail__nav-item${active ? ' model-nav-rail__nav-item--active' : ''}`}
                aria-current={active ? 'true' : undefined}
                onClick={() => onPrimaryFilterChange(item.key)}
              >
                <Icon name={item.iconName} size={15} aria-hidden="true" className="model-nav-rail__nav-icon" />
                <span className="model-nav-rail__nav-label">{item.label}</span>
                <span className="model-nav-rail__nav-count" aria-hidden="true">{primaryCounts[item.key]}</span>
                <span className="sr-only">{`, ${primaryCounts[item.key]} models`}</span>
              </button>
            </li>
          );
        })}
      </ul>

      {/* 2. Categories (collapsible) */}
      <section className="model-nav-rail__section">
        <h2 className="model-nav-rail__section-head">
          <button
            type="button"
            className="model-nav-rail__section-toggle"
            aria-expanded={categoriesOpen}
            aria-controls="nav-categories"
            onClick={() => setCategoriesOpen(v => !v)}
          >
            <Icon name={categoriesOpen ? 'chevron-down' : 'chevron-right'} size={13} aria-hidden="true" />
            <span>Categories</span>
          </button>
        </h2>
        {categoriesOpen && (
          <ul className="model-nav-rail__cat-list" id="nav-categories" role="list">
            {CATEGORY_ITEMS.map(item => {
              const active = categoryFilter === item.key;
              return (
                <li key={item.key}>
                  <button
                    type="button"
                    className={`model-nav-rail__cat-item${active ? ' model-nav-rail__cat-item--active' : ''}`}
                    aria-current={active ? 'true' : undefined}
                    onClick={() => onCategoryFilterChange(item.key)}
                  >
                    <Icon name={item.iconName} size={14} aria-hidden="true" className="model-nav-rail__cat-icon" />
                    <span className="model-nav-rail__cat-label">{item.label}</span>
                    <span className="model-nav-rail__nav-count" aria-hidden="true">{categoryCounts[item.key]}</span>
                    <span className="sr-only">{`, ${categoryCounts[item.key]} models`}</span>
                  </button>
                </li>
              );
            })}
          </ul>
        )}
      </section>

      {/* 3. Backends select */}
      <div className="model-nav-rail__backends">
        <label htmlFor="nav-backend-select" className="model-nav-rail__backends-label">Backends</label>
        <select
          id="nav-backend-select"
          className="model-nav-rail__backends-select"
          value={backendFilter}
          onChange={e => onBackendFilterChange(e.target.value)}
        >
          <option value="all">All backends</option>
          {backends.map(b => (
            <option key={b.value} value={b.value}>{`${b.label} (${b.count})`}</option>
          ))}
        </select>
      </div>

      {/* 4. Tags (collapsible) */}
      <section className="model-nav-rail__section">
        <h2 className="model-nav-rail__section-head">
          <button
            type="button"
            className="model-nav-rail__section-toggle"
            aria-expanded={tagsOpen}
            aria-controls="nav-tags"
            onClick={() => setTagsOpen(v => !v)}
          >
            <Icon name={tagsOpen ? 'chevron-down' : 'chevron-right'} size={13} aria-hidden="true" />
            <span>Tags</span>
          </button>
        </h2>
        {tagsOpen && (
          <div className="model-nav-rail__tags" id="nav-tags" role="group" aria-label="Filter by tag">
            {TAG_CHIPS.filter(tag => tagCounts[tag] > 0).map(tag => {
              const active = tagFilter?.toLowerCase() === tag.toLowerCase();
              return (
                <button
                  key={tag}
                  type="button"
                  className={`model-nav-rail__tag${active ? ' model-nav-rail__tag--active' : ''}`}
                  aria-pressed={active}
                  onClick={() => onTagFilterChange(active ? null : tag)}
                >
                  {tag}
                </button>
              );
            })}
            <button
              type="button"
              className="model-nav-rail__tag model-nav-rail__tag--recommended"
              aria-label="Show recommended tags"
            >
              + Recommended +
            </button>
          </div>
        )}
      </section>

      {/* 5. Storage meter */}
      <div className="model-nav-rail__storage">
        <div className="model-nav-rail__storage-row">
          <span className="model-nav-rail__storage-label">Storage</span>
          <span className="model-nav-rail__storage-value">{`${storage.used} GB / ${storage.total} GB`}</span>
        </div>
        <div
          className="model-nav-rail__storage-bar"
          role="progressbar"
          aria-label="Model storage used"
          aria-valuenow={storage.used}
          aria-valuemin={0}
          aria-valuemax={storage.total}
          aria-valuetext={`${storage.used} of ${storage.total} gigabytes used`}
        >
          <span className="model-nav-rail__storage-fill" style={{ width: `${storage.pct}%` }} aria-hidden="true" />
        </div>
      </div>
    </nav>
  );
};

export default ModelNavRail;
