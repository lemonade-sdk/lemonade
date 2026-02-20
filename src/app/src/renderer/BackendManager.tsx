import React, { useState, useCallback, useEffect, useRef } from 'react';
import { useSystem } from './hooks/useSystem';
import { useConfirmDialog } from './ConfirmDialog';
import { ToastContainer, useToast } from './Toast';
import { serverFetch } from './utils/serverConfig';
import { installBackend } from './utils/backendInstaller';
import { Recipe, BackendInfo } from './utils/systemData';

const RECIPE_INFO: Record<string, { displayName: string; description: string }> = {
  'llamacpp': {
    displayName: 'Llama.cpp',
    description: 'Cross-platform LLM inference engine for text generation and chat.',
  },
  'whispercpp': {
    displayName: 'Whisper.cpp',
    description: 'Speech-to-text transcription powered by OpenAI Whisper.',
  },
  'sd-cpp': {
    displayName: 'StableDiffusion.cpp',
    description: 'Image generation using Stable Diffusion models.',
  },
  'kokoro': {
    displayName: 'Kokoro TTS',
    description: 'High-quality text-to-speech synthesis.',
  },
  'flm': {
    displayName: 'FastFlowLM',
    description: 'Optimized LLM inference for AMD Ryzen AI NPUs.',
  },
  'ryzenai-llm': {
    displayName: 'Ryzen AI LLM',
    description: 'LLM inference using AMD Ryzen AI acceleration.',
  },
};

const BACKEND_INFO: Record<string, { displayName: string; description: string }> = {
  'vulkan': {
    displayName: 'Vulkan',
    description: 'GPU acceleration via the cross-platform Vulkan API.',
  },
  'rocm': {
    displayName: 'ROCm',
    description: 'GPU acceleration for AMD Radeon GPUs via ROCm.',
  },
  'cuda': {
    displayName: 'CUDA',
    description: 'GPU acceleration for NVIDIA GPUs via CUDA.',
  },
  'cpu': {
    displayName: 'CPU',
    description: 'Runs on CPU without GPU requirements.',
  },
  'metal': {
    displayName: 'Metal',
    description: 'GPU acceleration for Apple Silicon and AMD GPUs on macOS.',
  },
  'npu': {
    displayName: 'NPU',
    description: 'Accelerated inference using the neural processing unit.',
  },
  'default': {
    displayName: 'Default',
    description: 'Default backend configuration.',
  },
};

const getRecipeDisplayName = (recipe: string): string =>
  RECIPE_INFO[recipe]?.displayName || recipe;

const getRecipeDescription = (recipe: string): string =>
  RECIPE_INFO[recipe]?.description || '';

const getBackendDisplayName = (backend: string): string =>
  BACKEND_INFO[backend]?.displayName || backend;

const getBackendDescription = (backend: string): string =>
  BACKEND_INFO[backend]?.description || '';

// Parse a release_url like "https://github.com/owner/repo/releases/tag/v1.0"
// into { owner: "owner", repo: "repo", tag: "v1.0" }
function parseReleaseUrl(url: string): { owner: string; repo: string; tag: string } | null {
  const match = url.match(/github\.com\/([^/]+)\/([^/]+)\/releases\/tag\/(.+)$/);
  if (!match) return null;
  return { owner: match[1], repo: match[2], tag: match[3] };
}

// Module-level cache so GitHub results persist across re-renders and re-mounts
const assetSizeCache: Record<string, number> = {};  // "release_url:filename" -> bytes
const fetchedReleases = new Set<string>();  // release_urls already fetched

/**
 * Fetch GitHub release assets for a set of release URLs and populate assetSizeCache.
 * Groups by unique release URL to avoid duplicate requests.
 */
async function fetchAssetSizes(
  releaseUrls: string[],
  onUpdate: () => void
): Promise<void> {
  const uniqueUrls = [...new Set(releaseUrls)].filter(u => !fetchedReleases.has(u));
  if (uniqueUrls.length === 0) return;

  await Promise.all(uniqueUrls.map(async (releaseUrl) => {
    fetchedReleases.add(releaseUrl);
    const parsed = parseReleaseUrl(releaseUrl);
    if (!parsed) return;

    try {
      const apiUrl = `https://api.github.com/repos/${parsed.owner}/${parsed.repo}/releases/tags/${parsed.tag}`;
      const resp = await fetch(apiUrl);
      if (!resp.ok) return;

      const data = await resp.json();
      for (const asset of data.assets || []) {
        assetSizeCache[`${releaseUrl}:${asset.name}`] = asset.size;
      }
      onUpdate();
    } catch {
      // GitHub API failure is non-critical; sizes just won't show
    }
  }));
}

function getAssetSizeMb(releaseUrl?: string, filename?: string): string {
  if (!releaseUrl || !filename) return '';
  const bytes = assetSizeCache[`${releaseUrl}:${filename}`];
  if (!bytes) return '';
  return `${Math.round(bytes / 1048576)} MB`;
}

