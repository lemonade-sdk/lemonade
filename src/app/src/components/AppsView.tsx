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

export type MarketplaceApp = {
  id: string;
  name: string;
  description?: string;
  category?: string[];
  logo?: string;
  pinned?: boolean;
  links?: { app?: string; guide?: string; video?: string };
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

function appCountLabel(count: number): string {
  return `${count} compatible ${count === 1 ? 'app' : 'apps'}`;
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

type AppsViewProps = {
  apps: MarketplaceApp[];
  loading: boolean;
  error: string | null;
};

const AppsView: React.FC<AppsViewProps> = ({
  apps: marketplaceApps,
  loading: marketplaceLoading,
  error: marketplaceError,
}) => {
  const [categoryFilter, setCategoryFilter] = useState<string | null>(null);

  const featuredApps = useMemo(
    () => marketplaceApps.filter(app => app.pinned).slice(0, FEATURED_APP_LIMIT),
    [marketplaceApps],
  );

  const categoryGroups = useMemo<CategoryGroup[]>(() => {
    const groups = new Map<string, CategoryGroup>();
    marketplaceApps.forEach(app => {
      const primary = app.category?.[0]?.trim();
      const key = primary ? normalizedCategory(primary) : 'other';
      let group = groups.get(key);
      if (!group) {
        group = { key, label: primary ? categoryLabel(primary) : 'Other', apps: [] };
        groups.set(key, group);
      }
      group.apps.push(app);
    });
    return Array.from(groups.values())
      .map(group => ({ ...group, apps: [...group.apps].sort((a, b) => a.name.localeCompare(b.name)) }))
      .sort((left, right) => left.label.localeCompare(right.label));
  }, [marketplaceApps]);

  useEffect(() => {
    if (categoryFilter && !categoryGroups.some(group => group.key === categoryFilter)) setCategoryFilter(null);
  }, [categoryGroups, categoryFilter]);

  const categoryFilters = useMemo<CatalogFilterDefinition<string>[]>(() => [
    {
      id: ALL_APPS_SECTION,
      label: 'All apps',
      description: marketplaceLoading
        ? 'Loading directory'
        : marketplaceError
          ? 'Directory unavailable'
          : 'Compatible clients and tools',
      icon: 'globe',
      count: marketplaceLoading || marketplaceError ? undefined : marketplaceApps.length,
    },
    ...categoryGroups.map(group => ({
      id: categorySectionId(group.key),
      label: group.label,
      description: appCountLabel(group.apps.length),
      icon: categoryIcon(group.key),
      count: group.apps.length,
    })),
  ], [categoryGroups, marketplaceApps.length, marketplaceError, marketplaceLoading]);

  const categoryBySection = useMemo(() => new Map(
    categoryGroups.map(group => [categorySectionId(group.key), group.key]),
  ), [categoryGroups]);

  const activeCategorySection = categoryFilter ? categorySectionId(categoryFilter) : ALL_APPS_SECTION;
  const activeGroup = categoryFilter
    ? categoryGroups.find(group => group.key === categoryFilter) ?? null
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

  const paneTitle = activeGroup ? activeGroup.label : 'Apps Marketplace';
  const paneSubtitle = activeGroup
    ? `${appCountLabel(activeGroup.apps.length)} in ${activeGroup.label}.`
    : 'Lemonade works best as the inference server for applications. Try out this curated list of apps!';

  const renderAppCard = (app: MarketplaceApp) => (
    <article key={app.id || app.name} className="workspace-card app-card">
      <header className="workspace-card__head">
        {app.logo
          ? <img className="app-card__logo" src={app.logo} alt="" />
          : <span className="app-card__logo app-card__logo--fallback" aria-hidden="true">{app.name.slice(0, 1).toUpperCase()}</span>}
        <span className="app-card__identity">
          <h3 className="workspace-card__name app-card__name">{app.name}</h3>
          {app.category && app.category.length > 0 && (
            <span className="app-card__category">{app.category.map(categoryLabel).join(' · ')}</span>
          )}
        </span>
      </header>
      <p className="app-card__description">{app.description || 'No description available.'}</p>
      <footer className="app-card__footer">
        <WorkspaceActionGroup className="app-card__actions" label={`Links for ${app.name}`}>
          {app.links?.app && <WorkspaceActionButton appearance="secondary" size="small" icon="globe" onClick={() => openExternal(app.links?.app)}>Visit</WorkspaceActionButton>}
          {app.links?.guide && <WorkspaceActionButton appearance="quiet" size="small" onClick={() => openExternal(app.links?.guide)}>Guide</WorkspaceActionButton>}
          {app.links?.video && <WorkspaceActionButton appearance="quiet" size="small" onClick={() => openExternal(app.links?.video)}>Video</WorkspaceActionButton>}
        </WorkspaceActionGroup>
      </footer>
    </article>
  );

  return (
    <WorkspaceCatalogLayout
      view="apps"
      className="apps-workspace"
      panelId="apps-types-panel"
      railTitle="Filters"
      railLabel="App categories"
      sidebarLabel="app categories"
      mobileMenuLabel="Open app categories"
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
        <div className="apps__empty" role="status">Loading apps...</div>
      ) : marketplaceError ? (
        <div className="apps__error" role="alert">Apps unavailable: {marketplaceError}</div>
      ) : (
        <div className="workspace-catalog" aria-label={`${paneTitle} directory`}>
          {activeGroup ? (
            <WorkspaceCatalogSection>
              {activeGroup.apps.map(renderAppCard)}
            </WorkspaceCatalogSection>
          ) : (
            <>
              {featuredApps.length > 0 && (
                <WorkspaceCatalogSection
                  title="Featured"
                  description="Picks from the Lemonade team."
                >
                  {featuredApps.map(renderAppCard)}
                </WorkspaceCatalogSection>
              )}
              {categoryGroups.map(group => (
                <WorkspaceCatalogSection key={group.key} title={group.label}>
                  {group.apps.map(renderAppCard)}
                </WorkspaceCatalogSection>
              ))}
            </>
          )}
          {marketplaceApps.length === 0 && (
            <div className="apps__empty">
              No apps are available yet.
            </div>
          )}
        </div>
      )}
    </WorkspaceCatalogLayout>
  );
};

export default AppsView;
