import React, { useEffect, useMemo, useState } from 'react';
import type { IconName } from './Icon';
import WorkspaceSectionRail, { type WorkspaceSectionDefinition } from './WorkspaceSectionRail';
import {
  WorkspaceActionButton,
  WorkspaceActionGroup,
  WorkspacePaneHeader,
  WorkspaceResourceList,
  WorkspaceResourceRow,
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

function categorySectionId(category: string): string {
  const safeCategory = encodeURIComponent(category.trim().toLowerCase())
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

function appHasCategory(app: MarketplaceApp, category: string): boolean {
  const normalizedFilter = normalizedCategory(category);
  return (app.category || []).some(item => normalizedCategory(item) === normalizedFilter);
}

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
  const [railCollapsed, setRailCollapsed] = useState(false);

  const categories = useMemo(() => {
    const categoryByKey = new Map<string, string>();
    marketplaceApps.flatMap(app => app.category || []).forEach(rawCategory => {
      const category = rawCategory.trim();
      const key = normalizedCategory(category);
      if (key && !categoryByKey.has(key)) categoryByKey.set(key, category);
    });
    return Array.from(categoryByKey.values()).sort((left, right) => left.localeCompare(right));
  }, [marketplaceApps]);

  useEffect(() => {
    if (categoryFilter && !categories.includes(categoryFilter)) setCategoryFilter(null);
  }, [categories, categoryFilter]);

  const categorySections = useMemo<WorkspaceSectionDefinition<string>[]>(() => [
    {
      id: ALL_APPS_SECTION,
      label: 'All Apps',
      description: marketplaceLoading
        ? 'Loading directory'
        : marketplaceError
          ? 'Directory unavailable'
          : appCountLabel(marketplaceApps.length),
      icon: 'globe',
    },
    ...categories.map(category => {
      const count = marketplaceApps.filter(app => appHasCategory(app, category)).length;
      return {
        id: categorySectionId(category),
        label: categoryLabel(category),
        description: appCountLabel(count),
        icon: categoryIcon(category),
      };
    }),
  ], [categories, marketplaceApps, marketplaceError, marketplaceLoading]);

  const categoryBySection = useMemo(() => new Map(
    categories.map(category => [categorySectionId(category), category]),
  ), [categories]);

  const activeCategorySection = categoryFilter ? categorySectionId(categoryFilter) : ALL_APPS_SECTION;

  const filteredMarketplaceApps = useMemo(() => marketplaceApps
    .filter(app => !categoryFilter || appHasCategory(app, categoryFilter))
    .sort((a, b) => Number(Boolean(b.pinned)) - Number(Boolean(a.pinned)) || a.name.localeCompare(b.name)),
  [categoryFilter, marketplaceApps]);

  const openExternal = (url?: string) => {
    if (!url) return;
    const hostApi = (window as unknown as { api?: { openExternal?: (url: string) => void } }).api;
    if (hostApi?.openExternal) {
      hostApi.openExternal(url);
      return;
    }
    window.open(url, '_blank', 'noopener,noreferrer');
  };

  const visibleCount = filteredMarketplaceApps.length;
  const paneTitle = categoryFilter ? categoryLabel(categoryFilter) : 'All Apps';
  const paneSubtitle = categoryFilter
    ? `${appCountLabel(visibleCount)} in ${categoryLabel(categoryFilter)}.`
    : 'Compatible clients and tools for Lemonade.';

  return (
    <section
      className={`apps-workspace${railCollapsed ? ' workspace--rail-collapsed' : ''}`}
      data-view="apps"
    >
      <WorkspaceSectionRail
        sections={categorySections}
        activeSection={activeCategorySection}
        onSectionChange={section => setCategoryFilter(categoryBySection.get(section) ?? null)}
        collapsed={railCollapsed}
        onCollapsedChange={setRailCollapsed}
        panelId="apps-types-panel"
        railLabel="App type navigation"
        navigationLabel="App categories"
        railClassName="apps__rail"
        navClassName="apps__nav"
        headerTitle="App Types"
        sidebarLabel="app types"
        headerIcon="layers"
        mobileMenuLabel="Open app types"
      />

      <section className="workspace-pane apps__main" aria-labelledby="apps-pane-title">
        <WorkspacePaneHeader
          title={paneTitle}
          subtitle={paneSubtitle}
          titleId="apps-pane-title"
        />

        <div className="apps__content workspace-pane__scroll">
          <section className="apps__directory" aria-label={paneTitle}>
            {marketplaceLoading ? (
              <div className="apps__empty" role="status">Loading apps...</div>
            ) : marketplaceError ? (
              <div className="apps__error" role="alert">Apps unavailable: {marketplaceError}</div>
            ) : (
              <WorkspaceResourceList label={`${paneTitle} directory`} className="apps__resource-list">
                {filteredMarketplaceApps.map(app => (
                  <WorkspaceResourceRow
                    key={app.id || app.name}
                    title={app.name}
                    description={app.description || 'No description available.'}
                    metadata={app.category?.join(' · ')}
                    leading={app.logo
                      ? <img className="apps__app-logo" src={app.logo} alt="" />
                      : <span className="apps__app-logo apps__app-logo--fallback">{app.name.slice(0, 1).toUpperCase()}</span>}
                    actions={<WorkspaceActionGroup className="apps__marketplace-actions" label={`Links for ${app.name}`}>
                      {app.links?.app && <WorkspaceActionButton appearance="secondary" size="small" icon="globe" onClick={() => openExternal(app.links?.app)}>Visit</WorkspaceActionButton>}
                      {app.links?.guide && <WorkspaceActionButton appearance="quiet" size="small" onClick={() => openExternal(app.links?.guide)}>Guide</WorkspaceActionButton>}
                      {app.links?.video && <WorkspaceActionButton appearance="quiet" size="small" onClick={() => openExternal(app.links?.video)}>Video</WorkspaceActionButton>}
                    </WorkspaceActionGroup>}
                  />
                ))}
                {filteredMarketplaceApps.length === 0 && (
                  <div className="apps__empty">
                    No apps are available for this type.
                  </div>
                )}
              </WorkspaceResourceList>
            )}
          </section>
        </div>
      </section>
    </section>
  );
};

export default AppsView;
