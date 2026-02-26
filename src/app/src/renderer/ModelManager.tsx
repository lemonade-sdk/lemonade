import React, { useState, useEffect, useCallback, useRef } from 'react';
import { Boxes, ChevronRight, Cpu, Settings as SettingsIcon, SlidersHorizontal, Store } from 'lucide-react';
import { ModelInfo } from './utils/modelData';
import { ToastContainer, useToast } from './Toast';
import { useConfirmDialog } from './ConfirmDialog';
import { serverFetch } from './utils/serverConfig';
import { pullModel, DownloadAbortError, ensureModelReady, installBackend, deleteModel } from './utils/backendInstaller';
import type { ModelRegistrationData } from './utils/backendInstaller';
import { useModels } from './hooks/useModels';
import { useSystem } from './hooks/useSystem';
import ModelOptionsModal from "./ModelOptionsModal";
import { RecipeOptions, recipeOptionsToApi } from "./recipes/recipeOptions";
import SettingsPanel from './SettingsPanel';
import BackendManager from './BackendManager';
import MarketplacePanel, { MarketplaceCategory } from './MarketplacePanel';
import { RECIPE_DISPLAY_NAMES } from './utils/recipeNames';

interface ModelManagerProps {
  isVisible: boolean;
  width?: number;
  currentView: LeftPanelView;
  onViewChange: (view: LeftPanelView) => void;
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

const ModelManager: React.FC<ModelManagerProps> = ({ isVisible, width = 280, currentView, onViewChange }) => {
  // Get shared model data from context
  const { modelsData, suggestedModels, refresh: refreshModels } = useModels();
  // Get system context for lazy loading system info
  const { ensureSystemInfoLoaded } = useSystem();

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
  const filterAnchorRef = useRef<HTMLDivElement | null>(null);

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
    try {
      // Trigger system info load on first model download (lazy loading)
      await ensureSystemInfoLoaded();

      // For registered models, verify metadata exists; for new models, we're registering now
      if (!registrationData && !modelsData[modelName]) {
        showError('Model metadata is unavailable. Please refresh and try again.');
        return;
      }

      // Add to loading state to show loading indicator
      setLoadingModels(prev => new Set(prev).add(modelName));

      // Use the single consolidated download function
      await pullModel(modelName, { registrationData });

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
        console.error('Error downloading model:', error);
        showError(`Failed to download model: ${error instanceof Error ? error.message : 'Unknown error'}`);
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

      setLoadingModels(prev => { const s = new Set(prev); s.delete(modelName); return s; });
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
      await deleteModel(modelName);
      // No manual modelsUpdated dispatch needed — deleteModel() handles it
      await fetchCurrentLoadedModel();
      showSuccess(`Model "${modelName}" deleted successfully.`);
    } catch (error) {
      console.error('Error deleting model:', error);
      showError(`Failed to delete model: ${error instanceof Error ? error.message : 'Unknown error'}`);
    }
  };

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
                const renderLoadOptionsButton = () => (
                  <button
                    className="model-action-btn load-btn"
                    onClick={(e) => {
                      e.stopPropagation();
                      setOptionsModel(model.name);
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
                                {renderLoadOptionsButton()}
                              </>
                            )}
                            {isLoaded && (
                              <>
                                {renderLoadOptionsButton()}
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

  return (
    <div className="model-manager" style={{ width: `${width}px` }}>
      <ToastContainer toasts={toasts} onRemove={removeToast} />
      <ConfirmDialog />
      <div className="left-panel-shell">
        <div className="left-panel-mode-rail">
          <button className={`left-panel-mode-btn ${currentView === 'models' ? 'active' : ''}`} onClick={() => onViewChange('models')} title="Models" aria-label="Models">
            <Boxes size={14} strokeWidth={1.9} />
          </button>
          <button className={`left-panel-mode-btn ${currentView === 'backends' ? 'active' : ''}`} onClick={() => onViewChange('backends')} title="Backends" aria-label="Backends">
            <Cpu size={14} strokeWidth={1.9} />
          </button>
          <button className={`left-panel-mode-btn ${currentView === 'marketplace' ? 'active' : ''}`} onClick={() => onViewChange('marketplace')} title="Marketplace" aria-label="Marketplace">
            <Store size={14} strokeWidth={1.9} />
          </button>
          <div className="left-panel-mode-rail-spacer" />
          <button className={`left-panel-mode-btn ${currentView === 'settings' ? 'active' : ''}`} onClick={() => onViewChange('settings')} title="Settings" aria-label="Settings">
            <SettingsIcon size={14} strokeWidth={1.9} />
          </button>
        </div>

        <div className={`left-panel-main ${showFilterPanel ? 'filter-menu-open' : ''}`}>
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
