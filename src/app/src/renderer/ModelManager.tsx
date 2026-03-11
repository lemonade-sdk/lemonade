import React, { useState, useEffect, useCallback, useRef, useMemo } from 'react';
import { Boxes, ChevronRight, Cpu, Settings, SlidersHorizontal, Store } from './components/Icons';
import { ModelInfo } from './utils/modelData';
import { ToastContainer, useToast } from './Toast';
import { useConfirmDialog } from './ConfirmDialog';
import { serverFetch } from './utils/serverConfig';
import { pullModel, DownloadAbortError, ensureModelReady, deleteModel, ensureBackendForRecipe, installBackend } from './utils/backendInstaller';
import { fetchSystemInfoData, BackendInfo } from './utils/systemData';
import type { ModelRegistrationData } from './utils/backendInstaller';
import { downloadTracker } from './utils/downloadTracker';
import { useModels } from './hooks/useModels';
import { useSystem } from './hooks/useSystem';
import ModelOptionsModal from "./ModelOptionsModal";
import { RecipeOptions, recipeOptionsToApi } from "./recipes/recipeOptions";
import SettingsPanel from './SettingsPanel';
import BackendManager from './BackendManager';
import ConnectedBackendRow from './components/ConnectedBackendRow';
import MarketplacePanel, { MarketplaceCategory } from './MarketplacePanel';
import { RECIPE_DISPLAY_NAMES } from './utils/recipeNames';
import { EjectIcon } from './components/Icons';
import { getExperienceComponents, isExperienceFullyDownloaded, isExperienceFullyLoaded, isExperienceModel, isModelEffectivelyDownloaded } from './utils/experienceModels';

interface ModelFamily {
  displayName: string;
  regex: RegExp;
}

const SIZE_TOKEN = String.raw`(\d+\.?\d*B(?:-A\d+\.?\d*B)?)`;
const FLM_SIZE_TOKEN = String.raw`(\d+\.?\d*[bm])`;

function buildFamilyRegex(prefix: string, suffix = '-GGUF$'): RegExp {
  return new RegExp(`^${prefix}-${SIZE_TOKEN}${suffix}`);
}

function buildFlmFamilyRegex(prefix: string): RegExp {
  return new RegExp(`^${prefix}-${FLM_SIZE_TOKEN}-FLM$`);
}

const MODEL_FAMILIES: ModelFamily[] = [
  // Standardized family matching: capture *B or *B-A*B.
  {
    displayName: 'Qwen3',
    regex: buildFamilyRegex('Qwen3'),
  },
  {
    displayName: 'Qwen3-Instruct-2507',
    regex: buildFamilyRegex('Qwen3', '-Instruct-2507-GGUF$'),
  },
  {
    displayName: 'Qwen3.5',
    regex: buildFamilyRegex('Qwen3\\.5'),
  },
  {
    displayName: 'Qwen3-Embedding',
    regex: buildFamilyRegex('Qwen3-Embedding'),
  },
  {
    displayName: 'Qwen2.5-VL-Instruct',
    regex: buildFamilyRegex('Qwen2\\.5-VL', '-Instruct-GGUF$'),
  },
  {
    displayName: 'Qwen3-VL-Instruct',
    regex: buildFamilyRegex('Qwen3-VL', '-Instruct-GGUF$'),
  },
  {
    displayName: 'Llama-3.2-Instruct',
    regex: buildFamilyRegex('Llama-3\\.2', '-Instruct-GGUF$'),
  },
  {
    displayName: 'gpt-oss',
    regex: /^gpt-oss-(\d+\.?\d*b)-mxfp4?-GGUF$/,
  },
  {
    displayName: 'LFM2',
    regex: buildFamilyRegex('LFM2'),
  },
  // FLM families
  {
    displayName: 'gemma3',
    regex: buildFlmFamilyRegex('gemma3'),
  },
  {
    displayName: 'lfm2',
    regex: buildFlmFamilyRegex('lfm2'),
  },
  {
    displayName: 'llama3.2',
    regex: buildFlmFamilyRegex('llama3\\.2'),
  },
  {
    displayName: 'qwen3',
    regex: buildFlmFamilyRegex('qwen3'),
  },
];

type ModelListItem =
  | { type: 'model'; name: string; info: ModelInfo }
  | { type: 'family'; family: ModelFamily; members: { label: string; name: string; info: ModelInfo }[] };

