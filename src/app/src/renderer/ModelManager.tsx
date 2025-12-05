import React, { useState, useEffect, useCallback } from 'react';
import {
  fetchSupportedModelsData,
  ModelInfo,
  ModelsData,
  USER_MODEL_PREFIX
} from './utils/modelData';
import { ToastContainer, useToast } from './Toast';
import { useConfirmDialog } from './ConfirmDialog';
import { serverFetch, onServerPortChange } from './utils/serverConfig';

interface ModelManagerProps {
  isVisible: boolean;
  width?: number;
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
  const [models, setModels] = useState<Array<{ name: string; info: ModelInfo }>>([]);
  const [downloadedModels, setDownloadedModels] = useState<Set<string>>(new Set());
  const [expandedCategories, setExpandedCategories] = useState<Set<string>>(new Set(['all']));
  const [organizationMode, setOrganizationMode] = useState<'recipe' | 'category'>('recipe');
  const [showDownloadedOnly, setShowDownloadedOnly] = useState(false);
  const [showAddModelForm, setShowAddModelForm] = useState(false);
  const [searchQuery, setSearchQuery] = useState('');
  const [currentLoadedModel, setCurrentLoadedModel] = useState<string | null>(null);
  const [loadingModels, setLoadingModels] = useState<Set<string>>(new Set());
  const [hoveredModel, setHoveredModel] = useState<string | null>(null);
  const [newModel, setNewModel] = useState(createEmptyModelForm);
  const [isAddingModel, setIsAddingModel] = useState(false);
  const [supportedModelsData, setSupportedModelsData] = useState<ModelsData>({});
  
  const { toasts, removeToast, showError, showSuccess, showWarning } = useToast();
  const { confirm, ConfirmDialog } = useConfirmDialog();

  const fetchDownloadedModels = useCallback(async () => {
    try {
      const response = await serverFetch('/models');
      const data = await response.json();
      
      // Handle both array format and object with data array
      const modelList = Array.isArray(data) ? data : data.data || [];
      const downloadedModelIds = new Set<string>(modelList.map((m: any) => m.id as string));
      setDownloadedModels(downloadedModelIds);
    } catch (error) {
      console.error('Failed to fetch downloaded models:', error);
    }
  }, []);

  const fetchCurrentLoadedModel = useCallback(async () => {
    try {
      const response = await serverFetch('/health');
      const data = await response.json();
      
      if (data && data.model_loaded) {
        setCurrentLoadedModel(data.model_loaded);
        // Remove from loading state if it was loading
        setLoadingModels(prev => {
          const newSet = new Set(prev);
          newSet.delete(data.model_loaded);
          return newSet;
        });
      } else {
        setCurrentLoadedModel(null);
      }
    } catch (error) {
      console.error('Failed to fetch current loaded model:', error);
    }
  }, []);

  const loadModels = useCallback(async () => {
    try {
      const data = await fetchSupportedModelsData();
      setSupportedModelsData(data);
      const suggestedModels = Object.entries(data)
        .filter(([name, info]) => info.suggested || name.startsWith(USER_MODEL_PREFIX))
        .filter(([name, info]) => info.recipe !== 'whispercpp')
        .map(([name, info]) => ({ name, info }))
        .sort((a, b) => a.name.localeCompare(b.name));
      setModels(suggestedModels);
    } catch (error) {
      console.error('Failed to load models:', error);
    }
  }, []);

  useEffect(() => {
    loadModels();
    fetchDownloadedModels();
    fetchCurrentLoadedModel();
    
    // Poll for model status every 5 seconds to detect loaded models
    const interval = setInterval(() => {
      fetchCurrentLoadedModel();
    }, 5000);

    // Listen for port changes and refetch data
    const unsubscribePortChange = onServerPortChange(() => {
      console.log('Server port changed, refetching model data...');
      loadModels();
      fetchDownloadedModels();
      fetchCurrentLoadedModel();
    });
    
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
      } else {
        loadModels();
      }
    };
    
    window.addEventListener('modelLoadStart' as any, handleModelLoadStart);
    window.addEventListener('modelLoadEnd' as any, handleModelLoadEnd);

    const stopWatchingUserModels = window.api?.watchUserModels?.(() => {
      loadModels();
    });
    
