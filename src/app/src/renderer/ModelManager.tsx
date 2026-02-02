import React, { useState, useEffect, useCallback } from 'react';
import { ModelInfo } from './utils/modelData';
import { ToastContainer, useToast } from './Toast';
import { useConfirmDialog } from './ConfirmDialog';
import { serverFetch } from './utils/serverConfig';
import { downloadTracker } from './utils/downloadTracker';
import { useModels } from './hooks/useModels';
import ModelOptionsModal from "./ModelOptionsModal";
import { RecipeOptions } from "./recipes/recipeOptions";
import { OgaRecipies } from "./recipes/onnx/recipeOptions";

interface ModelManagerProps {
  isVisible: boolean;
  width?: number;
}

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

const ModelManager: React.FC<ModelManagerProps> = ({ isVisible, width = 280 }) => {
  // Get shared model data from context
  const { modelsData, suggestedModels, refresh: refreshModels } = useModels();

  const [expandedCategories, setExpandedCategories] = useState<Set<string>>(new Set(['all']));
  const [organizationMode, setOrganizationMode] = useState<'recipe' | 'category'>('recipe');
  const [showDownloadedOnly, setShowDownloadedOnly] = useState(false);
  const [showAddModelForm, setShowAddModelForm] = useState(false);
  const [searchQuery, setSearchQuery] = useState('');
  const [loadedModels, setLoadedModels] = useState<Set<string>>(new Set());
  const [loadingModels, setLoadingModels] = useState<Set<string>>(new Set());
  const [hoveredModel, setHoveredModel] = useState<string | null>(null);
  const [showModelOptionsModal, setShowModelOptionsModal] = useState(false);
  const [newModel, setNewModel] = useState(createEmptyModelForm);

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
      'oga-cpu': 'CPU',
      'oga-hybrid': 'Hybrid',
      'oga-npu': 'NPU',
      'oga-igpu': 'iGPU',
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
        'oga-cpu': 'ONNX Runtime CPU',
        'oga-hybrid': 'ONNX Runtime Hybrid',
        'oga-npu': 'ONNX Runtime NPU',
        'whispercpp': 'Whisper.cpp',
        'sd-cpp': 'StableDiffusion.cpp'
      };
      return recipeLabels[key] || key;
    } else {
      // Use friendly labels for categories
      return getCategoryLabel(key);
    }
  };

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

      // if options are provided, which is not required
      if (options) {
        if (options.recipe === 'llamacpp') {
          modelData = {
            ...modelData,
            ctx_size: options.ctxSize.value,
            // llamacpp_backend: options.llamacppBackend.value,
            llamacpp_args: options.llamacppArgs.value,
            save_options: options.saveOptions.value
          }
        } else if (options.recipe === 'whispercpp') {
          modelData = { ...modelData, ctx_size: options.ctxSize.value, save_options: options.saveOptions.value }
        } else if (options.recipe === 'sd-cpp') {
          modelData = { ...modelData, ctx_size: options.ctxSize.value, save_options: options.saveOptions.value }
        } else if (options.recipe === 'flm') {
          modelData = { ...modelData, ctx_size: options.ctxSize.value, save_options: options.saveOptions.value }
        } else if (OgaRecipies.includes(options.recipe)) {
          modelData = { ...modelData, ctx_size: options.ctxSize.value, save_options: options.saveOptions.value }
        }
      }

      // Add to loading state
      setLoadingModels(prev => new Set(prev).add(modelName));

      // Dispatch event to notify other components
      window.dispatchEvent(new CustomEvent('modelLoadStart', { detail: { modelId: modelName } }));

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

  return (
    <div className="model-manager" style={{ width: `${width}px` }}>
      <ToastContainer toasts={toasts} onRemove={removeToast} />
      <ConfirmDialog />
      <div className="model-manager-header">
        <h3>MODEL MANAGER</h3>
        <div className="organization-toggle">
          <button
            className={`toggle-button ${organizationMode === 'recipe' ? 'active' : ''}`}
            onClick={() => setOrganizationMode('recipe')}
          >
            By Recipe
          </button>
          <button
            className={`toggle-button ${organizationMode === 'category' ? 'active' : ''}`}
            onClick={() => setOrganizationMode('category')}
          >
            By Category
          </button>
        </div>
        <div className="model-search">
          <input
            type="text"
            className="model-search-input"
            placeholder="Search models..."
            value={searchQuery}
            onChange={(e) => setSearchQuery(e.target.value)}
          />
        </div>
      </div>

      {/* Currently Loaded Models Section */}
      {loadedModels.size > 0 && (
        <div className="loaded-model-section">
          <div className="loaded-model-label">CURRENTLY LOADED ({loadedModels.size})</div>
          {Array.from(loadedModels).map(modelName => (
            <div key={modelName} className="loaded-model-info">
              <div className="loaded-model-details">
                <span className="loaded-model-indicator">●</span>
                <span className="loaded-model-name">{modelName}</span>
              </div>
              <button
                className="eject-model-button"
                onClick={() => handleUnloadModel(modelName)}
                title="Eject model"
              >
                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                  <path d="M9 11L12 8L15 11" />
                  <path d="M12 8V16" />
                  <path d="M5 20H19" />
                </svg>
              </button>
            </div>
          ))}
        </div>
      )}

      <div className="model-manager-content">
        {categories.map(category => (
          <div key={category} className="model-category">
            <div
              className="model-category-header"
              onClick={() => toggleCategory(category)}
            >
              <span className={`category-chevron ${shouldShowCategory(category) ? 'expanded' : ''}`}>
                ▶
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
                      className={`model-item ${isDownloaded ? 'downloaded' : ''}`}
                      onMouseEnter={() => setHoveredModel(model.name)}
                      onMouseLeave={() => setHoveredModel(null)}
                    >
                      <div className="model-item-content">
                        <div className="model-info-left">
                          <span className="model-name">
                            <span
                              className={`model-status-indicator ${statusClass}`}
                              title={statusTitle}
                            >
                              ●
                            </span>
                            {model.name}
                          </span>
                          <span className="model-size">{formatSize(model.info.size)}</span>

                          {/* Action buttons appear right after size on hover */}
                          {isHovered && (
                            <span className="model-actions">
                              {/* Not downloaded: show download button */}
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

                              {/* Downloaded but not loaded: show load button, delete button and load with options button */}
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
                                  {!['sd-cpp', 'whispercpp'].includes(model.info.recipe) && <button
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
                                  </button>}
                                </>
                              )}

                              {/* Downloaded and loaded: show unload button, delete button, and load with options button */}
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
                                  {!['sd-cpp', 'whispercpp'].includes(model.info.recipe) && <button
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
                                  </button>}
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
      </div>

      <div className="downloaded-filter">
        <label className="toggle-switch-label">
          <span className="toggle-label-text">Downloaded only</span>
          <div className="toggle-switch">
            <input
              type="checkbox"
              checked={showDownloadedOnly}
              onChange={(e) => setShowDownloadedOnly(e.target.checked)}
            />
            <span className="toggle-slider"></span>
          </div>
        </label>
      </div>

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
                <option value="oga-cpu">ONNX Runtime CPU</option>
                <option value="oga-hybrid">ONNX Runtime Hybrid</option>
                <option value="oga-npu">ONNX Runtime NPU</option>
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
              <button
                className="install-button"
                onClick={handleInstallModel}
              >
                Install
              </button>
              <button
                className="cancel-button"
                onClick={resetNewModelForm}
              >
                Cancel
              </button>
            </div>
          </div>
        )}
      </div>
    </div>
  );
};

export default ModelManager;