function buildModelList(
  models: Array<{ name: string; info: ModelInfo }>
): ModelListItem[] {
  // Build family groups
  const consumed = new Set<string>();
  const familyItems: ModelListItem[] = [];

  for (const family of MODEL_FAMILIES) {
    const members: { label: string; name: string; info: ModelInfo }[] = [];
    for (const m of models) {
      const match = family.regex.exec(m.name);
      if (match) {
        members.push({ label: match[1], name: m.name, info: m.info });
        consumed.add(m.name);
      }
    }
    if (members.length > 1) {
      members.sort((a, b) => parseFloat(a.label) - parseFloat(b.label));
      familyItems.push({ type: 'family', family, members });
    } else {
      members.forEach(m => consumed.delete(m.name));
    }
  }

  // Build individual items for non-consumed models
  const individualItems: ModelListItem[] = models
    .filter(m => !consumed.has(m.name))
    .map(m => ({ type: 'model' as const, name: m.name, info: m.info }));

  // Merge and sort alphabetically by display name
  const allItems = [...familyItems, ...individualItems];
  allItems.sort((a, b) => {
    const nameA = a.type === 'family' ? a.family.displayName : a.name;
    const nameB = b.type === 'family' ? b.family.displayName : b.name;
    return nameA.localeCompare(nameB);
  });

  return allItems;
}

interface ModelManagerProps {
  isContentVisible: boolean;
  onContentVisibilityChange: (visible: boolean) => void;
  width?: number;
  currentView: LeftPanelView;
  onViewChange: (view: LeftPanelView) => void;
}

interface ModelJSON {
  id?: string,
  model_name?: string,
  recipe: string,
  recipe_options?: object,
  checkpoint?: string,
  checkpoints?: string[],
  downloaded?: boolean,
  labels?: string[],
  size?: number,
  image_defaults?: []
}

export type LeftPanelView = 'models' | 'backends' | 'marketplace' | 'settings';

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

