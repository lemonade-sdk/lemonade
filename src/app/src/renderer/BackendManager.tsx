import React, { useState, useCallback, useEffect } from 'react';
import { useSystem } from './hooks/useSystem';
import { useConfirmDialog } from './ConfirmDialog';
import { installBackend, uninstallBackend } from './utils/backendInstaller';
import { Recipe, BackendInfo } from './utils/systemData';
import { RECIPE_DISPLAY_NAMES } from './utils/recipeNames';

const RECIPE_ORDER = new Map([
  'llamacpp',
  'whispercpp',
  'sd-cpp',
  'kokoro',
  'flm',
  'ryzenai-llm',
].map((recipe, index) => [recipe, index]));

const STATUS_INDICATOR_CLASS: Record<BackendInfo['state'], string> = {
  installed: 'available',
  installable: 'not-downloaded',
  update_required: 'update-required',
  unsupported: 'unsupported',
};

interface GithubReleaseRef {
  owner: string;
  repo: string;
  tag: string;
}

const parseGithubReleaseUrl = (url?: string): GithubReleaseRef | null => {
  if (!url) return null;
  const match = url.match(/github\.com\/([^/]+)\/([^/]+)\/releases\/tag\/(.+)$/);
  if (!match) return null;
  return { owner: match[1], repo: match[2], tag: match[3] };
};

interface BackendManagerProps {
  searchQuery: string;
  showError: (msg: string) => void;
  showSuccess: (msg: string) => void;
}

