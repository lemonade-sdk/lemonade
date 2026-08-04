import React, { useEffect, useMemo, useState } from 'react';
import { friendlyErrorMessage } from '../api';
import { Icon } from './Icon';
import {
  WorkspaceActionButton,
  WorkspaceActionGroup,
  WorkspaceMetadataChip,
  WorkspacePaneHeader,
  WorkspaceResourceList,
  WorkspaceResourceRow,
} from './WorkspacePanels';

type MarketplaceApp = {
  id: string;
  name: string;
  description?: string;
  category?: string[];
  logo?: string;
  pinned?: boolean;
  links?: { app?: string; guide?: string; video?: string };
};

const MARKETPLACE_URL = 'https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps.json';

const AppsView: React.FC<{ isActive: boolean; embedded?: boolean }> = ({ isActive, embedded = false }) => {
  const [marketplaceApps, setMarketplaceApps] = useState<MarketplaceApp[]>([]);
  const [marketplaceSearch, setMarketplaceSearch] = useState('');
  const [categoryFilter, setCategoryFilter] = useState<string | null>(null);
  const [marketplaceError, setMarketplaceError] = useState<string | null>(null);
  const [marketplaceLoading, setMarketplaceLoading] = useState(false);

  useEffect(() => {
    if (!isActive || marketplaceApps.length > 0) return;
    let cancelled = false;
    setMarketplaceLoading(true);
    fetch(MARKETPLACE_URL)
      .then(response => {
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return response.json();
      })
      .then(data => {
        if (cancelled) return;
        const apps = Array.isArray(data?.apps) ? data.apps as MarketplaceApp[] : [];
        setMarketplaceApps(apps);
        setMarketplaceError(null);
      })
      .catch(err => { if (!cancelled) setMarketplaceError(friendlyErrorMessage(err)); })
      .finally(() => { if (!cancelled) setMarketplaceLoading(false); });
    return () => { cancelled = true; };
  }, [isActive, marketplaceApps.length]);

  const categories = useMemo(() => Array.from(new Set(
    marketplaceApps.flatMap(app => app.category || [])
      .map(category => category.trim())
      .filter(Boolean),
  )).sort((left, right) => left.localeCompare(right)), [marketplaceApps]);

  const filteredMarketplaceApps = useMemo(() => {
    const query = marketplaceSearch.trim().toLowerCase();
    return marketplaceApps
      .filter(app => {
        const categories = app.category || [];
        const matchesCategory = !categoryFilter || categories.some(category => category.toLowerCase() === categoryFilter.toLowerCase());
        const matchesQuery = !query || app.name.toLowerCase().includes(query)
          || (app.description || '').toLowerCase().includes(query)
          || categories.join(' ').toLowerCase().includes(query);
        return matchesCategory && matchesQuery;
      })
      .sort((a, b) => Number(Boolean(b.pinned)) - Number(Boolean(a.pinned)) || a.name.localeCompare(b.name));
  }, [categoryFilter, marketplaceApps, marketplaceSearch]);

  const openExternal = (url?: string) => {
    if (!url) return;
    const hostApi = (window as unknown as { api?: { openExternal?: (url: string) => void } }).api;
    if (hostApi?.openExternal) {
      hostApi.openExternal(url);
      return;
    }
    window.open(url, '_blank', 'noopener,noreferrer');
  };

  const directoryContent = (
    <section className="connect__section connect__section--marketplace">
          <div className="connect__section-head connect__section-head--search">
            <input
              className="input connect__marketplace-search"
              value={marketplaceSearch}
              onChange={e => setMarketplaceSearch(e.target.value)}
              placeholder="Search apps..."
              aria-label="Search apps"
            />
          </div>
          {categories.length > 0 && (
            <div className="apps__category-filters" role="group" aria-label="Filter apps by category">
              <WorkspaceMetadataChip
                emphasis={categoryFilter === null ? 'medium' : 'low'}
                tone={categoryFilter === null ? 'accent' : 'neutral'}
                buttonProps={{ onClick: () => setCategoryFilter(null), 'aria-pressed': categoryFilter === null }}
              >
                All
              </WorkspaceMetadataChip>
              {categories.map(category => (
                <WorkspaceMetadataChip
                  key={category}
                  emphasis={categoryFilter === category ? 'medium' : 'low'}
                  tone={categoryFilter === category ? 'accent' : 'neutral'}
                  buttonProps={{ onClick: () => setCategoryFilter(category), 'aria-pressed': categoryFilter === category }}
                >
                  {category}
                </WorkspaceMetadataChip>
              ))}
            </div>
          )}
          {marketplaceLoading ? <div className="connect__empty">Loading apps...</div> : marketplaceError ? <div className="connect__error">Apps unavailable: {marketplaceError}</div> : (
            <WorkspaceResourceList label="Compatible apps">
              {filteredMarketplaceApps.map(app => (
                <WorkspaceResourceRow
                  key={app.id || app.name}
                  title={app.name}
                  description={app.description || 'No description available.'}
                  metadata={app.category?.[0]}
                  leading={app.logo ? <img className="connect__app-logo" src={app.logo} alt="" /> : <span className="connect__app-logo connect__app-logo--fallback">{app.name.slice(0, 1).toUpperCase()}</span>}
                  actions={<WorkspaceActionGroup className="connect__marketplace-actions" label={`Links for ${app.name}`}>
                    {app.links?.app && <WorkspaceActionButton appearance="secondary" size="small" icon="globe" onClick={() => openExternal(app.links?.app)}>Visit</WorkspaceActionButton>}
                    {app.links?.guide && <WorkspaceActionButton appearance="quiet" size="small" onClick={() => openExternal(app.links?.guide)}>Guide</WorkspaceActionButton>}
                    {app.links?.video && <WorkspaceActionButton appearance="quiet" size="small" onClick={() => openExternal(app.links?.video)}>Video</WorkspaceActionButton>}
                  </WorkspaceActionGroup>}
                />
              ))}
              {filteredMarketplaceApps.length === 0 && <div className="connect__empty">No apps match your search.</div>}
            </WorkspaceResourceList>
          )}
    </section>
  );

  if (embedded) return directoryContent;

  return (
    <section className="workspace-pane connect__main apps__main" aria-labelledby="apps-pane-title" data-view="apps">
      <WorkspacePaneHeader
        title="Apps"
        subtitle="Compatible clients and tools for Lemonade."
        titleId="apps-pane-title"
      />
      <div className="connect__layout workspace-pane__scroll">
        {directoryContent}
      </div>
    </section>
  );
};

export default AppsView;
