import React, { useEffect, useMemo, useRef, useState } from 'react';
import { BookOpen, ExternalLink } from 'lucide-react';
import { MarketplaceApp, MarketplaceCategory, APPS_JSON_URL } from './utils/marketplace';

interface MarketplacePanelProps {
  searchQuery: string;
  selectedCategory: string;
  onCategoriesLoaded?: (categories: MarketplaceCategory[]) => void;
}

const MarketplacePanel: React.FC<MarketplacePanelProps> = ({ searchQuery, selectedCategory, onCategoriesLoaded }) => {
  const [marketplaceApps, setMarketplaceApps] = useState<MarketplaceApp[]>([]);
  const [marketplaceCategories, setMarketplaceCategories] = useState<MarketplaceCategory[]>([]);
  const [marketplaceLoading, setMarketplaceLoading] = useState(true);
  const [marketplaceError, setMarketplaceError] = useState<string | null>(null);
  const onCategoriesLoadedRef = useRef(onCategoriesLoaded);
  onCategoriesLoadedRef.current = onCategoriesLoaded;

  useEffect(() => {
    let isMounted = true;

    const fetchMarketplaceApps = async () => {
      setMarketplaceLoading(true);
      setMarketplaceError(null);

      try {
        const response = await fetch(APPS_JSON_URL);
        if (!response.ok) {
          throw new Error(`Failed to fetch marketplace apps: HTTP ${response.status}`);
        }

        const data = await response.json();
        if (!isMounted) return;

        const apps: MarketplaceApp[] = Array.isArray(data?.apps) ? data.apps : [];
        const categories: MarketplaceCategory[] = Array.isArray(data?.categories) ? data.categories : [];
        setMarketplaceApps(apps);
        setMarketplaceCategories(categories);
        onCategoriesLoadedRef.current?.(categories);
      } catch (error) {
        if (!isMounted) return;
        setMarketplaceError(error instanceof Error ? error.message : 'Unknown error');
      } finally {
        if (isMounted) {
          setMarketplaceLoading(false);
        }
      }
    };

    fetchMarketplaceApps();
    return () => {
      isMounted = false;
    };
  }, []);

  const openExternalLink = (url?: string) => {
    if (!url) return;
    // Open documentation pages in-app via iframe
    if (url.endsWith('.html') && (url.includes('lemonade-server.ai') || url.startsWith('/'))) {
      window.dispatchEvent(new CustomEvent('open-external-content', { detail: { url } }));
      return;
    }
    if (window.api?.openExternal) {
      window.api.openExternal(url);
      return;
    }
    window.open(url, '_blank', 'noopener,noreferrer');
  };

  const filteredApps = useMemo(() => {
    return marketplaceApps
      .filter((app) => {
        if (selectedCategory !== 'all') {
          return Array.isArray(app.category) && app.category.includes(selectedCategory);
        }
        return true;
      })
      .filter((app) => {
        if (!searchQuery.trim()) return true;
        const query = searchQuery.toLowerCase();
        return (
          app.name.toLowerCase().includes(query) ||
          (app.description || '').toLowerCase().includes(query) ||
          (app.category || []).some(category => category.toLowerCase().includes(query))
        );
      })
      .sort((a, b) => Number(Boolean(b.pinned)) - Number(Boolean(a.pinned)) || a.name.localeCompare(b.name));
  }, [marketplaceApps, searchQuery, selectedCategory]);

  if (marketplaceLoading) {
    return <div className="left-panel-empty-state">Loading marketplace apps...</div>;
  }
  if (marketplaceError) {
    return <div className="left-panel-empty-state">Marketplace unavailable: {marketplaceError}</div>;
  }
  if (filteredApps.length === 0) {
    return <div className="left-panel-empty-state">No apps match your current filter.</div>;
  }

  return (
    <div className="left-panel-row-list">
      {filteredApps.map((app) => (
        <div key={app.id} className="left-panel-row-item marketplace-app-card">
          <div className="left-panel-row-main marketplace-app-main">
            <div className="left-panel-app-icon-wrap">
              {app.logo ? (
                <img className="left-panel-app-icon" src={app.logo} alt={app.name} />
              ) : (
                <span className="left-panel-app-fallback">{app.name.charAt(0).toUpperCase()}</span>
              )}
            </div>
            <div className="left-panel-row-text marketplace-app-content">
              <div className="marketplace-title-row">
                <div className="left-panel-row-title marketplace-app-title">{app.name}</div>
                {Array.isArray(app.category) && app.category.length > 0 && (
                  <div className="marketplace-app-categories">{app.category[0]}</div>
                )}
              </div>
              <div className="left-panel-row-meta marketplace-app-description">{app.description || 'No description available'}</div>
              {(app.links?.guide || app.links?.video || app.links?.app) && (
                <div className="left-panel-row-actions marketplace-app-actions">
                  {app.links?.app && (
                    <button className="left-panel-link-btn primary" title="Visit app" onClick={() => openExternalLink(app.links?.app)}>
                      <ExternalLink size={12} strokeWidth={1.9} />
                      <span>Visit</span>
                    </button>
                  )}
                  {app.links?.guide && (
                    <button className="left-panel-link-btn" title="Open guide" onClick={() => openExternalLink(app.links?.guide)}>
                      <BookOpen size={12} strokeWidth={1.9} />
                      <span>Guide</span>
                    </button>
                  )}
                  {app.links?.video && (
                    <button className="left-panel-link-btn" title="Watch video" onClick={() => openExternalLink(app.links?.video)}>
                      <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                        <polygon points="6 4 20 12 6 20 6 4" fill="currentColor" />
                      </svg>
                      <span>Video</span>
                    </button>
                  )}
                </div>
              )}
            </div>
          </div>
        </div>
      ))}
    </div>
  );
};

export { MarketplaceCategory };
export default MarketplacePanel;
