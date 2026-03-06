import React, { useState, useCallback, useEffect } from 'react';

import { useSystem } from './hooks/useSystem';
import { useConfirmDialog } from './ConfirmDialog';
import { installBackend, uninstallBackend } from './utils/backendInstaller';
import { Recipe, BackendInfo } from './utils/systemData';
import { RECIPE_DISPLAY_NAMES } from './utils/recipeNames';
import BackendRow from './components/BackendRow';

const RECIPE_ORDER = new Map([
  'llamacpp',
  'whispercpp',
  'sd-cpp',
  'kokoro',
  'flm',
  'ryzenai-llm',
].map((recipe, index) => [recipe, index]));

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
  const { systemInfo, isLoading, refresh } = useSystem();
  const { confirm, ConfirmDialog } = useConfirmDialog();
  const [installingBackends, setInstallingBackends] = useState<Set<string>>(new Set());
  const [hoveredBackend, setHoveredBackend] = useState<string | null>(null);
  const [backendAssetSizes, setBackendAssetSizes] = useState<Record<string, number>>({});

  // Refresh system info when the backend manager is opened
  useEffect(() => {
    refresh();
  }, [refresh]);

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
      const errorMessage = error instanceof Error ? error.message : 'Unknown error';
      showError(`Failed: ${errorMessage}`);

      // If the error has a help URL in the action, open it
      const backendInfo = systemInfo?.recipes?.[recipe]?.backends?.[backend];
      const action = backendInfo?.action;

      // Extract URL from action
      if (action) {
        const urlMatch = action.match(/https?:\/\/[^\s]+/);
        if (urlMatch) {
          window.dispatchEvent(new CustomEvent('open-external-content', { detail: { url: urlMatch[0] } }));
        }
      }
    } finally {
      setInstallingBackends(prev => {
        const next = new Set(prev);
        next.delete(key);
        return next;
      });
    }
  }, [showError, showSuccess, systemInfo]);

  const handleCopyAction = useCallback(async (recipe: string, backend: string, action?: string) => {
    if (!action) return;
    try {
      // If the action contains a lemonade-server.ai documentation URL, open it in-app
      if (action.match(/https:\/\/lemonade-server\.ai\/[^\s]+\.html/)) {
        const urlMatch = action.match(/https:\/\/lemonade-server\.ai\/[^\s]+/);
        if (urlMatch) {
          window.dispatchEvent(new CustomEvent('open-external-content', { detail: { url: urlMatch[0] } }));
          return;
        }
      }

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
              return (
                <BackendRow
                  key={key}
                  recipeName={recipeName}
                  backendName={backendName}
                  info={info}
                  isInstalling={installingBackends.has(key)}
                  sizeLabel={getBackendSizeLabel(info)}
                  hoverActions={true}
                  isHovered={hoveredBackend === key}
                  onMouseEnter={() => setHoveredBackend(key)}
                  onMouseLeave={() => setHoveredBackend(null)}
                  onInstall={handleInstallBackend}
                  onUninstall={handleUninstallBackend}
                  onCopyAction={handleCopyAction}
                  onOpenReleaseUrl={openExternalLink}
                />
              );
            })}
          </div>
        </div>
      ))}
    </>
  );
};

export default BackendManager;
