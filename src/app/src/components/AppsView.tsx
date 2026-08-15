import React, { useEffect, useMemo, useState } from 'react';
import type { IconName } from './Icon';
import {
  WorkspaceCatalogLayout,
  WorkspaceCatalogSection,
  type CatalogFilterDefinition,
} from './WorkspaceCatalogLayout';
import {
  WorkspaceActionButton,
  WorkspaceActionGroup,
  WorkspacePaneHeader,
} from './WorkspacePanels';
import { useI18n } from '../i18n';
import {
  localizedMarketplaceCategory,
  localizedMarketplaceDescription,
} from '../features/apps/marketplacePresentation';

export type MarketplaceApp = {
  id: string;
  name: string;
  description?: string;
  category?: string[];
  logo?: string;
  pinned?: boolean;
  links?: { app?: string; guide?: string; video?: string };
};

export type MarketplaceCategory = {
  id: string;
  label: string;
};

export const MARKETPLACE_URL = 'https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps.json';
const ALL_APPS_SECTION = 'all-apps';
const FEATURED_APP_LIMIT = 4;

function categorySectionId(categoryKey: string): string {
  const safeCategory = encodeURIComponent(categoryKey)
    .replace(/%/g, '')
    .replace(/[^a-z0-9_-]+/g, '-');
  return `category-${safeCategory || 'other'}`;
}

function categoryIcon(category: string): IconName {
  const normalized = category.toLowerCase();
  if (/chat|assistant|conversation|bot/.test(normalized)) return 'chat';
  if (/image|photo|design|creative|art/.test(normalized)) return 'image';
  if (/audio|music|speech|voice/.test(normalized)) return 'audio';
  if (/code|developer|development|ide/.test(normalized)) return 'code';
  if (/search|research|knowledge|rag/.test(normalized)) return 'search';
  if (/automation|workflow|tool|productivity/.test(normalized)) return 'tools';
  if (/3d|model/.test(normalized)) return 'box';
  return 'layers';
}

function categoryLabel(category: string): string {
  return category.trim().replace(/(^|[\s/&-])(\p{Ll})/gu, (_match, separator: string, letter: string) =>
    `${separator}${letter.toLocaleUpperCase()}`);
}

function normalizedCategory(category: string): string {
  return category.trim().toLocaleLowerCase();
}

type CategoryGroup = {
  key: string;
  label: string;
  apps: MarketplaceApp[];
};

function collectCategoryGroups(
  apps: MarketplaceApp[],
  categoriesOf: (app: MarketplaceApp) => string[],
  labelByCategory: Map<string, string>,
  feedOrder: Map<string, number>,
  otherLabel: string,
): CategoryGroup[] {
  const groups = new Map<string, CategoryGroup>();
  apps.forEach(app => {
    const rawCategories = categoriesOf(app).map(category => category.trim());
    const seenKeys = new Set<string>();
    (rawCategories.length ? rawCategories : ['']).forEach(raw => {
      const key = raw ? normalizedCategory(raw) : 'other';
      if (seenKeys.has(key)) return;
      seenKeys.add(key);
      let group = groups.get(key);
      if (!group) {
        group = { key, label: raw ? labelByCategory.get(key) ?? categoryLabel(raw) : otherLabel, apps: [] };
        groups.set(key, group);
      }
      group.apps.push(app);
    });
  });
  return Array.from(groups.values())
    .map(group => ({ ...group, apps: [...group.apps].sort((a, b) => a.name.localeCompare(b.name)) }))
    .sort((left, right) => {
      const leftOrder = feedOrder.get(left.key);
      const rightOrder = feedOrder.get(right.key);
      if (leftOrder !== undefined && rightOrder !== undefined) return leftOrder - rightOrder;
      if (leftOrder !== undefined) return -1;
      if (rightOrder !== undefined) return 1;
      return left.label.localeCompare(right.label);
    });
}

/* Dark artwork over a transparent field disappears on the dark theme, so
 * those logos get a white tile behind them; opaque or bright logos render
 * bare. Sampled through a canvas, which requires CORS-enabled logo hosts. */
const logoTileCache = new Map<string, boolean>();