const BackendManager: React.FC<BackendManagerProps> = ({ searchQuery, showError, showSuccess }) => {
  const { systemInfo, isLoading, ensureSystemInfoLoaded } = useSystem();
  const { confirm, ConfirmDialog } = useConfirmDialog();
  const [installingBackends, setInstallingBackends] = useState<Set<string>>(new Set());
  const [hoveredBackend, setHoveredBackend] = useState<string | null>(null);
  const [backendAssetSizes, setBackendAssetSizes] = useState<Record<string, number>>({});

  // Ensure system info is loaded when the backend manager is opened
  useEffect(() => {
    ensureSystemInfoLoaded();
  }, [ensureSystemInfoLoaded]);

  const recipes = systemInfo?.recipes;

  // Fetch asset sizes from GitHub Releases API
  useEffect(() => {
    if (!recipes) return;

    const pendingByRelease = new Map<string, Set<string>>();
    Object.values(recipes).forEach((recipe: Recipe) => {
      Object.values(recipe.backends).forEach((backend: BackendInfo) => {
        const releaseUrl = backend.release_url;
        const filename = backend.download_filename;
        if (!releaseUrl || !filename) return;
        if (typeof backend.download_size_mb === 'number' || typeof backend.download_size_bytes === 'number') return;

        const cacheKey = `${releaseUrl}:${filename}`;
        if (typeof backendAssetSizes[cacheKey] === 'number') return;

        if (!pendingByRelease.has(releaseUrl)) {
          pendingByRelease.set(releaseUrl, new Set());
        }
        pendingByRelease.get(releaseUrl)!.add(filename);
      });
    });

    if (pendingByRelease.size === 0) return;

    let isCancelled = false;

    const fetchReleaseAssets = async () => {
      const discoveredSizes: Record<string, number> = {};

      await Promise.all(
        Array.from(pendingByRelease.entries()).map(async ([releaseUrl, fileNames]) => {
          const parsed = parseGithubReleaseUrl(releaseUrl);
          if (!parsed) return;

          try {
            const response = await fetch(`https://api.github.com/repos/${parsed.owner}/${parsed.repo}/releases/tags/${parsed.tag}`);
            if (!response.ok) return;
            const data = await response.json();
            const assets = Array.isArray(data?.assets) ? data.assets : [];
            assets.forEach((asset: any) => {
              if (fileNames.has(asset?.name) && typeof asset?.size === 'number') {
                discoveredSizes[`${releaseUrl}:${asset.name}`] = asset.size;
              }
            });
          } catch {
            // Size fallback is best-effort
          }
        })
      );

      if (isCancelled || Object.keys(discoveredSizes).length === 0) return;
      setBackendAssetSizes((prev) => ({ ...prev, ...discoveredSizes }));
    };

    fetchReleaseAssets();
    return () => {
      isCancelled = true;
    };
  }, [backendAssetSizes, recipes]);

  const getBackendSizeLabel = useCallback((backendInfo: BackendInfo): string | null => {
    if (typeof backendInfo.download_size_mb === 'number' && backendInfo.download_size_mb > 0) {
      return `${Math.round(backendInfo.download_size_mb)} MB`;
    }

    if (typeof backendInfo.download_size_bytes === 'number' && backendInfo.download_size_bytes > 0) {
      return `${Math.round(backendInfo.download_size_bytes / (1024 * 1024))} MB`;
    }

    if (backendInfo.release_url && backendInfo.download_filename) {
      const bytes = backendAssetSizes[`${backendInfo.release_url}:${backendInfo.download_filename}`];
      if (typeof bytes === 'number' && bytes > 0) {
        return `${Math.round(bytes / (1024 * 1024))} MB`;
      }
      return '...';
    }

    return null;
  }, [backendAssetSizes]);

  const openExternalLink = useCallback((url?: string) => {
    if (!url) return;
    if (window.api?.openExternal) {
      window.api.openExternal(url);
      return;
    }
    window.open(url, '_blank', 'noopener,noreferrer');
  }, []);

  const handleInstallBackend = useCallback(async (recipe: string, backend: string) => {
    const key = `${recipe}:${backend}`;
    setInstallingBackends(prev => new Set(prev).add(key));
    try {
      await installBackend(recipe, backend, true);
      showSuccess(`${RECIPE_DISPLAY_NAMES[recipe] || recipe} ${backend} installed successfully.`);
      // No manual refreshSystem() needed — installBackend() dispatches 'backendsUpdated'
      // which useSystem auto-listens for and refreshes.
    } catch (error) {
      showError(`Failed to install backend: ${error instanceof Error ? error.message : 'Unknown error'}`);
    } finally {
      setInstallingBackends(prev => {
        const next = new Set(prev);
        next.delete(key);
        return next;
      });
    }
  }, [showError, showSuccess]);

  const handleCopyAction = useCallback(async (recipe: string, backend: string, action?: string) => {
    if (!action) return;
    try {
      await navigator.clipboard.writeText(action);
      showSuccess(`Copied action for ${RECIPE_DISPLAY_NAMES[recipe] || recipe} ${backend}.`);
    } catch {
      showError('Failed to copy action to clipboard.');
    }
  }, [showError, showSuccess]);

  const handleUninstallBackend = useCallback(async (recipe: string, backend: string) => {
    const confirmed = await confirm({
      title: 'Uninstall Backend',
      message: `Are you sure you want to uninstall ${RECIPE_DISPLAY_NAMES[recipe] || recipe} ${backend}?`,
      confirmText: 'Uninstall',
      cancelText: 'Cancel',
      danger: true
    });
    if (!confirmed) return;

    try {
      await uninstallBackend(recipe, backend);
      showSuccess(`${RECIPE_DISPLAY_NAMES[recipe] || recipe} ${backend} uninstalled successfully.`);
      // No manual refreshSystem() needed — uninstallBackend() dispatches 'backendsUpdated'
    } catch (error) {
      showError(`Failed to uninstall backend: ${error instanceof Error ? error.message : 'Unknown error'}`);
    }
  }, [confirm, showError, showSuccess]);

  const groupedBackends: Array<[string, Array<[string, BackendInfo]>]> = recipes
    ? Object.entries(recipes)
      .map(([recipeName, recipe]: [string, Recipe]) => {
        const backends = Object.entries(recipe.backends).filter(([, info]) => info.state !== 'unsupported');
        return [recipeName, backends] as [string, Array<[string, BackendInfo]>];
      })
      .filter(([, backends]) => backends.length > 0)
      .sort(([a], [b]) => {
        const aOrder = RECIPE_ORDER.get(a) ?? Number.MAX_SAFE_INTEGER;
        const bOrder = RECIPE_ORDER.get(b) ?? Number.MAX_SAFE_INTEGER;
        if (aOrder !== bOrder) return aOrder - bOrder;
        return a.localeCompare(b);
      })
    : [];

  const query = searchQuery.trim().toLowerCase();
  const visibleGroups = groupedBackends
    .map(([recipeName, backends]) => {
      const filteredBackends = backends.filter(([backendName, info]) => {
        if (!query) return true;
        const haystack = `${recipeName} ${backendName} ${info.version || ''} ${info.state} ${info.message || ''}`.toLowerCase();
        return haystack.includes(query);
      });
      return [recipeName, filteredBackends] as [string, Array<[string, BackendInfo]>];
    })
    .filter(([, backends]) => backends.length > 0);

  if (isLoading || !recipes) {
    return (
      <>
        <ConfirmDialog />
        <div className="left-panel-empty-state">Loading backends...</div>
      </>
    );
  }

  if (visibleGroups.length === 0) {
    return (
      <>
        <ConfirmDialog />
        <div className="left-panel-empty-state">No backends match your current filter.</div>
      </>
    );
  }

  return (
    <>
      <ConfirmDialog />
      {visibleGroups.map(([recipeName, backends]) => (
        <div key={recipeName} className="model-category">
          <div className="model-category-header static">
            <span className="category-label">{RECIPE_DISPLAY_NAMES[recipeName] || recipeName}</span>
            <span className="category-count">({backends.length})</span>
          </div>
          <div className="model-list">
            {backends.map(([backendName, info]) => {
              const key = `${recipeName}:${backendName}`;
              const isInstalling = installingBackends.has(key);
              const isHovered = hoveredBackend === key;
              const sizeLabel = getBackendSizeLabel(info);
              return (
                <div
                  className="model-item backend-row-item"
                  key={key}
                  onMouseEnter={() => setHoveredBackend(key)}
                  onMouseLeave={() => setHoveredBackend(null)}
                >
                  <div className="model-item-content">
                    <div className="model-info-left backend-row-main">
                      <div className="backend-row-head">
                        <span className="model-name backend-name">
                          <span className={`model-status-indicator ${STATUS_INDICATOR_CLASS[info.state]}`}>●</span>
                          {backendName}
                        </span>
                        <div className="backend-inline-meta">
                          {info.release_url ? (
                            <button
                              className="backend-version-link"
                              onClick={() => openExternalLink(info.release_url)}
                              title="Open backend release page"
                            >
                              {info.version || 'Not installed'}
                            </button>
                          ) : info.version ? (
                            <span className="backend-version">{info.version}</span>
                          ) : (
                            <span className="backend-version">{info.state}</span>
                          )}
                          {sizeLabel && (
                            <>
                              <span className="backend-meta-separator">•</span>
                              <span className="backend-size">{sizeLabel}</span>
                            </>
                          )}
                        </div>
                      </div>
                      {info.message && <div className="backend-status-message">{info.message}</div>}
                    </div>
                    {isHovered && (
                      <span className="model-actions">
                        {(info.state === 'installable' || info.state === 'update_required') && (
                          <button
                            className={`model-action-btn ${info.state === 'update_required' ? 'update-btn' : 'download-btn'}`}
                            title={info.state === 'update_required' ? 'Update backend' : 'Install backend'}
                            disabled={isInstalling}
                            onClick={() => handleInstallBackend(recipeName, backendName)}
                          >
                            {isInstalling ? '…' : (
                              <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                                <polyline points="7 10 12 15 17 10" />
                                <line x1="12" y1="15" x2="12" y2="3" />
                              </svg>
                            )}
                          </button>
                        )}
                        {info.state === 'installed' && (
                          <button
                            className="model-action-btn delete-btn"
                            title="Uninstall backend"
                            onClick={() => handleUninstallBackend(recipeName, backendName)}
                          >
                            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                              <polyline points="3 6 5 6 21 6" />
                              <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
                            </svg>
                          </button>
                        )}
                        {(info.state === 'installable' || info.state === 'update_required') && info.action && (
                          <button
                            className="model-action-btn"
                            title={info.action}
                            onClick={() => handleCopyAction(recipeName, backendName, info.action)}
                          >
                            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                              <rect x="9" y="9" width="13" height="13" rx="2" ry="2" />
                              <path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1" />
                            </svg>
                          </button>
                        )}
                      </span>
                    )}
                  </div>
                </div>
              );
            })}
          </div>
        </div>
      ))}
    </>
  );
};

export default BackendManager;