    return () => {
      clearInterval(interval);
      unsubscribePortChange();
      window.removeEventListener('modelLoadStart' as any, handleModelLoadStart);
      window.removeEventListener('modelLoadEnd' as any, handleModelLoadEnd);
      delete (window as any).setModelLoading;
      if (typeof stopWatchingUserModels === 'function') {
        stopWatchingUserModels();
      }
    };
  }, [fetchDownloadedModels, fetchCurrentLoadedModel, loadModels]);

  const getFilteredModels = () => {
    let filtered = models;
    
    // Filter by downloaded status
    if (showDownloadedOnly) {
      filtered = filtered.filter(model => downloadedModels.has(model.name));
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
      'flm': 'FLM'
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
        'oga-npu': 'ONNX Runtime NPU'
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

  const handleInstallModel = async () => {
    if (isAddingModel) {
      return;
    }

    if (!window.api?.addUserModel) {
      showError('Adding custom models is not supported in this build.');
      return;
    }

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

    setIsAddingModel(true);
    try {
      await window.api.addUserModel({
        name: trimmedName,
        checkpoint: trimmedCheckpoint,
        recipe: trimmedRecipe,
        mmproj: trimmedMmproj,
        reasoning: newModel.reasoning,
        vision: newModel.vision,
        embedding: newModel.embedding,
        reranking: newModel.reranking,
      });

      await loadModels();
      resetNewModelForm();
      showSuccess('Model added to your catalog.');
    } catch (error) {
      console.error('Failed to add model:', error);
      showError(
        `Failed to add model: ${
          error instanceof Error ? error.message : 'Unknown error'
        }`
      );
    } finally {
      setIsAddingModel(false);
    }
  };

  const handleInputChange = (field: string, value: string | boolean) => {
    setNewModel(prev => ({
      ...prev,
      [field]: value
    }));
  };

  const handleDownloadModel = async (modelName: string) => {
    try {
      const modelData = supportedModelsData[modelName];
      if (!modelData) {
        showError('Model metadata is unavailable. Please refresh and try again.');
        return;
      }
      
      // Add to loading state to show loading indicator
      setLoadingModels(prev => new Set(prev).add(modelName));
      
      const response = await serverFetch('/pull', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ model_name: modelName, ...modelData })
      });
      
      if (!response.ok) {
        throw new Error(`Failed to download model: ${response.statusText}`);
      }
      
      // Refresh downloaded models and current loaded model status
      await fetchDownloadedModels();
      await fetchCurrentLoadedModel();
      
      // Show success notification
      showSuccess(`Model "${modelName}" downloaded successfully.`);
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
  };

  const handleLoadModel = async (modelName: string) => {
    try {
      const modelData = supportedModelsData[modelName];
      if (!modelData) {
        showError('Model metadata is unavailable. Please refresh and try again.');
        return;
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
        throw new Error(`Failed to load model: ${response.statusText}`);
      }
      
      // Wait a bit for the model to actually load, then refresh status
      setTimeout(async () => {
        await fetchCurrentLoadedModel();
        window.dispatchEvent(new CustomEvent('modelLoadEnd', { detail: { modelId: modelName } }));
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
        method: 'POST'
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
      
      // Refresh downloaded models and current loaded model status
      await fetchDownloadedModels();
      await fetchCurrentLoadedModel();
      await loadModels();
      showSuccess(`Model "${modelName}" deleted successfully.`);
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
      
      {/* Currently Loaded Model Section */}
      {currentLoadedModel && (
        <div className="loaded-model-section">
          <div className="loaded-model-label">CURRENTLY LOADED</div>
          <div className="loaded-model-info">
            <div className="loaded-model-details">
              <span className="loaded-model-indicator">●</span>
              <span className="loaded-model-name">{currentLoadedModel}</span>
            </div>
            <button 
              className="eject-model-button"
              onClick={() => handleUnloadModel(currentLoadedModel)}
              title="Eject model"
            >
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                <path d="M9 11L12 8L15 11" />
                <path d="M12 8V16" />
                <path d="M5 20H19" />
              </svg>
              Eject
            </button>
          </div>
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
                {groupedModels[category].map(model => {
                  const isDownloaded = downloadedModels.has(model.name);
                  const isLoaded = currentLoadedModel === model.name;
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
                              
                              {/* Downloaded but not loaded: show load button + delete button */}
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
                                </>
                              )}
                              
                              {/* Downloaded and loaded: show unload button + delete button */}
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