function logoNeedsTile(src: string): Promise<boolean> {
  return new Promise(resolve => {
    const probe = new Image();
    probe.crossOrigin = 'anonymous';
    probe.onload = () => {
      try {
        const size = 24;
        const canvas = document.createElement('canvas');
        canvas.width = size;
        canvas.height = size;
        const context = canvas.getContext('2d');
        if (!context) {
          resolve(false);
          return;
        }
        context.drawImage(probe, 0, 0, size, size);
        const { data } = context.getImageData(0, 0, size, size);
        let transparent = 0;
        let opaque = 0;
        let luminanceSum = 0;
        for (let i = 0; i < data.length; i += 4) {
          if (data[i + 3] < 26) {
            transparent += 1;
            continue;
          }
          opaque += 1;
          luminanceSum += (0.299 * data[i] + 0.587 * data[i + 1] + 0.114 * data[i + 2]) / 255;
        }
        const transparentShare = transparent / (transparent + opaque || 1);
        const meanLuminance = opaque ? luminanceSum / opaque : 1;
        resolve(transparentShare > 0.05 && meanLuminance < 0.45);
      } catch {
        resolve(false);
      }
    };
    probe.onerror = () => resolve(false);
    probe.src = src;
  });
}

const AppLogo: React.FC<{ src: string }> = ({ src }) => {
  const [tiled, setTiled] = useState(() => logoTileCache.get(src) ?? false);

  useEffect(() => {
    if (logoTileCache.has(src)) {
      setTiled(logoTileCache.get(src)!);
      return undefined;
    }
    let cancelled = false;
    logoNeedsTile(src).then(needsTile => {
      logoTileCache.set(src, needsTile);
      if (!cancelled) setTiled(needsTile);
    });
    return () => { cancelled = true; };
  }, [src]);

  return <img className={`app-card__logo${tiled ? ' app-card__logo--tile' : ''}`} src={src} alt="" />;
};

type AppsViewProps = {
  apps: MarketplaceApp[];
  categories: MarketplaceCategory[];
  loading: boolean;
  error: string | null;
};

