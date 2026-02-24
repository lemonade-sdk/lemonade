import React, { useState, useEffect, useCallback } from 'react';
import { BookOpen, Boxes, ChevronRight, Clock3, Cpu, ExternalLink, Settings as SettingsIcon, SlidersHorizontal, Store } from 'lucide-react';
import { ModelInfo } from './utils/modelData';
import { ToastContainer, useToast } from './Toast';
import { useConfirmDialog } from './ConfirmDialog';
import { serverFetch } from './utils/serverConfig';
import { downloadTracker } from './utils/downloadTracker';
import { ensureBackendForRecipe } from './utils/backendInstaller';
import { installBackend } from './utils/backendInstaller';
import { useModels } from './hooks/useModels';
import { useSystem } from './hooks/useSystem';
import ModelOptionsModal from "./ModelOptionsModal";
import { RecipeOptions, recipeOptionsToApi } from "./recipes/recipeOptions";
import { BackendInfo, Recipe } from './utils/systemData';
import SettingsPanel from './SettingsPanel';

interface ModelManagerProps {
  isVisible: boolean;
  width?: number;
  currentView: LeftPanelView;
  onViewChange: (view: LeftPanelView) => void;
}

export type LeftPanelView = 'models' | 'marketplace' | 'backends' | 'history' | 'settings';

interface MarketplaceApp {
  id: string;
  name: string;
  description?: string;
  category?: string[];
  logo?: string;
  pinned?: boolean;
  links?: {
    app?: string;
    guide?: string;
    video?: string;
  };
}

interface MarketplaceCategory {
  id: string;
  label: string;
}

const APPS_JSON_URL = 'https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps.json';
const RECIPE_ORDER = new Map(['llamacpp', 'whispercpp', 'sd-cpp', 'kokoro', 'flm', 'ryzenai-llm'].map((recipe, index) => [recipe, index]));