const ModelManager: React.FC<ModelManagerProps> = ({ isContentVisible, onContentVisibilityChange, width = 280, currentView, onViewChange }) => {
  // Get shared model data from context
  const { modelsData, suggestedModels, refresh: refreshModels } = useModels();
  // Get system context for lazy loading system info
  const { ensureSystemInfoLoaded, systemInfo } = useSystem();

  const [expandedCategories, setExpandedCategories] = useState<Set<string>>(new Set(['all']));
  const [organizationMode, setOrganizationMode] = useState<'recipe' | 'category'>('recipe');
  const [showDownloadedOnly, setShowDownloadedOnly] = useState(false);
  const [showFilterPanel, setShowFilterPanel] = useState(false);
  const [showAddModelForm, setShowAddModelForm] = useState(false);
  const [searchQuery, setSearchQuery] = useState('');
  const [loadedModels, setLoadedModels] = useState<Set<string>>(new Set());
  const [loadingModels, setLoadingModels] = useState<Set<string>>(new Set());
  const [hoveredModel, setHoveredModel] = useState<string | null>(null);
  const [optionsModel, setOptionsModel] = useState<string | null>(null);
  const [showModelOptionsModal, setShowModelOptionsModal] = useState(false);
  const [newModel, setNewModel] = useState(createEmptyModelForm);
  const [selectedMarketplaceCategory, setSelectedMarketplaceCategory] = useState<string>('all');
  const [marketplaceCategories, setMarketplaceCategories] = useState<MarketplaceCategory[]>([]);
  const [expandedFamilies, setExpandedFamilies] = useState<Set<string>>(new Set());
  const filterAnchorRef = useRef<HTMLDivElement | null>(null);
  const addModelFromJSONRef = useRef<HTMLInputElement>(null);


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

  // Load system info on mount so recipe categories (e.g., FLM) can appear
  // even when the backend isn't installed yet
  useEffect(() => {
    ensureSystemInfoLoaded();
  }, [ensureSystemInfoLoaded]);

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
    setShowFilterPanel(false);
  }, [currentView]);

  useEffect(() => {
    if (!showFilterPanel) return;

    const handlePointerDown = (event: MouseEvent) => {
      const target = event.target as Node | null;
      if (!target) return;
      if (filterAnchorRef.current?.contains(target)) return;
      setShowFilterPanel(false);
    };

    document.addEventListener('mousedown', handlePointerDown);
    return () => {
      document.removeEventListener('mousedown', handlePointerDown);
    };
  }, [showFilterPanel]);

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

    // Inject empty categories for supported recipes that have no models
    // (e.g., FLM when backend needs install/upgrade)
    const recipes = systemInfo?.recipes;
    if (recipes && !showDownloadedOnly) {
      for (const [recipeName, recipe] of Object.entries(recipes)) {
        if (grouped[recipeName]) continue; // Already has models
        const backends = recipe?.backends;
        if (!backends) continue;
        // Check if any backend is non-unsupported (installable, update_required, or installed)
        const hasViableBackend = Object.values(backends).some(
          b => b?.state && b.state !== 'unsupported'
        );
        if (hasViableBackend) {
          grouped[recipeName] = [];
        }
      }
    }

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

  const groupedModels = useMemo(
    () => organizationMode === 'recipe' ? groupModelsByRecipe() : groupModelsByCategory(),
    [suggestedModels, modelsData, organizationMode, showDownloadedOnly, searchQuery, systemInfo?.recipes]
  );
  const availableModelCount = useMemo(
    () => Object.values(groupedModels).reduce((sum, arr) => sum + arr.length, 0),
    [groupedModels]
  );
  const categories = useMemo(() => Object.keys(groupedModels).sort(), [groupedModels]);
  const builtModelLists = useMemo(
    () => Object.fromEntries(
      Object.entries(groupedModels).map(([cat, models]) => [cat, buildModelList(models)])
    ),
    [groupedModels]
  );

  // Auto-expand the single category if only one is available
  useEffect(() => {
    if (categories.length === 1 && !expandedCategories.has(categories[0])) {
      setExpandedCategories(new Set([categories[0]]));
    }
  }, [categories]);

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

  const getModelSize = (modelName: string, info: ModelInfo): number | undefined => {
    if (!isExperienceModel(info)) {
      return info.size;
    }
    const components = getExperienceComponents(info);
    if (components.length === 0) return info.size;
    const total = components.reduce((sum, component) => sum + (modelsData[component]?.size || 0), 0);
    return total > 0 ? total : info.size;
  };

  const getDisplayLabelsForModel = (modelName: string, info: ModelInfo): string[] => {
    if (isExperienceModel(info)) {
      // Experiences intentionally show a single, consistent legend marker.
      return ['experience'];
    }
    return (info.labels || []).filter((label): label is string => typeof label === 'string' && label.length > 0);
  };

  const getModelDownloadedState = (modelName: string, info: ModelInfo): boolean => {
    return isModelEffectivelyDownloaded(modelName, info, modelsData);
  };

  const getModelLoadedState = (modelName: string, info: ModelInfo): boolean => {
    if (isExperienceModel(info)) {
      return isExperienceFullyLoaded(modelName, modelsData, loadedModels);
    }
    return loadedModels.has(modelName);
  };

  const getModelLoadingState = (modelName: string): boolean => {
    return loadingModels.has(modelName);
  };

  const getCategoryLabel = (category: string): string => {
    const labels: { [key: string]: string } = {
      'experience': 'Experience',
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

  // Auto-expand all categories when searching
  const shouldShowCategory = (category: string): boolean => {
    if (searchQuery.trim()) {
      return true; // Show all categories when searching
    }
    return expandedCategories.has(category);
  };

  const getDisplayLabel = (key: string): string => {
    if (organizationMode === 'recipe') {
      return RECIPE_DISPLAY_NAMES[key] || key;
    } else {
      return getCategoryLabel(key);
    }
  };

  const loadedModelEntries = Array.from(loadedModels)
    .map(modelName => ({ modelName }))
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
    let downloadId: string | null = null;

    try {
      // Trigger system info load on first model download (lazy loading)
      await ensureSystemInfoLoaded();

      // Ensure the backend for this model's recipe is installed
      const recipe = (registrationData?.recipe) || modelsData[modelName]?.recipe;
      if (recipe) {
        // Fetch fresh system-info directly (avoid stale closure over React state)
        const freshSystemInfo = await fetchSystemInfoData();
        await ensureBackendForRecipe(recipe, freshSystemInfo.info?.recipes);
      }

      // For registered models, verify metadata exists; for new models, we're registering now
      if (!registrationData && !modelsData[modelName]) {
        showError('Model metadata is unavailable. Please refresh and try again.');
        return;
      }

      // Add to loading state to show loading indicator
      setLoadingModels(prev => new Set(prev).add(modelName));

      // Use the single consolidated download function
      await pullModel(modelName, { registrationData: registrationData });

      await fetchCurrentLoadedModel();
      showSuccess(`Model "${modelName}" downloaded successfully.`);
    } catch (error) {
      if (error instanceof DownloadAbortError) {
        if (error.reason === 'paused') {
          showWarning(`Download paused: ${modelName}`);
        } else {
          showWarning(`Download cancelled: ${modelName}`);
        }
      } else {
        const errorMsg = error instanceof Error ? error.message : 'Unknown error';
        console.error('Error downloading model:', error);

        // Detect driver-related errors and open the driver guide iframe
        if (errorMsg.toLowerCase().includes('driver') && errorMsg.toLowerCase().includes('older than required')) {
          window.dispatchEvent(new CustomEvent('open-external-content', {
            detail: { url: 'https://lemonade-server.ai/driver_install.html' }
          }));
          showError('Your NPU driver needs to be updated. Please follow the guide.');
        } else {
          showError(`Failed to download model: ${errorMsg}`);
        }
      }
    } finally {
      // Remove from loading state
      setLoadingModels(prev => {
        const newSet = new Set(prev);
        newSet.delete(modelName);
        return newSet;
      });
    }
  }, [modelsData, showError, showSuccess, showWarning, fetchCurrentLoadedModel, ensureSystemInfoLoaded]);

  // Separate useEffect for download resume/retry to avoid stale closure issues
  useEffect(() => {
    const handleDownloadResume = (event: CustomEvent) => {
      const { modelName, downloadType } = event.detail;
      if (!modelName) return;
      if (downloadType === 'backend') {
        // Parse "recipe:backend" format from displayName
        const [recipe, backend] = modelName.split(':');
        if (recipe && backend) installBackend(recipe, backend, true);
      } else {
        handleDownloadModel(modelName);
      }
    };

    const handleDownloadRetry = (event: CustomEvent) => {
      const { modelName, downloadType } = event.detail;
      if (!modelName) return;
      if (downloadType === 'backend') {
        const [recipe, backend] = modelName.split(':');
        if (recipe && backend) installBackend(recipe, backend, true);
      } else {
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

  const handleLoadModel = async (modelName: string, options?: RecipeOptions) => {
    try {
      const modelData = modelsData[modelName];
      if (!modelData) {
        showError('Model metadata is unavailable. Please refresh and try again.');
        return;
      }

      if (isExperienceModel(modelData)) {
        const components = getExperienceComponents(modelData);
        if (components.length === 0) {
          showError(`Experience model "${modelName}" has no component models.`);
          return;
        }

        setLoadingModels(prev => {
          const next = new Set(prev);
          next.add(modelName);
          components.forEach((component) => next.add(component));
          return next;
        });
        window.dispatchEvent(new CustomEvent('modelLoadStart', { detail: { modelId: modelName } }));

        for (const component of components) {
          if (!modelsData[component]) {
            throw new Error(`Missing component model "${component}" for ${modelName}.`);
          }
          await ensureModelReady(component, modelsData, {
            onModelLoading: () => {},
            skipHealthCheck: false,
          });
        }

        await fetchCurrentLoadedModel();
        window.dispatchEvent(new CustomEvent('modelLoadEnd', { detail: { modelId: modelName } }));
        window.dispatchEvent(new CustomEvent('modelsUpdated'));
        return;
      }

      setLoadingModels(prev => new Set(prev).add(modelName));
      window.dispatchEvent(new CustomEvent('modelLoadStart', { detail: { modelId: modelName } }));

      const loadBody = options ? recipeOptionsToApi(options) : undefined;

      await ensureModelReady(modelName, modelsData, {
        onModelLoading: () => {}, // already set loading above
        skipHealthCheck: !!options, // Force re-load when options are provided (Load Options modal)
        loadBody,
      });

      await fetchCurrentLoadedModel();
      window.dispatchEvent(new CustomEvent('modelLoadEnd', { detail: { modelId: modelName } }));
      window.dispatchEvent(new CustomEvent('modelsUpdated'));
    } catch (error) {
      if (error instanceof DownloadAbortError) {
        if (error.reason === 'paused') {
          showWarning(`Download paused for ${modelName}`);
        } else {
          showWarning(`Download cancelled for ${modelName}`);
        }
      } else {
        console.error('Error loading model:', error);
        showError(`Failed to load model: ${error instanceof Error ? error.message : 'Unknown error'}`);
      }

      setLoadingModels(prev => {
        const next = new Set(prev);
        next.delete(modelName);
        const info = modelsData[modelName];
        if (isExperienceModel(info)) {
          getExperienceComponents(info).forEach((component) => next.delete(component));
        }
        return next;
      });
      window.dispatchEvent(new CustomEvent('modelLoadEnd', { detail: { modelId: modelName } }));
    }
  };

  const handleUnloadModel = async (modelName: string) => {
    try {
      const modelData = modelsData[modelName];
      if (modelData && isExperienceModel(modelData)) {
        const components = getExperienceComponents(modelData);
        for (const component of components) {
          if (!loadedModels.has(component)) continue;
          const response = await serverFetch('/unload', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ model_name: component })
          });
          if (!response.ok) {
            throw new Error(`Failed to unload model: ${response.statusText}`);
          }
        }
        await fetchCurrentLoadedModel();
        window.dispatchEvent(new CustomEvent('modelUnload'));
        return;
      }

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
      await deleteModel(modelName);
      // No manual modelsUpdated dispatch needed — deleteModel() handles it
      await fetchCurrentLoadedModel();
      showSuccess(`Model "${modelName}" deleted successfully.`);
    } catch (error) {
      console.error('Error deleting model:', error);
      showError(`Failed to delete model: ${error instanceof Error ? error.message : 'Unknown error'}`);
    }
  };

  const handleUploadModel = (event: React.ChangeEvent<HTMLInputElement>) => {
    const files = event.target.files;

    if (!files || files.length === 0) {
      return;
    }

    const file = files[0];
    const reader = new FileReader();
    reader.onload = (e) => {
      try {
        const result = JSON.parse(e.target?.result as string);
        uploadModelJSON(result);
      } catch(err: any) {
        showError(`Failed to parse JSON file. ${err}`);
      }
    };
    reader.readAsText(file);
    event.target.value = '';
  }

  const uploadModelJSON = (json: ModelJSON) => {
    let modelName: string;

    if (!json.recipe) {
      showError("Invalid model JSON. Recipe is missing");
      return;
    }

    if(!json.model_name && !json.id) {
      showError("Invalid model JSON. Either model or id must be present.");
      return;
    }

    modelName = json.model_name ? json.model_name : json.id as string;

    if (json.checkpoint && json.checkpoints) delete json.checkpoint;
    if (json.model_name) delete json.model_name;
    if(json.id) delete json.id;

    handleDownloadModel(modelName as string, json as ModelRegistrationData);
  }

  const viewTitle = currentView === 'models'
    ? 'Model Manager'
    : currentView === 'backends'
      ? 'Backend Manager'
      : currentView === 'marketplace'
        ? 'Marketplace'
        : 'Settings';

  const searchPlaceholder = currentView === 'models'
    ? 'Filter models...'
    : currentView === 'backends'
      ? 'Filter backends...'
      : currentView === 'marketplace'
        ? 'Filter marketplace...'
        : 'Filter settings...';
  const showInlineFilterButton = currentView === 'models' || currentView === 'marketplace';

  const getModelStatus = (modelName: string) => {
    const isDownloaded = modelsData[modelName]?.downloaded ?? false;
    const isLoaded = loadedModels.has(modelName);
    const isLoading = loadingModels.has(modelName);

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

    return { isDownloaded, isLoaded, isLoading, statusClass, statusTitle };
  };

  const renderLoadOptionsButton = (modelName: string) => (
    <button
      className="model-action-btn load-btn"
      onClick={(e) => {
        e.stopPropagation();
        setOptionsModel(modelName);
        setShowModelOptionsModal(true);
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
  );

  const renderDeleteButton = (modelName: string) => (
    <button
      className="model-action-btn delete-btn"
      onClick={(e) => { e.stopPropagation(); handleDeleteModel(modelName); }}
      title="Delete model"
    >
      <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
        <polyline points="3 6 5 6 21 6" />
        <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
      </svg>
    </button>
  );

  const renderActionButtonsContent = (modelName: string) => {
    const { isDownloaded, isLoaded, isLoading } = getModelStatus(modelName);
    return (
      <>
        {!isDownloaded && (
          <button
            className="model-action-btn download-btn"
            onClick={(e) => { e.stopPropagation(); handleDownloadModel(modelName); }}
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
              onClick={(e) => { e.stopPropagation(); handleLoadModel(modelName); }}
              title="Load model"
            >
              <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                <polygon points="5 3 19 12 5 21" fill="currentColor" />
              </svg>
            </button>
            {renderDeleteButton(modelName)}
            {renderLoadOptionsButton(modelName)}
          </>
        )}
        {isLoaded && (
          <>
            <button
              className="model-action-btn unload-btn"
              onClick={(e) => { e.stopPropagation(); handleUnloadModel(modelName); }}
              title="Eject model"
            >
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                <path d="M9 11L12 8L15 11" />
                <path d="M12 8V16" />
                <path d="M5 20H19" />
              </svg>
            </button>
            {renderDeleteButton(modelName)}
            {renderLoadOptionsButton(modelName)}
          </>
        )}
      </>
    );
  };

  const renderActionButtons = (modelName: string, isHovered: boolean) => {
    if (!isHovered) return null;
    return (
      <span className="model-actions">
        {renderActionButtonsContent(modelName)}
      </span>
    );
  };

  const renderModelItem = (
    modelName: string, modelInfo: ModelInfo, hoverKey: string,
    displayName?: string, extraClass?: string
  ) => {
    const { isDownloaded, statusClass, statusTitle } = getModelStatus(modelName);
    const isHovered = hoveredModel === hoverKey;
    return (
      <div
        key={modelName}
        className={`model-item model-catalog-item ${extraClass ?? ''} ${isDownloaded ? 'downloaded' : ''}`}
        onMouseEnter={() => setHoveredModel(hoverKey)}
        onMouseLeave={() => setHoveredModel(null)}
      >
        <div className="model-item-content">
          <div className="model-info-left">
            <span className={`model-status-indicator ${statusClass}`} title={statusTitle}>●</span>
            <span className="model-name">{displayName ?? modelName}</span>
            <span className="model-size">{formatSize(modelInfo.size)}</span>
            {renderActionButtons(modelName, isHovered)}
          </div>
          {modelInfo.labels && modelInfo.labels.length > 0 && (
            <span className="model-labels">
              {modelInfo.labels.map(label => (
                <span key={label} className={`model-label label-${label}`} title={getCategoryLabel(label)} />
              ))}
            </span>
          )}
        </div>
      </div>
    );
  };

  const toggleFamily = (familyName: string) => {
    setExpandedFamilies(prev => {
      const next = new Set(prev);
      if (next.has(familyName)) next.delete(familyName);
      else next.add(familyName);
      return next;
    });
  };

  const renderFamilyItem = (item: Extract<ModelListItem, { type: 'family' }>) => {
    const { family, members } = item;
    const isExpanded = expandedFamilies.has(family.displayName);

    // Collect shared labels from first member (labels are shared at family level)
    const sharedLabels = members[0]?.info.labels;

    return (
      <div key={family.displayName} className="model-family-group">
        <div
          className="model-family-header"
          onClick={() => toggleFamily(family.displayName)}
        >
          <span className={`family-chevron ${isExpanded ? 'expanded' : ''}`}>
            <ChevronRight size={11} strokeWidth={2.1} />
          </span>
          <span className="model-name family-model-name">{family.displayName}</span>
          {sharedLabels && sharedLabels.length > 0 && (
            <span className="model-labels">
              {sharedLabels.map(label => (
                <span key={label} className={`model-label label-${label}`} title={getCategoryLabel(label)} />
              ))}
            </span>
          )}
        </div>
        {isExpanded && (
          <div className="model-family-members-list">
            {members.map(m =>
              renderModelItem(
                m.name, m.info,
                `family-${family.displayName}-${m.label}`,
                m.label, 'model-family-member-row'
              )
            )}
          </div>
        )}
      </div>
    );
  };

  // Get the default backend info for a recipe from system-info
  const getRecipeBackendInfo = (recipe: string): {
    state: BackendInfo['state'];
    message: string;
    backend: string;
    size?: number;
    version?: string;
    action?: string;
  } | null => {
    const recipes = systemInfo?.recipes;
    if (!recipes || !recipes[recipe]) return null;
    const recipeInfo = recipes[recipe];
    const defaultBackend = recipeInfo.default_backend;
    if (!defaultBackend || !recipeInfo.backends[defaultBackend]) return null;
    const info = recipeInfo.backends[defaultBackend];
    return {
      state: info.state,
      message: info.message,
      backend: defaultBackend,
      size: info.download_size_mb,
      version: info.version,
      action: info.action,
    };
  };

  const renderBackendSetupBanner = (recipe: string) => {
    const info = getRecipeBackendInfo(recipe);
    if (!info || info.state === 'installed') return null;
    if (info.state === 'unsupported') return null;

    const isUpdate = info.state === 'update_required';
    const defaultMessage = isUpdate
      ? 'A backend update is required to show models.'
      : 'Install the backend to browse and download models.';

    return (
      <ConnectedBackendRow
        recipe={recipe}
        backend={info.backend}
        showError={showError}
        showSuccess={showSuccess}
        variant="banner"
        statusMessage={info.message || defaultMessage}
        sizeLabel={info.size ? `${Math.round(info.size)} MB` : null}
      />
    );
  };

  const renderModelsView = () => (
    <>
      {categories.map(category => {
        const listItems = builtModelLists[category] || [];
        const hasModels = groupedModels[category]?.length > 0;
        return (
          <div key={category} className="model-category">
            <div
              className="model-category-header"
              onClick={() => toggleCategory(category)}
            >
              <span className={`category-chevron ${shouldShowCategory(category) ? 'expanded' : ''}`}>
                <ChevronRight size={11} strokeWidth={2.1} />
              </span>
              <span className="category-label">{getDisplayLabel(category)}</span>
              {hasModels && <span className="category-count">({groupedModels[category].length})</span>}
            </div>

            {shouldShowCategory(category) && (
              <div className="model-list">
                {organizationMode === 'recipe' && !hasModels && renderBackendSetupBanner(category)}
                <ModelOptionsModal model={optionsModel} isOpen={showModelOptionsModal}
                                   onCancel={() => {
                                     setShowModelOptionsModal(false);
                                     setOptionsModel(null);
                                   }}
                                   onSubmit={(modelName, options) => {
                                     setShowModelOptionsModal(false);
                                     setOptionsModel(null);
                                     handleLoadModel(modelName, options);
                                   }}/>
                {listItems.map(item => {
                  if (item.type === 'family') {
                    return renderFamilyItem(item);
                  }
                  return renderModelItem(item.name, item.info, item.name);
                })}
              </div>
            )}
          </div>
        );
      })}
    </>
  );

  const handleRailClick = (view: LeftPanelView) => {
    if (view === currentView && isContentVisible) {
      onContentVisibilityChange(false);
    } else {
      onViewChange(view);
      onContentVisibilityChange(true);
    }
  };

  return (
    <div className="model-manager" style={{ width: `${width}px` }}>
      <ToastContainer toasts={toasts} onRemove={removeToast} />
      <ConfirmDialog />
      <div className="left-panel-shell">
        <div className="left-panel-mode-rail">
          <button className={`left-panel-mode-btn ${currentView === 'models' && isContentVisible ? 'active' : ''}`} onClick={() => handleRailClick('models')} title="Models" aria-label="Models">
            <Boxes size={14} strokeWidth={1.9} />
          </button>
          <button className={`left-panel-mode-btn ${currentView === 'backends' && isContentVisible ? 'active' : ''}`} onClick={() => handleRailClick('backends')} title="Backends" aria-label="Backends">
            <Cpu size={14} strokeWidth={1.9} />
          </button>
          <button className={`left-panel-mode-btn ${currentView === 'marketplace' && isContentVisible ? 'active' : ''}`} onClick={() => handleRailClick('marketplace')} title="Marketplace" aria-label="Marketplace">
            <Store size={14} strokeWidth={1.9} />
          </button>
          <div className="left-panel-mode-rail-spacer" />
          <button className={`left-panel-mode-btn ${currentView === 'settings' && isContentVisible ? 'active' : ''}`} onClick={() => handleRailClick('settings')} title="Settings" aria-label="Settings">
            <Settings size={14} strokeWidth={1.9} />
          </button>
        </div>

        {isContentVisible && <div className={`left-panel-main ${showFilterPanel ? 'filter-menu-open' : ''}`}>
          <div className="model-manager-header">
            <div className="left-panel-header-top">
              <h3>{viewTitle}</h3>
            </div>
            <div ref={filterAnchorRef} className={`model-search ${showInlineFilterButton ? 'with-inline-filter' : ''}`}>
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
              {currentView === 'marketplace' && showFilterPanel && (
                <div className="left-panel-filter-popover marketplace-filter-popover">
                  <div className="marketplace-filter-list">
                    <button
                      type="button"
                      className={`marketplace-filter-option ${selectedMarketplaceCategory === 'all' ? 'active' : ''}`}
                      onClick={() => {
                        setSelectedMarketplaceCategory('all');
                        setShowFilterPanel(false);
                      }}
                    >
                      All
                    </button>
                    {marketplaceCategories.map((category) => (
                      <button
                        key={category.id}
                        type="button"
                        className={`marketplace-filter-option ${selectedMarketplaceCategory === category.id ? 'active' : ''}`}
                        onClick={() => {
                          setSelectedMarketplaceCategory(category.id);
                          setShowFilterPanel(false);
                        }}
                      >
                        {category.label}
                      </button>
                    ))}
                  </div>
                </div>
              )}
              {currentView === 'models' && showFilterPanel && (
                <div className="left-panel-filter-popover model-filter-popover">
                  <div className="organization-toggle">
                    <button className={`toggle-button ${organizationMode === 'recipe' ? 'active' : ''}`} onClick={() => {
                      setOrganizationMode('recipe');
                      setShowFilterPanel(false);
                    }}>
                      By Recipe
                    </button>
                    <button className={`toggle-button ${organizationMode === 'category' ? 'active' : ''}`} onClick={() => {
                      setOrganizationMode('category');
                      setShowFilterPanel(false);
                    }}>
                      By Category
                    </button>
                  </div>
                  <label className="toggle-switch-label">
                    <span className="toggle-label-text">Downloaded only</span>
                    <div className="toggle-switch">
                      <input type="checkbox" checked={showDownloadedOnly} onChange={(e) => {
                        setShowDownloadedOnly(e.target.checked);
                        setShowFilterPanel(false);
                      }} />
                      <span className="toggle-slider"></span>
                    </div>
                  </label>
                </div>
              )}
            </div>
          </div>

          {currentView === 'models' && (
            <div className="loaded-model-section widget">
              <div className="loaded-model-header">
                <div className="loaded-model-label">ACTIVE MODELS</div>
                <div className="loaded-model-count-pill">{loadedModelEntries.length} loaded</div>
              </div>
              {loadedModelEntries.length === 0 && <div className="loaded-model-empty">No models loaded</div>}
              <div className="loaded-model-list">
                {loadedModelEntries.map(({ modelName }) => (
                  <div key={modelName} className="loaded-model-info">
                    <div className="loaded-model-details">
                      <span className="loaded-model-indicator">●</span>
                      <span className="loaded-model-name">{modelName}</span>
                    </div>
                    <button className="model-action-btn unload-btn active-model-eject-button" onClick={() => handleUnloadModel(modelName)} title="Eject model">
                      <EjectIcon />
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
            {currentView === 'marketplace' && (
              <MarketplacePanel
                searchQuery={searchQuery}
                selectedCategory={selectedMarketplaceCategory}
                onCategoriesLoaded={setMarketplaceCategories}
              />
            )}
            {currentView === 'backends' && (
              <BackendManager
                searchQuery={searchQuery}
                showError={showError}
                showSuccess={showSuccess}
              />
            )}
            {currentView === 'settings' && <SettingsPanel isVisible={true} searchQuery={searchQuery} />}
          </div>

          {currentView === 'models' && (
            <div className="model-manager-footer">
              {!showAddModelForm ? (
                <div className="add-model-buttons-container">
                  <input ref={addModelFromJSONRef} type="file" accept=".json" onChange={handleUploadModel} style={{ display: 'none' }}/>
                  <button className="add-model-button" onClick={() => addModelFromJSONRef.current?.click()} title="Import JSON">
                    Import a model
                  </button>
                  <button
                    className="add-model-button"
                    onClick={() => {
                      setNewModel(createEmptyModelForm());
                      setShowAddModelForm(true);
                    }}
                  >
                    Add a model
                  </button>
                </div>
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
        </div>}
      </div>
    </div>
  );
};

export default ModelManager;
