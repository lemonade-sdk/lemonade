import React, { useState, useEffect, memo, useCallback } from 'react';

// Remote apps JSON URL (same as marketplace)
const APPS_JSON_URL = 'https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps.json';

interface App {
  name: string;
  logo?: string;
  pinned?: boolean;
  links?: {
    app?: string;
  };
}

interface CenterPanelMenuProps {
  onOpenMarketplace: () => void;
}

const CenterPanelMenu: React.FC<CenterPanelMenuProps> = memo(({ onOpenMarketplace }) => {
  const [pinnedApps, setPinnedApps] = useState<App[]>([]);
  const [isLoading, setIsLoading] = useState(true);

  // Fetch pinned apps on mount
  useEffect(() => {
    const fetchApps = async () => {
      try {
        const response = await fetch(APPS_JSON_URL);
        if (!response.ok) throw new Error('Failed to fetch apps');

        const data = await response.json();
        const apps: App[] = data.apps || [];

        // Filter pinned apps and take top 9
        const pinned = apps.filter(app => app.pinned).slice(0, 9);
        setPinnedApps(pinned);
      } catch (error) {
        console.warn('Failed to fetch pinned apps:', error);
        setPinnedApps([]);
      } finally {
        setIsLoading(false);
      }
    };

    fetchApps();
  }, []);

  const handleMarketplaceClick = useCallback(() => {
    onOpenMarketplace();
  }, [onOpenMarketplace]);

  // Placeholder logo for apps without one
  const getLogoUrl = (app: App) => {
    if (app.logo) return app.logo;
    return `data:image/svg+xml,<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100"><rect fill="%23333" width="100" height="100" rx="12"/><text x="50" y="58" text-anchor="middle" font-size="36" fill="%23666">${encodeURIComponent(app.name.charAt(0).toUpperCase())}</text></svg>`;
  };

  return (
    <div className="center-panel-menu">
      <div className="center-panel-menu-content">
        <h2 className="center-panel-menu-title">Welcome to Lemonade</h2>
        <p className="center-panel-menu-subtitle">Your local AI control center</p>

        <div className="center-panel-menu-cards">
          {/* Marketplace Card */}
          <button
            className="center-panel-menu-card marketplace-card"
            onClick={handleMarketplaceClick}
          >
            <div className="menu-card-header">
              <div className="menu-card-title-section">
                <h3 className="menu-card-title">Marketplace</h3>
                <p className="menu-card-description">Discover local AI apps from our partners and community</p>
              </div>
            </div>

            {/* 3x3 Grid of Pinned Apps */}
            <div className="pinned-apps-grid">
              {isLoading ? (
                // Loading skeleton
                Array.from({ length: 9 }).map((_, i) => (
                  <div key={i} className="pinned-app-item loading">
                    <div className="pinned-app-icon-skeleton" />
                  </div>
                ))
              ) : (
                // Actual apps or placeholders
                Array.from({ length: 9 }).map((_, i) => {
                  const app = pinnedApps[i];
                  if (app) {
                    return (
                      <div key={i} className="pinned-app-item">
                        <img
                          src={getLogoUrl(app)}
                          alt={app.name}
                          className="pinned-app-icon"
                          onError={(e) => {
                            (e.target as HTMLImageElement).src = `data:image/svg+xml,<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100"><rect fill="%23333" width="100" height="100" rx="12"/><text x="50" y="58" text-anchor="middle" font-size="36" fill="%23666">${encodeURIComponent(app.name.charAt(0).toUpperCase())}</text></svg>`;
                          }}
                        />
                      </div>
                    );
                  } else {
                    return (
                      <div key={i} className="pinned-app-item empty">
                        <div className="pinned-app-icon-empty" />
                      </div>
                    );
                  }
                })
              )}
            </div>

            <div className="menu-card-action">
              <span>Open Marketplace</span>
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <polyline points="9 18 15 12 9 6"/>
              </svg>
            </div>
          </button>
        </div>
      </div>
    </div>
  );
});

CenterPanelMenu.displayName = 'CenterPanelMenu';

export default CenterPanelMenu;
