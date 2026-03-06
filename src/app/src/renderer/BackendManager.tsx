import React, { useState, useCallback, useEffect } from 'react';

import { useSystem } from './hooks/useSystem';
import { Recipe, BackendInfo } from './utils/systemData';
import { RECIPE_DISPLAY_NAMES } from './utils/recipeNames';
import ConnectedBackendRow from './components/ConnectedBackendRow';

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
    return <div className="left-panel-empty-state">Loading backends...</div>;
  }

  if (visibleGroups.length === 0) {
    return <div className="left-panel-empty-state">No backends match your current filter.</div>;
  }

  return (
    <>
      {visibleGroups.map(([recipeName, backends]) => (
        <div key={recipeName} className="model-category">
          <div className="model-category-header static">
            <span className="category-label">{RECIPE_DISPLAY_NAMES[recipeName] || recipeName}</span>
            <span className="category-count">({backends.length})</span>
          </div>
          <div className="model-list">
            {backends.map(([backendName, info]) => (
              <ConnectedBackendRow
                key={`${recipeName}:${backendName}`}
                recipe={recipeName}
                backend={backendName}
                showError={showError}
                showSuccess={showSuccess}
                variant="full"
                sizeLabel={getBackendSizeLabel(info)}
                onOpenReleaseUrl={openExternalLink}
              />
            ))}
          </div>
        </div>
      ))}
    </>
  );
};

export default BackendManager;
