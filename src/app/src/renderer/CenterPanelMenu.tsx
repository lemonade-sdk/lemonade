import React, { useState, useEffect, memo, useCallback, useMemo } from 'react';
import { Cpu, MessageSquare, Mic, Paintbrush, Music, LucideIcon } from 'lucide-react';
import { useSystem } from './hooks/useSystem';

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
  onOpenBackendManager: () => void;
}

interface SectionHeaderProps {
  title: string;
}

const BACKEND_PREVIEW_META: Record<string, { displayName: string; icon: LucideIcon; color: string }> = {
  'llamacpp': { displayName: 'Llama.cpp', icon: MessageSquare, color: '#a78bfa' },
  'whispercpp': { displayName: 'Whisper.cpp', icon: Mic, color: '#60a5fa' },
  'sd-cpp': { displayName: 'Stable Diffusion', icon: Paintbrush, color: '#f472b6' },
  'kokoro': { displayName: 'Kokoro TTS', icon: Music, color: '#f59e0b' },
  'flm': { displayName: 'FastFlowLM', icon: Cpu, color: '#34d399' },
  'ryzenai-llm': { displayName: 'Ryzen AI LLM', icon: Cpu, color: '#22d3ee' },
};

const FALLBACK_RECIPES = ['llamacpp', 'whispercpp', 'sd-cpp', 'kokoro', 'flm', 'ryzenai-llm'];
const PREVIEW_ORDER = new Map(FALLBACK_RECIPES.map((recipe, index) => [recipe, index]));

const SectionHeader: React.FC<SectionHeaderProps> = ({ title }) => (
  <div className="center-panel-section-header">
    <span className="center-panel-section-title">{title}</span>
    <span className="center-panel-section-action">View all →</span>
  </div>
);

const CenterPanelMenu: React.FC<CenterPanelMenuProps> = memo(({ onOpenMarketplace, onOpenBackendManager }) => {
  const [pinnedApps, setPinnedApps] = useState<App[]>([]);
  const [isLoading, setIsLoading] = useState(true);
  const { supportedRecipes } = useSystem();

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

  const backendPreviewItems = useMemo(() => {
    const recipeNames = Object.keys(supportedRecipes);
    const orderedNames = recipeNames.length > 0
      ? [...recipeNames].sort((a, b) => (PREVIEW_ORDER.get(a) ?? 999) - (PREVIEW_ORDER.get(b) ?? 999))
      : FALLBACK_RECIPES;

    return orderedNames.slice(0, 5).map((recipeName) => {
      const meta = BACKEND_PREVIEW_META[recipeName];
      return {
        key: recipeName,
        displayName: meta?.displayName || recipeName,
        icon: meta?.icon || Cpu,
        color: meta?.color || '#94a3b8',
      };
    });
  }, [supportedRecipes]);

  // Placeholder logo for apps without one
  const getLogoUrl = (app: App) => {
    if (app.logo) return app.logo;
    return `data:image/svg+xml,<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100"><rect fill="%23333" width="100" height="100" rx="12"/><text x="50" y="58" text-anchor="middle" font-size="36" fill="%23666">${encodeURIComponent(app.name.charAt(0).toUpperCase())}</text></svg>`;
  };

  return (
    <div className="center-panel-menu">
      <div className="center-panel-menu-content">
        <h2 className="center-panel-menu-title">Welcome to Lemonade</h2>
        <p className="center-panel-menu-subtitle">
          Your local AI control center - models, apps, and backends in one place.
        </p>

        <div className="center-panel-menu-cards">
          {/* Marketplace Card */}
          <button
            className="center-panel-menu-card marketplace-card"
            onClick={handleMarketplaceClick}
          >
            <SectionHeader title="Marketplace" />

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
                      <div key={i} className="pinned-app-item" title={app.name} aria-label={app.name}>
                        <img
                          src={getLogoUrl(app)}
                          alt={app.name}
                          className="pinned-app-icon"
                          onError={(e) => {
                            (e.target as HTMLImageElement).src = `data:image/svg+xml,<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100"><rect fill="%23333" width="100" height="100" rx="12"/><text x="50" y="58" text-anchor="middle" font-size="36" fill="%23666">${encodeURIComponent(app.name.charAt(0).toUpperCase())}</text></svg>`;
                          }}
                        />
                        <span className="pinned-app-name">{app.name}</span>
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
          </button>

          {/* Backend Manager Card */}
          <button
            className="center-panel-menu-card backend-manager-card"
            onClick={onOpenBackendManager}
          >
            <SectionHeader title="Backends" />

            <div className="backend-manager-preview">
              {backendPreviewItems.map((backend) => {
                const Icon = backend.icon;
                return (
                  <div className="backend-preview-item" key={backend.key}>
                    <span className="backend-preview-icon" style={{ '--backend-color': backend.color } as React.CSSProperties}>
                      <Icon size={14} strokeWidth={1.9} />
                    </span>
                    <span className="backend-preview-name">{backend.displayName}</span>
                  </div>
                );
              })}
            </div>
          </button>
        </div>
      </div>
    </div>
  );
});

CenterPanelMenu.displayName = 'CenterPanelMenu';

export default CenterPanelMenu;