const BackendManager: React.FC = () => {
  const { systemInfo, refresh } = useSystem();
  const { toasts, removeToast, showError, showSuccess } = useToast();
  const { confirm, ConfirmDialog } = useConfirmDialog();
  const [installingBackends, setInstallingBackends] = useState<Set<string>>(new Set());
  const [, setAssetTick] = useState(0);  // trigger re-render when asset sizes arrive
  const fetchedRef = useRef(false);

  const recipes = systemInfo?.recipes;

  // Fetch asset sizes from GitHub when recipes data is available
  useEffect(() => {
    if (!recipes || fetchedRef.current) return;
    fetchedRef.current = true;

    const releaseUrls: string[] = [];
    for (const recipe of Object.values(recipes)) {
      for (const backend of Object.values(recipe.backends)) {
        if (backend.supported && backend.release_url) {
          releaseUrls.push(backend.release_url);
        }
      }
    }

    if (releaseUrls.length > 0) {
      fetchAssetSizes(releaseUrls, () => setAssetTick(t => t + 1));
    }
  }, [recipes]);

  const handleInstall = useCallback(async (recipe: string, backend: string) => {
    const key = `${recipe}:${backend}`;
    setInstallingBackends(prev => new Set(prev).add(key));

    try {
      await installBackend(recipe, backend, true);
      showSuccess(`${getRecipeDisplayName(recipe)} ${getBackendDisplayName(backend)} installed successfully.`);
      await refresh();
    } catch (error) {
      showError(`Failed to install ${getRecipeDisplayName(recipe)} ${getBackendDisplayName(backend)}: ${error instanceof Error ? error.message : 'Unknown error'}`);
    } finally {
      setInstallingBackends(prev => {
        const next = new Set(prev);
        next.delete(key);
        return next;
      });
    }
  }, [refresh, showSuccess, showError]);

  const handleUninstall = useCallback(async (recipe: string, backend: string) => {
    const confirmed = await confirm({
      title: 'Uninstall Backend',
      message: `Are you sure you want to uninstall ${getRecipeDisplayName(recipe)} ${getBackendDisplayName(backend)}? You can reinstall it later.`,
      confirmText: 'Uninstall',
      cancelText: 'Cancel',
      danger: true,
    });

    if (!confirmed) return;

    try {
      const response = await serverFetch('/uninstall', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ recipe, backend }),
      });

      if (!response.ok) {
        const errorText = await response.text();
        throw new Error(errorText || response.statusText);
      }

      showSuccess(`${getRecipeDisplayName(recipe)} ${getBackendDisplayName(backend)} uninstalled successfully.`);
      await refresh();
    } catch (error) {
      showError(`Failed to uninstall ${getRecipeDisplayName(recipe)} ${getBackendDisplayName(backend)}: ${error instanceof Error ? error.message : 'Unknown error'}`);
    }
  }, [refresh, confirm, showSuccess, showError]);

  // Filter recipes to only those with at least one supported backend
  const supportedRecipes: [string, Recipe][] = recipes
    ? Object.entries(recipes).filter(([, recipe]) =>
        Object.values(recipe.backends).some(b => b.supported)
      )
    : [];

  return (
    <div className="backend-manager">
      <ToastContainer toasts={toasts} onRemove={removeToast} />
      <ConfirmDialog />

      <div className="backend-manager-header">
        <h3>Backend Manager</h3>
        <p>Install and manage AI inference backends</p>
      </div>

      <div className="backend-manager-content">
        {supportedRecipes.length === 0 && (
          <div className="backend-manager-empty">
            <p>No supported backends found for this system.</p>
          </div>
        )}

        {supportedRecipes.map(([recipeName, recipe]) => {
          const supportedBackends = Object.entries(recipe.backends).filter(
            ([, info]) => info.supported
          );

          return (
            <div key={recipeName} className="backend-recipe-section">
              <div className="backend-recipe-header">
                <h4 className="backend-recipe-title">{getRecipeDisplayName(recipeName)}</h4>
                <span className="backend-recipe-desc">{getRecipeDescription(recipeName)}</span>
              </div>
              <div className="backend-cards">
                {supportedBackends.map(([backendName, info]: [string, BackendInfo]) => {
                  const key = `${recipeName}:${backendName}`;
                  const isInstalling = installingBackends.has(key);
                  const versionText = info.version || '';
                  const sizeText = getAssetSizeMb(info.release_url, info.download_filename);

                  return (
                    <div key={backendName} className={`backend-card ${info.available ? 'installed' : ''}`}>
                      <div className="backend-card-info">
                        <div className="backend-card-title-row">
                          <span className={`backend-status-dot ${info.available ? 'installed' : 'not-installed'}`} />
                          <span className="backend-card-name">{getBackendDisplayName(backendName)}</span>
                          {versionText && (
                            <>
                              <span className="backend-card-separator">&middot;</span>
                              {info.release_url ? (
                                <a
                                  className="backend-version-link"
                                  href={info.release_url}
                                  target="_blank"
                                  rel="noopener noreferrer"
                                  onClick={(e) => e.stopPropagation()}
                                >
                                  {versionText}
                                </a>
                              ) : (
                                <span className="backend-version">{versionText}</span>
                              )}
                            </>
                          )}
                          {sizeText && (
                            <>
                              <span className="backend-card-separator">&middot;</span>
                              <span className="backend-size">{sizeText}</span>
                            </>
                          )}
                        </div>
                        <div className="backend-card-desc">
                          {getBackendDescription(backendName)}
                        </div>
                      </div>
                      <div className="backend-card-actions">
                        {info.available ? (
                          <button
                            className="backend-action-btn uninstall"
                            onClick={() => handleUninstall(recipeName, backendName)}
                            title="Uninstall backend"
                          >
                            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                              <polyline points="3 6 5 6 21 6" />
                              <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
                            </svg>
                          </button>
                        ) : (
                          <button
                            className="backend-action-btn install"
                            onClick={() => handleInstall(recipeName, backendName)}
                            disabled={isInstalling}
                            title="Install backend"
                          >
                            {isInstalling ? (
                              <div className="backend-install-spinner" />
                            ) : (
                              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                                <polyline points="7 10 12 15 17 10" />
                                <line x1="12" y1="15" x2="12" y2="3" />
                              </svg>
                            )}
                          </button>
                        )}
                      </div>
                    </div>
                  );
                })}
              </div>
            </div>
          );
        })}
      </div>
    </div>
  );
};

export default BackendManager;