const AppsView: React.FC<AppsViewProps> = ({
  apps: marketplaceApps,
  categories: marketplaceCategories,
  loading: marketplaceLoading,
  error: marketplaceError,
}) => {
  const { t, tOptional } = useI18n('apps');
  const displayMarketplaceError = marketplaceError === 'Unknown error' ? t('unknownError') : marketplaceError;
  const [categoryFilter, setCategoryFilter] = useState<string | null>(null);

  const featuredApps = useMemo(
    () => marketplaceApps.filter(app => app.pinned).slice(0, FEATURED_APP_LIMIT),
    [marketplaceApps],
  );

  const labelByCategory = useMemo(() => new Map(
    marketplaceCategories.map(category => [
      normalizedCategory(category.id),
      localizedMarketplaceCategory(category.id, category.label, tOptional),
    ]),
  ), [marketplaceCategories, tOptional]);

  const displayLabel = (rawCategory: string): string => {
    const fallback = labelByCategory.get(normalizedCategory(rawCategory)) ?? categoryLabel(rawCategory);
    return localizedMarketplaceCategory(rawCategory, fallback, tOptional);
  };

  const feedOrder = useMemo(() => new Map(
    marketplaceCategories.map((category, index) => [normalizedCategory(category.id), index]),
  ), [marketplaceCategories]);

  /* Sections group each app under its primary category; the sidebar filters
   * match every assigned category, so a multi-category app is reachable from
   * all of them. */
  const categoryGroups = useMemo(
    () => collectCategoryGroups(marketplaceApps, app => app.category?.slice(0, 1) ?? [], labelByCategory, feedOrder, t('category.other')),
    [feedOrder, labelByCategory, marketplaceApps, t],
  );

  const categoryOptions = useMemo(
    () => collectCategoryGroups(marketplaceApps, app => app.category ?? [], labelByCategory, feedOrder, t('category.other')),
    [feedOrder, labelByCategory, marketplaceApps, t],
  );

  useEffect(() => {
    if (categoryFilter && !categoryOptions.some(group => group.key === categoryFilter)) setCategoryFilter(null);
  }, [categoryOptions, categoryFilter]);

  const categoryFilters = useMemo<CatalogFilterDefinition<string>[]>(() => [
    {
      id: ALL_APPS_SECTION,
      label: t('filters.all'),
      description: marketplaceLoading
        ? t('filters.loading')
        : marketplaceError
          ? t('filters.unavailable')
          : t('filters.compatible'),
      icon: 'globe',
      count: marketplaceLoading || marketplaceError ? undefined : marketplaceApps.length,
    },
    ...categoryOptions.map(group => ({
      id: categorySectionId(group.key),
      label: group.label,
      description: t('filters.count', { count: group.apps.length }),
      icon: categoryIcon(group.key),
      count: group.apps.length,
    })),
  ], [categoryOptions, marketplaceApps.length, marketplaceError, marketplaceLoading, t]);

  const categoryBySection = useMemo(() => new Map(
    categoryOptions.map(group => [categorySectionId(group.key), group.key]),
  ), [categoryOptions]);

  const activeCategorySection = categoryFilter ? categorySectionId(categoryFilter) : ALL_APPS_SECTION;
  const activeGroup = categoryFilter
    ? categoryOptions.find(group => group.key === categoryFilter) ?? null
    : null;

  const openExternal = (url?: string) => {
    if (!url) return;
    const hostApi = (window as unknown as { api?: { openExternal?: (url: string) => void } }).api;
    if (hostApi?.openExternal) {
      hostApi.openExternal(url);
      return;
    }
    window.open(url, '_blank', 'noopener,noreferrer');
  };

  const paneTitle = t('title');
  const paneSubtitle = t('subtitle');

  const showFeatured = !activeGroup && featuredApps.length > 0;
  const visibleGroups = (activeGroup ? [activeGroup] : categoryGroups)
    .map(group => ({
      ...group,
      apps: showFeatured ? group.apps.filter(app => !featuredApps.includes(app)) : group.apps,
    }))
    .filter(group => group.apps.length > 0);

  const renderAppCard = (app: MarketplaceApp) => (
    <article key={app.id || app.name} className="workspace-card app-card">
      <header className="workspace-card__head">
        {app.logo
          ? <AppLogo src={app.logo} />
          : <span className="app-card__logo app-card__logo--tile app-card__logo--fallback" aria-hidden="true">{app.name.slice(0, 1).toUpperCase()}</span>}
        <span className="app-card__identity">
          <h3 className="workspace-card__name app-card__name">{app.name}</h3>
          {app.category && app.category.length > 0 && (
            <span className="app-card__category">{app.category.map(displayLabel).join(' · ')}</span>
          )}
        </span>
      </header>
      <p className="app-card__description">{localizedMarketplaceDescription(app, tOptional) || t('card.noDescription')}</p>
      <footer className="app-card__footer">
        <WorkspaceActionGroup className="app-card__actions" label={t('card.links', { name: app.name })}>
          {app.links?.app && <WorkspaceActionButton appearance="secondary" size="small" icon="globe" onClick={() => openExternal(app.links?.app)}>{t('card.visit')}</WorkspaceActionButton>}
          {app.links?.guide && <WorkspaceActionButton appearance="quiet" size="small" onClick={() => openExternal(app.links?.guide)}>{t('card.guide')}</WorkspaceActionButton>}
          {app.links?.video && <WorkspaceActionButton appearance="quiet" size="small" onClick={() => openExternal(app.links?.video)}>{t('card.video')}</WorkspaceActionButton>}
        </WorkspaceActionGroup>
      </footer>
    </article>
  );

  return (
    <WorkspaceCatalogLayout
      view="apps"
      className="apps-workspace"
      panelId="apps-types-panel"
      railTitle={t('filters.title')}
      railLabel={t('filters.railLabel')}
      sidebarLabel={t('filters.sidebarLabel')}
      mobileMenuLabel={t('filters.mobileMenu')}
      filters={categoryFilters}
      activeFilter={activeCategorySection}
      onFilterChange={section => setCategoryFilter(categoryBySection.get(section) ?? null)}
      header={
        <WorkspacePaneHeader
          headingLevel={1}
          title={paneTitle}
          subtitle={paneSubtitle}
          titleId="apps-pane-title"
        />
      }
    >
      {marketplaceLoading ? (
        <div className="apps__empty" role="status">{t('loading')}</div>
      ) : marketplaceError ? (
        <div className="apps__error" role="alert">{t('error', { error: displayMarketplaceError })}</div>
      ) : (
        <div className="workspace-catalog" aria-label={t('directory', { title: paneTitle })}>
          {showFeatured && (
            <WorkspaceCatalogSection
              title={t('featured.title')}
              description={t('featured.description')}
            >
              {featuredApps.map(renderAppCard)}
            </WorkspaceCatalogSection>
          )}
          {visibleGroups.map(group => (
            <WorkspaceCatalogSection key={group.key} title={group.label}>
              {group.apps.map(renderAppCard)}
            </WorkspaceCatalogSection>
          ))}
          {marketplaceApps.length === 0 && (
            <div className="apps__empty">
              {t('empty')}
            </div>
          )}
        </div>
      )}
    </WorkspaceCatalogLayout>
  );
};

export default AppsView;