const RECIPE_DISPLAY_NAMES: Record<string, string> = {
  'flm': 'FastFlowLM NPU',
  'llamacpp': 'Llama.cpp GPU',
  'ryzenai-llm': 'Ryzen AI LLM',
  'whispercpp': 'Whisper.cpp',
  'sd-cpp': 'StableDiffusion.cpp',
  'kokoro': 'Kokoro',
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

// Registration data for new custom models
interface ModelRegistrationData {
  checkpoint: string;
  recipe: string;
  mmproj?: string;
  reasoning?: boolean;
  vision?: boolean;
  embedding?: boolean;
  reranking?: boolean;
}

const createEmptyModelForm = () => ({
  name: '',
  checkpoint: '',
  recipe: 'llamacpp',
  mmproj: '',
  reasoning: false,
  vision: false,
  embedding: false,
  reranking: false,
});

const ModelManager: React.FC<ModelManagerProps> = ({ isVisible, width = 280, currentView, onViewChange }) => {
  // Get shared model data from context
  const { modelsData, suggestedModels, refresh: refreshModels } = useModels();
  const { systemInfo, refresh: refreshSystem } = useSystem();

  const [expandedCategories, setExpandedCategories] = useState<Set<string>>(new Set(['all']));
  const [organizationMode, setOrganizationMode] = useState<'recipe' | 'category'>('recipe');
  const [showDownloadedOnly, setShowDownloadedOnly] = useState(false);
  const [showMarketplacePinnedOnly, setShowMarketplacePinnedOnly] = useState(false);
  const [showBackendAvailableOnly, setShowBackendAvailableOnly] = useState(false);
  const [showFilterPanel, setShowFilterPanel] = useState(false);
  const [showAddModelForm, setShowAddModelForm] = useState(false);
  const [searchQuery, setSearchQuery] = useState('');
  const [loadedModels, setLoadedModels] = useState<Set<string>>(new Set());
  const [loadingModels, setLoadingModels] = useState<Set<string>>(new Set());
  const [installingBackends, setInstallingBackends] = useState<Set<string>>(new Set());
  const [hoveredModel, setHoveredModel] = useState<string | null>(null);
  const [hoveredBackend, setHoveredBackend] = useState<string | null>(null);
  const [showModelOptionsModal, setShowModelOptionsModal] = useState(false);
  const [newModel, setNewModel] = useState(createEmptyModelForm);
  const [marketplaceApps, setMarketplaceApps] = useState<MarketplaceApp[]>([]);
  const [marketplaceCategories, setMarketplaceCategories] = useState<MarketplaceCategory[]>([]);
  const [marketplaceLoading, setMarketplaceLoading] = useState(true);
  const [marketplaceError, setMarketplaceError] = useState<string | null>(null);
  const [selectedMarketplaceCategory, setSelectedMarketplaceCategory] = useState<string>('all');
  const [backendAssetSizes, setBackendAssetSizes] = useState<Record<string, number>>({});

  const { toasts, removeToast, showError, showSuccess, showWarning } = useToast();
  const { confirm, ConfirmDialog } = useConfirmDialog();

  const fetchCurrentLoadedModel = useCallback(async () => {
    try {
      const response = await serverFetch('/health');
      const data = await response.json();

      if (data && data.all_models_loaded && Array.isArray(data.all_models_loaded)) {
        // Extract model names from the all_models_loaded array
        const loadedModelNames = new Set<string>(
          data.all_models_loaded.map((model: any) => model.model_name)
        );
        setLoadedModels(loadedModelNames);

        // Remove loaded models from loading state
        setLoadingModels(prev => {
          const newSet = new Set(prev);
          loadedModelNames.forEach(modelName => newSet.delete(modelName));
          return newSet;
        });
      } else {
        setLoadedModels(new Set());
      }
    } catch (error) {
      setLoadedModels(new Set());
      console.error('Failed to fetch current loaded model:', error);
    }
  }, []);

  useEffect(() => {
    fetchCurrentLoadedModel();

    // Poll for model status every 5 seconds to detect loaded models
    const interval = setInterval(() => {
      fetchCurrentLoadedModel();
    }, 5000);

    // === Integration API for other parts of the app ===
    // To indicate a model is loading, use either:
    // 1. window.setModelLoading(modelId, true/false)
    // 2. window.dispatchEvent(new CustomEvent('modelLoadStart', { detail: { modelId } }))
    // The health endpoint polling will automatically detect when loading completes

    // Expose the loading state updater globally for integration with other parts of the app
    (window as any).setModelLoading = (modelId: string, isLoading: boolean) => {
      setLoadingModels(prev => {
        const newSet = new Set(prev);
        if (isLoading) {
          newSet.add(modelId);
        } else {
          newSet.delete(modelId);
        }
        return newSet;
      });
    };

    // Listen for custom events that indicate model loading
    const handleModelLoadStart = (event: CustomEvent) => {
      const { modelId } = event.detail;
      if (modelId) {
        setLoadingModels(prev => new Set(prev).add(modelId));
      }
    };

    const handleModelLoadEnd = (event: CustomEvent) => {
      const { modelId } = event.detail;
      if (modelId) {
        setLoadingModels(prev => {
          const newSet = new Set(prev);
          newSet.delete(modelId);
          return newSet;
        });
        // Refresh the loaded model status
        fetchCurrentLoadedModel();
      }
    };

    window.addEventListener('modelLoadStart' as any, handleModelLoadStart);
    window.addEventListener('modelLoadEnd' as any, handleModelLoadEnd);

    return () => {
      clearInterval(interval);
      window.removeEventListener('modelLoadStart' as any, handleModelLoadStart);
      window.removeEventListener('modelLoadEnd' as any, handleModelLoadEnd);
      delete (window as any).setModelLoading;
    };
  }, [fetchCurrentLoadedModel]);

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

  const openExternalLink = useCallback((url?: string) => {
    if (!url) return;
    if (window.api?.openExternal) {
      window.api.openExternal(url);
      return;
    }
    window.open(url, '_blank', 'noopener,noreferrer');
  }, []);

  useEffect(() => {
    const recipes = systemInfo?.recipes;
    if (currentView !== 'backends' || !recipes) return;

    const pendingByRelease = new Map<string, Set<string>>();
    Object.values(recipes).forEach((recipe: Recipe) => {
      Object.values(recipe.backends).forEach((backend: BackendInfo) => {
        const releaseUrl = backend.release_url;
        const filename = backend.download_filename;
        if (!releaseUrl || !filename) return;
        if (typeof backend.download_size_mb === 'number' || typeof backend.download_size_bytes === 'number') return;

        const cacheKey = `${releaseUrl}:${filename}`;
        if (typeof backendAssetSizes[cacheKey] === 'number') return;
        if (!releaseUrl.includes('github.com/')) return;

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
            // Size fallback is best-effort and should not block rendering.
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
  }, [backendAssetSizes, currentView, systemInfo?.recipes]);

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

  // Auto-expand the single category if only one is available
  useEffect(() => {
    const groupedModels = organizationMode === 'recipe' ? groupModelsByRecipe() : groupModelsByCategory();
    const categories = Object.keys(groupedModels);

    // If only one category exists and it's not already expanded, expand it
    if (categories.length === 1 && !expandedCategories.has(categories[0])) {
      setExpandedCategories(new Set([categories[0]]));
    }
  }, [suggestedModels, organizationMode, showDownloadedOnly, searchQuery]);

  const getFilteredModels = () => {
    let filtered = suggestedModels;

    // Filter by downloaded status
    if (showDownloadedOnly) {
      filtered = filtered.filter(model => modelsData[model.name]?.downloaded);
    }

    // Filter by search query
    if (searchQuery.trim()) {
      const query = searchQuery.toLowerCase();
      filtered = filtered.filter(model =>
        model.name.toLowerCase().includes(query)
      );
    }

    return filtered;
  };

  const groupModelsByRecipe = () => {
    const grouped: { [key: string]: Array<{ name: string; info: ModelInfo }> } = {};
    const filteredModels = getFilteredModels();

    filteredModels.forEach(model => {
      const recipe = model.info.recipe || 'other';
      if (!grouped[recipe]) {
        grouped[recipe] = [];
      }
      grouped[recipe].push(model);
    });

    return grouped;
  };

  const groupModelsByCategory = () => {
    const grouped: { [key: string]: Array<{ name: string; info: ModelInfo }> } = {};
    const filteredModels = getFilteredModels();

    filteredModels.forEach(model => {
      if (model.info.labels && model.info.labels.length > 0) {
        model.info.labels.forEach(label => {
          if (!grouped[label]) {
            grouped[label] = [];
          }
          grouped[label].push(model);
        });
      } else {
        // Models without labels go to 'uncategorized'
        if (!grouped['uncategorized']) {
          grouped['uncategorized'] = [];
        }
        grouped['uncategorized'].push(model);
      }
    });

    return grouped;
  };

  const toggleCategory = (category: string) => {
    setExpandedCategories(prev => {
      const newSet = new Set(prev);
      if (newSet.has(category)) {
        newSet.delete(category);
      } else {
        newSet.add(category);
      }
      return newSet;
    });
  };

  const formatSize = (size?: number): string => {
    if (typeof size !== 'number' || Number.isNaN(size)) {
      return 'Size N/A';
    }

    if (size < 1) {
      return `${(size * 1024).toFixed(0)} MB`;
    }
    return `${size.toFixed(2)} GB`;
  };

  const getRecipeLabel = (recipe: string): string => {
    const labels: { [key: string]: string } = {
      'ryzenai-llm': 'RyzenAI',
      'llamacpp': 'GGUF',
      'flm': 'FLM',
      'whispercpp': 'Whisper.cpp'
    };
    return labels[recipe] || recipe.toUpperCase();
  };

  const getCategoryLabel = (category: string): string => {
    const labels: { [key: string]: string } = {
      'reasoning': 'Reasoning',
      'coding': 'Coding',
      'vision': 'Vision',
      'hot': 'Hot',
      'embeddings': 'Embeddings',
      'reranking': 'Reranking',
      'tool-calling': 'Tool Calling',
      'custom': 'Custom',
      'uncategorized': 'Uncategorized'
    };
    return labels[category] || category.charAt(0).toUpperCase() + category.slice(1);
  };

  if (!isVisible) return null;

  const groupedModels = organizationMode === 'recipe' ? groupModelsByRecipe() : groupModelsByCategory();
  const availableModelCount = getFilteredModels().length;
  const categories = Object.keys(groupedModels).sort();

  // Auto-expand all categories when searching
  const shouldShowCategory = (category: string): boolean => {
    if (searchQuery.trim()) {
      return true; // Show all categories when searching
    }
    return expandedCategories.has(category);
  };

  const getDisplayLabel = (key: string): string => {
    if (organizationMode === 'recipe') {
      // Use friendly names for recipes
      const recipeLabels: { [key: string]: string } = {
        'flm': 'FastFlowLM NPU',
        'llamacpp': 'Llama.cpp GPU',
        'ryzenai-llm': 'Ryzen AI LLM',
        'whispercpp': 'Whisper.cpp',
        'sd-cpp': 'StableDiffusion.cpp',
        'kokoro': 'Kokoro'
      };
      return recipeLabels[key] || key;
    } else {
      // Use friendly labels for categories
      return getCategoryLabel(key);
    }
  };

  const loadedModelEntries = Array.from(loadedModels)
    .map(modelName => ({
      modelName,
      recipe: modelsData[modelName]?.recipe || 'other',
    }))
    .sort((a, b) => a.modelName.localeCompare(b.modelName));

  const resetNewModelForm = () => {
    setNewModel(createEmptyModelForm());
    setShowAddModelForm(false);
  };

  const handleInstallModel = () => {
    const trimmedName = newModel.name.trim();
    const trimmedCheckpoint = newModel.checkpoint.trim();
    const trimmedRecipe = newModel.recipe.trim();
    const trimmedMmproj = newModel.mmproj.trim();

    if (!trimmedName) {
      showWarning('Model name is required.');
      return;
    }

    if (!trimmedCheckpoint) {
      showWarning('Checkpoint is required.');
      return;
    }

    if (!trimmedRecipe) {
      showWarning('Recipe is required.');
      return;
    }

    // Validate GGUF checkpoint format
    if (trimmedCheckpoint.toLowerCase().includes('gguf') && !trimmedCheckpoint.includes(':')) {
      showWarning('GGUF checkpoints must include a variant using the CHECKPOINT:VARIANT syntax');
      return;
    }

    // Close the form and start the download
    const modelName = `user.${trimmedName}`;
    resetNewModelForm();

    // Use the same download flow as registered models, but include registration data
    handleDownloadModel(modelName, {
      checkpoint: trimmedCheckpoint,
      recipe: trimmedRecipe,
      mmproj: trimmedMmproj || undefined,
      reasoning: newModel.reasoning,
      vision: newModel.vision,
      embedding: newModel.embedding,
      reranking: newModel.reranking,
    });
  };

  const handleInputChange = (field: string, value: string | boolean) => {
    setNewModel(prev => ({
      ...prev,
      [field]: value
    }));
  };

  const handleDownloadModel = useCallback(async (modelName: string, registrationData?: ModelRegistrationData) => {
    try {
      // For registered models, verify metadata exists; for new models, we're registering now
      if (!registrationData && !modelsData[modelName]) {
        showError('Model metadata is unavailable. Please refresh and try again.');
        return;
      }

      // Add to loading state to show loading indicator
      setLoadingModels(prev => new Set(prev).add(modelName));

      // Create abort controller for this download
      const abortController = new AbortController();
      const downloadId = downloadTracker.startDownload(modelName, abortController);

      // Dispatch event to open download manager
      window.dispatchEvent(new CustomEvent('download:started', { detail: { modelName } }));

      let downloadCompleted = false;
      let isPaused = false;
      let isCancelled = false;

      // Listen for cancel and pause events
      const handleCancel = (event: CustomEvent) => {
        if (event.detail.modelName === modelName) {
          isCancelled = true;
          abortController.abort();
        }
      };
      const handlePause = (event: CustomEvent) => {
        if (event.detail.modelName === modelName) {
          isPaused = true;
          abortController.abort();
        }
      };
      window.addEventListener('download:cancelled' as any, handleCancel);
      window.addEventListener('download:paused' as any, handlePause);

      try {
        // Build request body - include registration data for new custom models
        const requestBody: Record<string, unknown> = { model_name: modelName, stream: true };
        if (registrationData) {
          requestBody.checkpoint = registrationData.checkpoint;
          requestBody.recipe = registrationData.recipe;
          if (registrationData.mmproj) requestBody.mmproj = registrationData.mmproj;
          if (registrationData.reasoning) requestBody.reasoning = registrationData.reasoning;
          if (registrationData.vision) requestBody.vision = registrationData.vision;
          if (registrationData.embedding) requestBody.embedding = registrationData.embedding;
          if (registrationData.reranking) requestBody.reranking = registrationData.reranking;
        }

        const response = await serverFetch('/pull', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(requestBody),
          signal: abortController.signal,
        });

        if (!response.ok) {
          throw new Error(`Failed to download model: ${response.statusText}`);
        }

        // Read SSE stream for progress updates
        const reader = response.body?.getReader();
        if (!reader) {
          throw new Error('No response body');
        }

        const decoder = new TextDecoder();
        let buffer = '';
        let currentEventType = 'progress';

        try {
          while (true) {
            const { done, value } = await reader.read();

            if (done) break;

            buffer += decoder.decode(value, { stream: true });
            const lines = buffer.split('\n');
            buffer = lines.pop() || '';

            for (const line of lines) {
              if (line.startsWith('event:')) {
                currentEventType = line.substring(6).trim();
              } else if (line.startsWith('data:')) {
                // Parse JSON separately so server errors aren't swallowed
                let data;
                try {
                  data = JSON.parse(line.substring(5).trim());
                } catch (parseError) {
                  console.error('Failed to parse SSE data:', line, parseError);
                  continue;
                }

                if (currentEventType === 'progress') {
                  downloadTracker.updateProgress(downloadId, data);
                } else if (currentEventType === 'complete') {
                  downloadTracker.completeDownload(downloadId);
                  downloadCompleted = true;
                } else if (currentEventType === 'error') {
                  downloadTracker.failDownload(downloadId, data.error || 'Unknown error');
                  throw new Error(data.error || 'Download failed');
                }
              } else if (line.trim() === '') {
                currentEventType = 'progress';
              }
            }
          }
        } catch (streamError: any) {
          // If we already got the complete event, ignore stream errors
          if (!downloadCompleted) {
            throw streamError;
          }
        }

        // Mark as complete if not already done
        if (!downloadCompleted) {
          downloadTracker.completeDownload(downloadId);
          downloadCompleted = true;
        }

        // Notify all components that models have been updated
        window.dispatchEvent(new CustomEvent('modelsUpdated'));
        await fetchCurrentLoadedModel();

        // Show success notification
        showSuccess(`Model "${modelName}" downloaded successfully.`);
      } catch (error: any) {
        // Only handle as error if download didn't complete successfully
        if (downloadCompleted) {
          // Download actually succeeded, ignore any network errors from connection closing
          return;
        }

        if (error.name === 'AbortError') {
          if (isPaused) {
            downloadTracker.pauseDownload(downloadId);
            showWarning(`Download paused: ${modelName}`);
          } else if (isCancelled) {
            downloadTracker.cancelDownload(downloadId);
            showWarning(`Download cancelled: ${modelName}`);
            // Dispatch cleanup-complete event to signal that file handles are released
            window.dispatchEvent(new CustomEvent('download:cleanup-complete', {
              detail: { id: downloadId, modelName }
            }));
          } else {
            downloadTracker.cancelDownload(downloadId);
            showWarning(`Download cancelled: ${modelName}`);
            // Dispatch cleanup-complete event to signal that file handles are released
            window.dispatchEvent(new CustomEvent('download:cleanup-complete', {
              detail: { id: downloadId, modelName }
            }));
          }
        } else {
          downloadTracker.failDownload(downloadId, error.message || 'Unknown error');
          throw error;
        }
      } finally {
        window.removeEventListener('download:cancelled' as any, handleCancel);
        window.removeEventListener('download:paused' as any, handlePause);
      }
    } catch (error) {
      console.error('Error downloading model:', error);
      showError(`Failed to download model: ${error instanceof Error ? error.message : 'Unknown error'}`);
    } finally {
      // Remove from loading state
      setLoadingModels(prev => {
        const newSet = new Set(prev);
        newSet.delete(modelName);
        return newSet;
      });
    }
  }, [modelsData, showError, showSuccess, showWarning, fetchCurrentLoadedModel]);

  // Separate useEffect for download resume/retry to avoid stale closure issues
  useEffect(() => {
    const handleDownloadResume = (event: CustomEvent) => {
      const { modelName } = event.detail;
      if (modelName) {
        handleDownloadModel(modelName);
      }
    };

    const handleDownloadRetry = (event: CustomEvent) => {
      const { modelName } = event.detail;
      if (modelName) {
        handleDownloadModel(modelName);
      }
    };

    window.addEventListener('download:resume' as any, handleDownloadResume);
    window.addEventListener('download:retry' as any, handleDownloadRetry);

    return () => {
      window.removeEventListener('download:resume' as any, handleDownloadResume);
      window.removeEventListener('download:retry' as any, handleDownloadRetry);
    };
  }, [handleDownloadModel]);

  const handleLoadModel = async (modelName: string, options?: RecipeOptions, autoLoadAfterDownload: boolean = false) => {
    try {
      let modelData = modelsData[modelName];
      if (!modelData) {
        showError('Model metadata is unavailable. Please refresh and try again.');
        return;
      }

      // if options are provided, convert them to API format
      if (options) {
        const apiOptions = recipeOptionsToApi(options);
        modelData = { ...modelData, ...apiOptions };
      }

      // Add to loading state
      setLoadingModels(prev => new Set(prev).add(modelName));

      // Dispatch event to notify other components
      window.dispatchEvent(new CustomEvent('modelLoadStart', { detail: { modelId: modelName } }));

      // Ensure the backend is installed before loading (shows in Download Manager)
      if (modelData.recipe) {
        await ensureBackendForRecipe(modelData.recipe, systemInfo?.recipes);
        // Refresh system info so backend status is up to date
        await refreshSystem();
      }

      const response = await serverFetch('/load', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ model_name: modelName, ...modelData })
      });

      if (!response.ok) {
        // Try to parse error response to check for model_invalidated
        try {
          const errorData = await response.json();
          if (errorData?.error?.code === 'model_invalidated') {
            console.log('[ModelManager] Model was invalidated, triggering re-download:', modelName);

            // Remove from loading state before starting download
            setLoadingModels(prev => {
              const newSet = new Set(prev);
              newSet.delete(modelName);
              return newSet;
            });
            window.dispatchEvent(new CustomEvent('modelLoadEnd', { detail: { modelId: modelName } }));

            // Show info message
            showWarning(`Model "${modelName}" needs to be re-downloaded due to a backend upgrade. Starting download...`);

            // Start download, then auto-load when complete
            await handleDownloadModel(modelName);

            // After download completes, load the model
            console.log('[ModelManager] Re-download complete, loading model:', modelName);
            await handleLoadModel(modelName, undefined, true);
            return;
          }
        } catch (parseError) {
          // Couldn't parse error response, fall through to generic error
        }
        throw new Error(`Failed to load model: ${response.statusText}`);
      }

      // Wait a bit for the model to actually load, then refresh status
      setTimeout(async () => {
        await fetchCurrentLoadedModel();
        window.dispatchEvent(new CustomEvent('modelLoadEnd', { detail: { modelId: modelName } }));

        // Refresh the models list in case FLM upgrade invalidated other models
        window.dispatchEvent(new CustomEvent('modelsUpdated'));
      }, 1000);
    } catch (error) {
      console.error('Error loading model:', error);
      showError(`Failed to load model: ${error instanceof Error ? error.message : 'Unknown error'}`);

      // Remove from loading state on error
      setLoadingModels(prev => {
        const newSet = new Set(prev);
        newSet.delete(modelName);
        return newSet;
      });

      window.dispatchEvent(new CustomEvent('modelLoadEnd', { detail: { modelId: modelName } }));
    }
  };

  const handleUnloadModel = async (modelName: string) => {
    try {
      const response = await serverFetch('/unload', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ model_name: modelName })
      });

      if (!response.ok) {
        throw new Error(`Failed to unload model: ${response.statusText}`);
      }

      // Refresh current loaded model status
      await fetchCurrentLoadedModel();

      // Dispatch event to notify other components (e.g., ChatWindow) that model was unloaded
      window.dispatchEvent(new CustomEvent('modelUnload'));
    } catch (error) {
      console.error('Error unloading model:', error);
      showError(`Failed to unload model: ${error instanceof Error ? error.message : 'Unknown error'}`);
    }
  };

  const handleDeleteModel = async (modelName: string) => {
    const confirmed = await confirm({
      title: 'Delete Model',
      message: `Are you sure you want to delete the model "${modelName}"? This action cannot be undone.`,
      confirmText: 'Delete',
      cancelText: 'Cancel',
      danger: true
    });

    if (!confirmed) {
      return;
    }

    try {
      const response = await serverFetch('/delete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ model_name: modelName })
      });

      if (!response.ok) {
        throw new Error(`Failed to delete model: ${response.statusText}`);
      }

      // Notify all components that models have been updated
      window.dispatchEvent(new CustomEvent('modelsUpdated'));
      await fetchCurrentLoadedModel();
      showSuccess(`Model "${modelName}" deleted successfully.`);

      // Notify other components (e.g., ChatWindow) that models have been updated
      window.dispatchEvent(new CustomEvent('modelsUpdated'));
    } catch (error) {
      console.error('Error deleting model:', error);
      showError(`Failed to delete model: ${error instanceof Error ? error.message : 'Unknown error'}`);
    }
  };

  const handleInstallBackend = useCallback(async (recipe: string, backend: string) => {
    const key = `${recipe}:${backend}`;
    setInstallingBackends(prev => new Set(prev).add(key));
    try {
      await installBackend(recipe, backend, true);
      showSuccess(`${RECIPE_DISPLAY_NAMES[recipe] || recipe} ${backend} installed successfully.`);
      await refreshSystem();
    } catch (error) {
      showError(`Failed to install backend: ${error instanceof Error ? error.message : 'Unknown error'}`);
    } finally {
      setInstallingBackends(prev => {
        const next = new Set(prev);
        next.delete(key);
        return next;
      });
    }
  }, [refreshSystem, showError, showSuccess]);

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
      const response = await serverFetch('/uninstall', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ recipe, backend })
      });
      if (!response.ok) {
        const errorText = await response.text();
        throw new Error(errorText || response.statusText);
      }
      showSuccess(`${RECIPE_DISPLAY_NAMES[recipe] || recipe} ${backend} uninstalled successfully.`);
      await refreshSystem();
    } catch (error) {
      showError(`Failed to uninstall backend: ${error instanceof Error ? error.message : 'Unknown error'}`);
    }
  }, [confirm, refreshSystem, showError, showSuccess]);

  const recipes = systemInfo?.recipes;

  const filteredMarketplaceApps = marketplaceApps
    .filter((app) => {
      if (showMarketplacePinnedOnly && !app.pinned) return false;
      if (selectedMarketplaceCategory !== 'all') {
        return Array.isArray(app.category) && app.category.includes(selectedMarketplaceCategory);
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

  const groupedBackends: Array<[string, Array<[string, BackendInfo]>]> = recipes
    ? Object.entries(recipes)
      .map(([recipeName, recipe]: [string, Recipe]) => {
        const backends = Object.entries(recipe.backends).filter(([, info]) => info.supported);
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

  const viewTitle = currentView === 'models'
    ? 'Model Manager'
    : currentView === 'marketplace'
      ? 'Marketplace'
      : currentView === 'backends'
        ? 'Backend Manager'
        : currentView === 'history'
          ? 'Chat History'
          : 'Settings';

  const searchPlaceholder = currentView === 'models'
    ? 'Search models...'
    : currentView === 'marketplace'
      ? 'Search apps...'
      : currentView === 'backends'
        ? 'Search backends...'
        : currentView === 'history'
          ? 'Search history...'
          : 'Search settings...';
  const showInlineFilterButton = currentView !== 'history' && currentView !== 'settings';

  const renderModelsView = () => (
    <>
      {categories.map(category => (
        <div key={category} className="model-category">
          <div
            className="model-category-header"
            onClick={() => toggleCategory(category)}
          >
            <span className={`category-chevron ${shouldShowCategory(category) ? 'expanded' : ''}`}>
              <ChevronRight size={11} strokeWidth={2.1} />
            </span>
            <span className="category-label">{getDisplayLabel(category)}</span>
            <span className="category-count">({groupedModels[category].length})</span>
          </div>

          {shouldShowCategory(category) && (
            <div className="model-list">
              <ModelOptionsModal model={hoveredModel} isOpen={showModelOptionsModal}
                                 onCancel={() => setShowModelOptionsModal(false)}
                                 onSubmit={(modelName, options) => {
                                   setShowModelOptionsModal(false);
                                   handleLoadModel(modelName, options);
                                 }}/>
              {groupedModels[category].map(model => {
                const isDownloaded = modelsData[model.name]?.downloaded ?? false;
                const isLoaded = loadedModels.has(model.name);
                const isLoading = loadingModels.has(model.name);

                let statusClass = 'not-downloaded';
                let statusTitle = 'Not downloaded';

                if (isLoading) {
                  statusClass = 'loading';
                  statusTitle = 'Loading...';
                } else if (isLoaded) {
                  statusClass = 'loaded';
                  statusTitle = 'Model is loaded';
                } else if (isDownloaded) {
                  statusClass = 'available';
                  statusTitle = 'Available locally';
                }

                const isHovered = hoveredModel === model.name;

                return (
                  <div
                    key={model.name}
                    className={`model-item model-catalog-item ${isDownloaded ? 'downloaded' : ''}`}
                    onMouseEnter={() => setHoveredModel(model.name)}
                    onMouseLeave={() => setHoveredModel(null)}
                  >
                    <div className="model-item-content">
                      <div className="model-info-left">
                        <span
                          className={`model-status-indicator ${statusClass}`}
                          title={statusTitle}
                        >
                          ●
                        </span>
                        <span className="model-name">{model.name}</span>
                        <span className="model-size">{formatSize(model.info.size)}</span>
                        {isHovered && (
                          <span className="model-actions">
                            {!isDownloaded && (
                              <button
                                className="model-action-btn download-btn"
                                onClick={(e) => {
                                  e.stopPropagation();
                                  handleDownloadModel(model.name);
                                }}
                                title="Download model"
                              >
                                <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                  <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                                  <polyline points="7 10 12 15 17 10" />
                                  <line x1="12" y1="15" x2="12" y2="3" />
                                </svg>
                              </button>
                            )}
                            {isDownloaded && !isLoaded && !isLoading && (
                              <>
                                <button
                                  className="model-action-btn load-btn"
                                  onClick={(e) => {
                                    e.stopPropagation();
                                    handleLoadModel(model.name);
                                  }}
                                  title="Load model"
                                >
                                  <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                    <polygon points="5 3 19 12 5 21" fill="currentColor" />
                                  </svg>
                                </button>
                                <button
                                  className="model-action-btn delete-btn"
                                  onClick={(e) => {
                                    e.stopPropagation();
                                    handleDeleteModel(model.name);
                                  }}
                                  title="Delete model"
                                >
                                  <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                    <polyline points="3 6 5 6 21 6" />
                                    <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
                                  </svg>
                                </button>
                                <button
                                  className="model-action-btn load-btn"
                                  onClick={(e) => {
                                    e.stopPropagation();
                                    setShowModelOptionsModal(!showModelOptionsModal);
                                  }}
                                  title="Load model with options"
                                >
                                  <svg width="12" height="12" viewBox="0 0 16 16" fill="none"
                                       xmlns="http://www.w3.org/2000/svg">
                                    <path
                                      d="M6.5 1.5H9.5L9.9 3.4C10.4 3.6 10.9 3.9 11.3 4.2L13.1 3.5L14.6 6L13.1 7.4C13.2 7.9 13.2 8.1 13.2 8.5C13.2 8.9 13.2 9.1 13.1 9.6L14.6 11L13.1 13.5L11.3 12.8C10.9 13.1 10.4 13.4 9.9 13.6L9.5 15.5H6.5L6.1 13.6C5.6 13.4 5.1 13.1 4.7 12.8L2.9 13.5L1.4 11L2.9 9.6C2.8 9.1 2.8 8.9 2.8 8.5C2.8 8.1 2.8 7.9 2.9 7.4L1.4 6L2.9 3.5L4.7 4.2C5.1 3.9 5.6 3.6 6.1 3.4L6.5 1.5Z"
                                      stroke="currentColor" strokeWidth="1.2" strokeLinecap="round"
                                      strokeLinejoin="round"/>
                                    <circle cx="8" cy="8.5" r="2.5" stroke="currentColor"
                                            strokeWidth="1.2"/>
                                  </svg>
                                </button>
                              </>
                            )}
                            {isLoaded && (
                              <>
                                <button
                                  className="model-action-btn unload-btn"
                                  onClick={(e) => {
                                    e.stopPropagation();
                                    handleUnloadModel(model.name);
                                  }}
                                  title="Eject model"
                                >
                                  <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                    <path d="M9 11L12 8L15 11" />
                                    <path d="M12 8V16" />
                                    <path d="M5 20H19" />
                                  </svg>
                                </button>
                                <button
                                  className="model-action-btn delete-btn"
                                  onClick={(e) => {
                                    e.stopPropagation();
                                    handleDeleteModel(model.name);
                                  }}
                                  title="Delete model"
                                >
                                  <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                    <polyline points="3 6 5 6 21 6" />
                                    <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
                                  </svg>
                                </button>
                              </>
                            )}
                          </span>
                        )}
                      </div>
                      {model.info.labels && model.info.labels.length > 0 && (
                        <span className="model-labels">
                          {model.info.labels.map(label => (
                            <span
                              key={label}
                              className={`model-label label-${label}`}
                              title={getCategoryLabel(label)}
                            />
                          ))}
                        </span>
                      )}
                    </div>
                  </div>
                );
              })}
            </div>
          )}
        </div>
      ))}
    </>
  );

  const renderMarketplaceView = () => {
    if (marketplaceLoading) {
      return <div className="left-panel-empty-state">Loading marketplace apps...</div>;
    }
    if (marketplaceError) {
      return <div className="left-panel-empty-state">Marketplace unavailable: {marketplaceError}</div>;
    }
    if (filteredMarketplaceApps.length === 0) {
      return <div className="left-panel-empty-state">No apps match your current filters.</div>;
    }

    return (
      <div className="left-panel-row-list">
        {filteredMarketplaceApps.map((app) => (
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

  const renderBackendsView = () => {
    const query = searchQuery.trim().toLowerCase();
    const visibleGroups = groupedBackends
      .map(([recipeName, backends]) => {
        const filteredBackends = backends.filter(([backendName, info]) => {
          if (showBackendAvailableOnly && !info.available) return false;
          if (!query) return true;
          const haystack = `${recipeName} ${backendName} ${info.version || ''}`.toLowerCase();
          return haystack.includes(query);
        });
        return [recipeName, filteredBackends] as [string, Array<[string, BackendInfo]>];
      })
      .filter(([, backends]) => backends.length > 0);

    if (visibleGroups.length === 0) {
      return <div className="left-panel-empty-state">No backends match your current filters.</div>;
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
                        <span className="model-name backend-name">
                          <span className={`model-status-indicator ${info.available ? 'available' : 'not-downloaded'}`}>●</span>
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
                            <span className="backend-version">Not installed</span>
                          )}
                          {sizeLabel && (
                            <>
                              <span className="backend-meta-separator">•</span>
                              <span className="backend-size">{sizeLabel}</span>
                            </>
                          )}
                        </div>
                      </div>
                      {isHovered && (
                        <span className="model-actions">
                          {info.available ? (
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
                          ) : (
                            <button
                              className="model-action-btn download-btn"
                              title="Install backend"
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

  return (
    <div className="model-manager" style={{ width: `${width}px` }}>
      <ToastContainer toasts={toasts} onRemove={removeToast} />
      <ConfirmDialog />
      <div className="left-panel-shell">
        <div className="left-panel-mode-rail">
          <button className={`left-panel-mode-btn ${currentView === 'models' ? 'active' : ''}`} onClick={() => onViewChange('models')} title="Models" aria-label="Models">
            <Boxes size={14} strokeWidth={1.9} />
          </button>
          <button className={`left-panel-mode-btn ${currentView === 'marketplace' ? 'active' : ''}`} onClick={() => onViewChange('marketplace')} title="Marketplace" aria-label="Marketplace">
            <Store size={14} strokeWidth={1.9} />
          </button>
          <button className={`left-panel-mode-btn ${currentView === 'backends' ? 'active' : ''}`} onClick={() => onViewChange('backends')} title="Backends" aria-label="Backends">
            <Cpu size={14} strokeWidth={1.9} />
          </button>
          <button className={`left-panel-mode-btn ${currentView === 'history' ? 'active' : ''}`} onClick={() => onViewChange('history')} title="History (coming soon)" aria-label="History (coming soon)">
            <Clock3 size={14} strokeWidth={1.9} />
          </button>
          <div className="left-panel-mode-rail-spacer" />
          <button className={`left-panel-mode-btn ${currentView === 'settings' ? 'active' : ''}`} onClick={() => onViewChange('settings')} title="Settings" aria-label="Settings">
            <SettingsIcon size={14} strokeWidth={1.9} />
          </button>
        </div>

        <div className="left-panel-main">
          <div className="model-manager-header">
            <div className="left-panel-header-top">
              <h3>{viewTitle}</h3>
            </div>
            <div className={`model-search ${showInlineFilterButton ? 'with-inline-filter' : ''}`}>
              <input
                type="text"
                className="model-search-input"
                placeholder={searchPlaceholder}
                value={searchQuery}
                onChange={(e) => setSearchQuery(e.target.value)}
              />
              {showInlineFilterButton && (
                <button
                  className={`left-panel-inline-filter-btn ${showFilterPanel ? 'active' : ''}`}
                  onClick={() => setShowFilterPanel(prev => !prev)}
                  title="Filters"
                  aria-label="Filters"
                >
                  <SlidersHorizontal size={13} strokeWidth={2} />
                </button>
              )}
            </div>
          </div>

          {showFilterPanel && showInlineFilterButton && (
            <div className="left-panel-filter-drawer">
              {currentView === 'models' && (
                <>
                  <div className="organization-toggle">
                    <button className={`toggle-button ${organizationMode === 'recipe' ? 'active' : ''}`} onClick={() => setOrganizationMode('recipe')}>
                      By Recipe
                    </button>
                    <button className={`toggle-button ${organizationMode === 'category' ? 'active' : ''}`} onClick={() => setOrganizationMode('category')}>
                      By Category
                    </button>
                  </div>
                  <label className="toggle-switch-label">
                    <span className="toggle-label-text">Downloaded only</span>
                    <div className="toggle-switch">
                      <input type="checkbox" checked={showDownloadedOnly} onChange={(e) => setShowDownloadedOnly(e.target.checked)} />
                      <span className="toggle-slider"></span>
                    </div>
                  </label>
                </>
              )}
              {currentView === 'marketplace' && (
                <>
                  <div className="left-panel-filter-row">
                    <span className="toggle-label-text">Category</span>
                    <select className="left-panel-filter-select" value={selectedMarketplaceCategory} onChange={(e) => setSelectedMarketplaceCategory(e.target.value)}>
                      <option value="all">All</option>
                      {marketplaceCategories.map((category) => (
                        <option key={category.id} value={category.id}>{category.label}</option>
                      ))}
                    </select>
                  </div>
                  <label className="toggle-switch-label">
                    <span className="toggle-label-text">Featured only</span>
                    <div className="toggle-switch">
                      <input type="checkbox" checked={showMarketplacePinnedOnly} onChange={(e) => setShowMarketplacePinnedOnly(e.target.checked)} />
                      <span className="toggle-slider"></span>
                    </div>
                  </label>
                </>
              )}
              {currentView === 'backends' && (
                <label className="toggle-switch-label">
                  <span className="toggle-label-text">Installed only</span>
                  <div className="toggle-switch">
                    <input type="checkbox" checked={showBackendAvailableOnly} onChange={(e) => setShowBackendAvailableOnly(e.target.checked)} />
                    <span className="toggle-slider"></span>
                  </div>
                </label>
              )}
            </div>
          )}

          {currentView === 'models' && (
            <div className="loaded-model-section widget">
              <div className="loaded-model-header">
                <div className="loaded-model-label">ACTIVE MODELS</div>
                <div className="loaded-model-count-pill">{loadedModelEntries.length} loaded</div>
              </div>
              {loadedModelEntries.length === 0 && <div className="loaded-model-empty">No models loaded</div>}
              <div className="loaded-model-list">
                {loadedModelEntries.map(({ modelName, recipe }) => (
                  <div key={modelName} className="loaded-model-info">
                    <div className="loaded-model-details">
                      <span className="loaded-model-indicator">●</span>
                      <div className="loaded-model-name-stack">
                        <span className="loaded-model-name">{modelName}</span>
                        <span className="loaded-model-meta">{getRecipeLabel(recipe)}</span>
                      </div>
                    </div>
                    <button className="eject-model-button" onClick={() => handleUnloadModel(modelName)} title="Eject model">
                      <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                        <path d="M9 11L12 8L15 11" />
                        <path d="M12 8V16" />
                        <path d="M5 20H19" />
                      </svg>
                    </button>
                  </div>
                ))}
              </div>
            </div>
          )}

          <div className="model-manager-content">
            {currentView === 'models' && (
              <div className="available-models-section widget">
                <div className="available-models-header">
                  <div className="loaded-model-label">AVAILABLE MODELS</div>
                  <div className="loaded-model-count-pill">{availableModelCount} shown</div>
                </div>
                {renderModelsView()}
              </div>
            )}
            {currentView === 'marketplace' && renderMarketplaceView()}
            {currentView === 'backends' && renderBackendsView()}
            {currentView === 'history' && (
              <div className="left-panel-empty-state">Chat history will be available in a future update.</div>
            )}
            {currentView === 'settings' && <SettingsPanel isVisible={true} searchQuery={searchQuery} />}
          </div>

          {currentView === 'models' && (
            <div className="model-manager-footer">
              {!showAddModelForm ? (
                <button
                  className="add-model-button"
                  onClick={() => {
                    setNewModel(createEmptyModelForm());
                    setShowAddModelForm(true);
                  }}
                >
                  Add a model
                </button>
              ) : (
                <div className="add-model-form">
                  <div className="form-section">
                    <label className="form-label" title="A unique name to identify your model in the catalog">Model Name</label>
                    <div className="input-with-prefix">
                      <span className="input-prefix">user.</span>
                      <input
                        type="text"
                        className="form-input with-prefix"
                        placeholder="Gemma-3-12b-it-GGUF"
                        value={newModel.name}
                        onChange={(e) => handleInputChange('name', e.target.value)}
                      />
                    </div>
                  </div>

                  <div className="form-section">
                    <label className="form-label" title="Hugging Face model path (repo/model:quantization)">Checkpoint</label>
                    <input
                      type="text"
                      className="form-input"
                      placeholder="unsloth/gemma-3-12b-it-GGUF:Q4_0"
                      value={newModel.checkpoint}
                      onChange={(e) => handleInputChange('checkpoint', e.target.value)}
                    />
                  </div>

                  <div className="form-section">
                    <label className="form-label" title="Inference backend to use for this model">Recipe</label>
                    <select
                      className="form-input form-select"
                      value={newModel.recipe}
                      onChange={(e) => handleInputChange('recipe', e.target.value)}
                    >
                      <option value="">Select a recipe...</option>
                      <option value="llamacpp">Llama.cpp GPU</option>
                      <option value="flm">FastFlowLM NPU</option>
                      <option value="ryzenai-llm">Ryzen AI LLM</option>
                    </select>
                  </div>

                  <div className="form-section">
                    <label className="form-label">More info</label>
                    <div className="form-subsection">
                      <label className="form-label-secondary" title="Multimodal projection file for vision models">mmproj file (Optional)</label>
                      <input
                        type="text"
                        className="form-input"
                        placeholder="mmproj-F16.gguf"
                        value={newModel.mmproj}
                        onChange={(e) => handleInputChange('mmproj', e.target.value)}
                      />
                    </div>

                    <div className="form-checkboxes">
                      <label className="checkbox-label" title="Enable if model supports chain-of-thought reasoning">
                        <input
                          type="checkbox"
                          checked={newModel.reasoning}
                          onChange={(e) => handleInputChange('reasoning', e.target.checked)}
                        />
                        <span>Reasoning</span>
                      </label>

                      <label className="checkbox-label" title="Enable if model can process images">
                        <input
                          type="checkbox"
                          checked={newModel.vision}
                          onChange={(e) => handleInputChange('vision', e.target.checked)}
                        />
                        <span>Vision</span>
                      </label>

                      <label className="checkbox-label" title="Enable if model generates text embeddings">
                        <input
                          type="checkbox"
                          checked={newModel.embedding}
                          onChange={(e) => handleInputChange('embedding', e.target.checked)}
                        />
                        <span>Embedding</span>
                      </label>

                      <label className="checkbox-label" title="Enable if model performs reranking">
                        <input
                          type="checkbox"
                          checked={newModel.reranking}
                          onChange={(e) => handleInputChange('reranking', e.target.checked)}
                        />
                        <span>Reranking</span>
                      </label>
                    </div>
                  </div>

                  <div className="form-actions">
                    <button className="install-button" onClick={handleInstallModel}>
                      Install
                    </button>
                    <button className="cancel-button" onClick={resetNewModelForm}>
                      Cancel
                    </button>
                  </div>
                </div>
              )}
            </div>
          )}
        </div>
      </div>
    </div>
  );
};

export default ModelManager;
