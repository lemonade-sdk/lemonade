import React, { useState, useEffect, useCallback, useRef } from 'react';

// Types for Hugging Face API responses
interface HFModelInfo {
  id: string;
  modelId: string;
  author: string;
  downloads: number;
  likes: number;
  tags: string[];
  pipeline_tag?: string;
  lastModified: string;
}

interface HFSibling {
  rfilename: string;
}

interface HFModelDetails {
  id: string;
  modelId: string;
  siblings: HFSibling[];
  tags: string[];
}

interface GGUFQuantization {
  filename: string;
  quantization: string;
}

interface DetectedBackend {
  recipe: string;
  label: string;
  quantizations?: GGUFQuantization[];
}

interface AddModelModalProps {
  isOpen: boolean;
  onClose: () => void;
  onInstall: (modelData: {
    name: string;
    checkpoint: string;
    recipe: string;
    mmproj?: string;
    reasoning?: boolean;
    vision?: boolean;
    embedding?: boolean;
    reranking?: boolean;
  }) => void;
}

type TabType = 'search' | 'custom';

const AddModelModal: React.FC<AddModelModalProps> = ({ isOpen, onClose, onInstall }) => {
  const [activeTab, setActiveTab] = useState<TabType>('search');

  // Search tab state
  const [searchQuery, setSearchQuery] = useState('');
  const [searchResults, setSearchResults] = useState<HFModelInfo[]>([]);
  const [isSearching, setIsSearching] = useState(false);
  const [selectedModel, setSelectedModel] = useState<HFModelInfo | null>(null);
  const [detectedBackend, setDetectedBackend] = useState<DetectedBackend | null>(null);
  const [selectedQuantization, setSelectedQuantization] = useState<string>('');
  const [isDetecting, setIsDetecting] = useState(false);
  const searchTimeoutRef = useRef<NodeJS.Timeout | null>(null);

  // Custom tab state
  const [customForm, setCustomForm] = useState({
    name: '',
    checkpoint: '',
    recipe: 'llamacpp',
    mmproj: '',
    reasoning: false,
    vision: false,
    embedding: false,
    reranking: false,
  });

  // Reset state when modal opens/closes
  useEffect(() => {
    if (!isOpen) {
      setSearchQuery('');
      setSearchResults([]);
      setSelectedModel(null);
      setDetectedBackend(null);
      setSelectedQuantization('');
      setCustomForm({
        name: '',
        checkpoint: '',
        recipe: 'llamacpp',
        mmproj: '',
        reasoning: false,
        vision: false,
        embedding: false,
        reranking: false,
      });
    }
  }, [isOpen]);

  // Debounced search for Hugging Face models
  const searchHuggingFace = useCallback(async (query: string) => {
    if (!query.trim() || query.length < 2) {
      setSearchResults([]);
      return;
    }

    setIsSearching(true);
    try {
      const response = await fetch(
        `https://huggingface.co/api/models?search=${encodeURIComponent(query)}&limit=10&sort=downloads&direction=-1`
      );

      if (!response.ok) {
        throw new Error('Failed to search models');
      }

      const data: HFModelInfo[] = await response.json();
      setSearchResults(data);
    } catch (error) {
      console.error('Error searching Hugging Face:', error);
      setSearchResults([]);
    } finally {
      setIsSearching(false);
    }
  }, []);

  // Handle search input with debounce
  useEffect(() => {
    if (searchTimeoutRef.current) {
      clearTimeout(searchTimeoutRef.current);
    }

    searchTimeoutRef.current = setTimeout(() => {
      searchHuggingFace(searchQuery);
    }, 300);

    return () => {
      if (searchTimeoutRef.current) {
        clearTimeout(searchTimeoutRef.current);
      }
    };
  }, [searchQuery, searchHuggingFace]);

  // Detect backend based on repository files
  const detectBackend = useCallback(async (modelId: string) => {
    setIsDetecting(true);
    setDetectedBackend(null);
    setSelectedQuantization('');

    try {
      const response = await fetch(`https://huggingface.co/api/models/${modelId}`);
      if (!response.ok) {
        throw new Error('Failed to fetch model details');
      }

      const data: HFModelDetails = await response.json();
      const files = data.siblings.map(s => s.rfilename.toLowerCase());
      const tags = data.tags || [];

      // Check for GGUF files (llama.cpp)
      const ggufFiles = data.siblings.filter(s =>
        s.rfilename.toLowerCase().endsWith('.gguf')
      );

      if (ggufFiles.length > 0) {
        // Extract quantization info from GGUF filenames
        const quantizations = ggufFiles.map(f => {
          const filename = f.rfilename;
          // Common GGUF quantization patterns: Q4_0, Q4_K_M, Q5_K_S, Q8_0, F16, F32, etc.
          const quantMatch = filename.match(/[-._](Q\d+(?:_\d)?(?:_[KS])?(?:_[MSL])?|F(?:16|32)|IQ\d+(?:_[A-Z]+)?|BF16)[-._]/i) ||
            filename.match(/[-._](Q\d+(?:_\d)?(?:_[KS])?(?:_[MSL])?|F(?:16|32)|IQ\d+(?:_[A-Z]+)?|BF16)\.gguf$/i);
          const quantization = quantMatch ? quantMatch[1].toUpperCase() : 'Unknown';
          return { filename, quantization };
        });

        setDetectedBackend({
          recipe: 'llamacpp',
          label: 'Llama.cpp GPU (GGUF)',
          quantizations,
        });

        // Auto-select first quantization if available
        if (quantizations.length > 0) {
          setSelectedQuantization(quantizations[0].filename);
        }
        return;
      }

      // Check for ONNX files
      const hasOnnx = files.some(f => f.endsWith('.onnx') || f.endsWith('.onnx_data'));
      if (hasOnnx) {
        // Determine ONNX variant based on tags or folder structure
        const hasNpu = tags.includes('npu') || files.some(f => f.includes('npu'));
        const hasHybrid = tags.includes('hybrid') || files.some(f => f.includes('hybrid'));
        const hasIgpu = tags.includes('igpu') || files.some(f => f.includes('igpu'));

        if (hasNpu) {
          setDetectedBackend({ recipe: 'oga-npu', label: 'ONNX Runtime NPU' });
        } else if (hasHybrid) {
          setDetectedBackend({ recipe: 'oga-hybrid', label: 'ONNX Runtime Hybrid' });
        } else if (hasIgpu) {
          setDetectedBackend({ recipe: 'oga-igpu', label: 'ONNX Runtime iGPU' });
        } else {
          setDetectedBackend({ recipe: 'oga-cpu', label: 'ONNX Runtime CPU' });
        }
        return;
      }

      // Check for FLM files
      const hasFlm = tags.includes('flm') || files.some(f => f.includes('flm') || f.endsWith('.flm'));
      if (hasFlm) {
        setDetectedBackend({ recipe: 'flm', label: 'FastFlowLM NPU' });
        return;
      }

      // Check for Whisper cpp
      const hasWhisper = tags.includes('whisper') || modelId.toLowerCase().includes('whisper');
      const hasBin = files.some(f => f.endsWith('.bin'));
      if (hasWhisper && hasBin) {
        setDetectedBackend({ recipe: 'whispercpp', label: 'Whisper.cpp' });
        return;
      }

      // Check for Stable Diffusion cpp
      const hasSdTag = tags.includes('stable-diffusion') || tags.includes('text-to-image') ||
        modelId.toLowerCase().includes('stable-diffusion') || modelId.toLowerCase().includes('flux');
      if (hasSdTag) {
        setDetectedBackend({ recipe: 'sd-cpp', label: 'StableDiffusion.cpp' });
        return;
      }

      // Default fallback - try to guess from tags
      if (tags.includes('text-generation') || tags.includes('conversational')) {
        setDetectedBackend({ recipe: 'llamacpp', label: 'Llama.cpp GPU (requires conversion)' });
      } else {
        setDetectedBackend(null);
      }
    } catch (error) {
      console.error('Error detecting backend:', error);
      setDetectedBackend(null);
    } finally {
      setIsDetecting(false);
    }
  }, []);

  // When a model is selected, detect its backend
  useEffect(() => {
    if (selectedModel) {
      detectBackend(selectedModel.id);
    }
  }, [selectedModel, detectBackend]);

  const handleSelectModel = (model: HFModelInfo) => {
    setSelectedModel(model);
    setSearchResults([]);
    setSearchQuery(model.id);
  };

  const handleSearchInstall = () => {
    if (!selectedModel || !detectedBackend) return;

    let checkpoint = selectedModel.id;

    // For GGUF, append the selected quantization file
    if (detectedBackend.recipe === 'llamacpp' && selectedQuantization) {
      checkpoint = `${selectedModel.id}:${selectedQuantization}`;
    }

    // Generate a clean model name from the HF model ID
    const modelName = selectedModel.id.split('/').pop() || selectedModel.id;

    onInstall({
      name: modelName,
      checkpoint,
      recipe: detectedBackend.recipe,
    });
  };

  const handleCustomInstall = () => {
    const trimmedName = customForm.name.trim();
    const trimmedCheckpoint = customForm.checkpoint.trim();
    const trimmedRecipe = customForm.recipe.trim();
    const trimmedMmproj = customForm.mmproj.trim();

    if (!trimmedName || !trimmedCheckpoint || !trimmedRecipe) {
      return;
    }

    // Validate GGUF checkpoint format
    if (trimmedCheckpoint.toLowerCase().includes('gguf') && !trimmedCheckpoint.includes(':')) {
      alert('GGUF checkpoints must include a variant using the CHECKPOINT:VARIANT syntax');
      return;
    }

    onInstall({
      name: trimmedName,
      checkpoint: trimmedCheckpoint,
      recipe: trimmedRecipe,
      mmproj: trimmedMmproj || undefined,
      reasoning: customForm.reasoning,
      vision: customForm.vision,
      embedding: customForm.embedding,
      reranking: customForm.reranking,
    });
  };

  const handleCustomInputChange = (field: string, value: string | boolean) => {
    setCustomForm(prev => ({
      ...prev,
      [field]: value,
    }));
  };

  const formatDownloads = (downloads: number): string => {
    if (downloads >= 1000000) {
      return `${(downloads / 1000000).toFixed(1)}M`;
    } else if (downloads >= 1000) {
      return `${(downloads / 1000).toFixed(1)}K`;
    }
    return downloads.toString();
  };

  if (!isOpen) return null;

  return (
    <div className="add-model-overlay" onClick={onClose}>
      <div className="add-model-modal" onClick={e => e.stopPropagation()}>
        <div className="add-model-header">
          <h2>Add a Model</h2>
          <button className="add-model-close-button" onClick={onClose}>
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
              <line x1="18" y1="6" x2="6" y2="18" />
              <line x1="6" y1="6" x2="18" y2="18" />
            </svg>
          </button>
        </div>

        <div className="add-model-tabs">
          <button
            className={`add-model-tab ${activeTab === 'search' ? 'active' : ''}`}
            onClick={() => setActiveTab('search')}
          >
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
              <circle cx="11" cy="11" r="8" />
              <line x1="21" y1="21" x2="16.65" y2="16.65" />
            </svg>
            Search Hugging Face
          </button>
          <button
            className={`add-model-tab ${activeTab === 'custom' ? 'active' : ''}`}
            onClick={() => setActiveTab('custom')}
          >
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
              <path d="M12 3v18M3 12h18" />
            </svg>
            Custom Setup
          </button>
        </div>

        <div className="add-model-content">
          {activeTab === 'search' && (
            <div className="hf-search-panel">
              <div className="hf-search-input-container">
                <svg className="hf-search-icon" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                  <circle cx="11" cy="11" r="8" />
                  <line x1="21" y1="21" x2="16.65" y2="16.65" />
                </svg>
                <input
                  type="text"
                  className="hf-search-input"
                  placeholder="Search models on Hugging Face..."
                  value={searchQuery}
                  onChange={(e) => {
                    setSearchQuery(e.target.value);
                    setSelectedModel(null);
                    setDetectedBackend(null);
                  }}
                  autoFocus
                />
                {isSearching && (
                  <div className="hf-search-spinner" />
                )}
              </div>

              {/* Search Results Dropdown */}
              {searchResults.length > 0 && !selectedModel && (
                <div className="hf-search-results">
                  {searchResults.map((model) => (
                    <div
                      key={model.id}
                      className="hf-search-result-item"
                      onClick={() => handleSelectModel(model)}
                    >
                      <div className="hf-result-main">
                        <span className="hf-result-name">{model.id}</span>
                        <span className="hf-result-stats">
                          <span className="hf-result-downloads" title="Downloads">
                            ↓ {formatDownloads(model.downloads)}
                          </span>
                          <span className="hf-result-likes" title="Likes">
                            ♥ {model.likes}
                          </span>
                        </span>
                      </div>
                      {model.pipeline_tag && (
                        <span className="hf-result-tag">{model.pipeline_tag}</span>
                      )}
                    </div>
                  ))}
                </div>
              )}

              {/* Selected Model Details */}
              {selectedModel && (
                <div className="hf-selected-model">
                  <div className="hf-selected-header">
                    <div className="hf-selected-info">
                      <span className="hf-selected-name">{selectedModel.id}</span>
                      <div className="hf-selected-meta">
                        <span className="hf-selected-downloads">
                          ↓ {formatDownloads(selectedModel.downloads)} downloads
                        </span>
                        <span className="hf-selected-likes">
                          ♥ {selectedModel.likes} likes
                        </span>
                      </div>
                    </div>
                    <button
                      className="hf-clear-selection"
                      onClick={() => {
                        setSelectedModel(null);
                        setDetectedBackend(null);
                        setSearchQuery('');
                      }}
                      title="Clear selection"
                    >
                      <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                        <line x1="18" y1="6" x2="6" y2="18" />
                        <line x1="6" y1="6" x2="18" y2="18" />
                      </svg>
                    </button>
                  </div>

                  {/* Backend Detection */}
                  <div className="hf-backend-section">
                    <label className="hf-section-label">Detected Backend</label>
                    {isDetecting ? (
                      <div className="hf-detecting">
                        <div className="hf-detecting-spinner" />
                        <span>Analyzing repository files...</span>
                      </div>
                    ) : detectedBackend ? (
                      <div className="hf-detected-backend">
                        <span className="hf-backend-badge">{detectedBackend.label}</span>
                      </div>
                    ) : (
                      <div className="hf-no-backend">
                        <span>⚠ Could not auto-detect a compatible backend. Try Custom Setup.</span>
                      </div>
                    )}
                  </div>

                  {/* Quantization Selector for GGUF */}
                  {detectedBackend?.recipe === 'llamacpp' && detectedBackend.quantizations && detectedBackend.quantizations.length > 0 && (
                    <div className="hf-quantization-section">
                      <label className="hf-section-label">Select Quantization</label>
                      <select
                        className="hf-quantization-select"
                        value={selectedQuantization}
                        onChange={(e) => setSelectedQuantization(e.target.value)}
                      >
                        {detectedBackend.quantizations.map((q) => (
                          <option key={q.filename} value={q.filename}>
                            {q.quantization} — {q.filename}
                          </option>
                        ))}
                      </select>
                      <span className="hf-quantization-hint">
                        Lower quantization (Q4) = smaller & faster, higher (Q8, F16) = better quality
                      </span>
                    </div>
                  )}

                  {/* Install Button */}
                  <button
                    className="hf-install-button"
                    onClick={handleSearchInstall}
                    disabled={!detectedBackend}
                  >
                    <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                      <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                      <polyline points="7 10 12 15 17 10" />
                      <line x1="12" y1="15" x2="12" y2="3" />
                    </svg>
                    Install Model
                  </button>
                </div>
              )}

              {/* Empty State */}
              {!selectedModel && searchResults.length === 0 && searchQuery.length < 2 && (
                <div className="hf-empty-state">
                  <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5">
                    <circle cx="11" cy="11" r="8" />
                    <line x1="21" y1="21" x2="16.65" y2="16.65" />
                  </svg>
                  <p>Start typing to search models on Hugging Face</p>
                  <span className="hf-empty-hint">
                    Try: "llama", "phi", "qwen", "mistral"
                  </span>
                </div>
              )}

              {/* No Results State */}
              {!selectedModel && searchResults.length === 0 && searchQuery.length >= 2 && !isSearching && (
                <div className="hf-no-results">
                  <p>No models found for "{searchQuery}"</p>
                  <span>Try a different search term or use Custom Setup</span>
                </div>
              )}
            </div>
          )}

          {activeTab === 'custom' && (
            <div className="custom-setup-panel">
              <div className="custom-form-section">
                <label className="custom-form-label" title="A unique name to identify your model in the catalog">
                  Model Name
                </label>
                <div className="custom-input-with-prefix">
                  <span className="custom-input-prefix">user.</span>
                  <input
                    type="text"
                    className="custom-form-input with-prefix"
                    placeholder="Gemma-3-12b-it-GGUF"
                    value={customForm.name}
                    onChange={(e) => handleCustomInputChange('name', e.target.value)}
                  />
                </div>
              </div>

              <div className="custom-form-section">
                <label className="custom-form-label" title="Hugging Face model path (repo/model:quantization)">
                  Checkpoint
                </label>
                <input
                  type="text"
                  className="custom-form-input"
                  placeholder="unsloth/gemma-3-12b-it-GGUF:Q4_0"
                  value={customForm.checkpoint}
                  onChange={(e) => handleCustomInputChange('checkpoint', e.target.value)}
                />
              </div>

              <div className="custom-form-section">
                <label className="custom-form-label" title="Inference backend to use for this model">
                  Recipe
                </label>
                <select
                  className="custom-form-select"
                  value={customForm.recipe}
                  onChange={(e) => handleCustomInputChange('recipe', e.target.value)}
                >
                  <option value="">Select a recipe...</option>
                  <option value="llamacpp">Llama.cpp GPU</option>
                  <option value="flm">FastFlowLM NPU</option>
                  <option value="oga-cpu">ONNX Runtime CPU</option>
                  <option value="oga-hybrid">ONNX Runtime Hybrid</option>
                  <option value="oga-npu">ONNX Runtime NPU</option>
                </select>
              </div>

              <div className="custom-form-section">
                <label className="custom-form-label">More Info</label>
                <div className="custom-form-subsection">
                  <label className="custom-form-label-secondary" title="Multimodal projection file for vision models">
                    mmproj file (Optional)
                  </label>
                  <input
                    type="text"
                    className="custom-form-input"
                    placeholder="mmproj-F16.gguf"
                    value={customForm.mmproj}
                    onChange={(e) => handleCustomInputChange('mmproj', e.target.value)}
                  />
                </div>

                <div className="custom-form-checkboxes">
                  <label className="custom-checkbox-label" title="Enable if model supports chain-of-thought reasoning">
                    <input
                      type="checkbox"
                      checked={customForm.reasoning}
                      onChange={(e) => handleCustomInputChange('reasoning', e.target.checked)}
                    />
                    <span>Reasoning</span>
                  </label>

                  <label className="custom-checkbox-label" title="Enable if model can process images">
                    <input
                      type="checkbox"
                      checked={customForm.vision}
                      onChange={(e) => handleCustomInputChange('vision', e.target.checked)}
                    />
                    <span>Vision</span>
                  </label>

                  <label className="custom-checkbox-label" title="Enable if model generates text embeddings">
                    <input
                      type="checkbox"
                      checked={customForm.embedding}
                      onChange={(e) => handleCustomInputChange('embedding', e.target.checked)}
                    />
                    <span>Embedding</span>
                  </label>

                  <label className="custom-checkbox-label" title="Enable if model performs reranking">
                    <input
                      type="checkbox"
                      checked={customForm.reranking}
                      onChange={(e) => handleCustomInputChange('reranking', e.target.checked)}
                    />
                    <span>Reranking</span>
                  </label>
                </div>
              </div>

              <button
                className="custom-install-button"
                onClick={handleCustomInstall}
                disabled={!customForm.name.trim() || !customForm.checkpoint.trim() || !customForm.recipe.trim()}
              >
                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                  <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                  <polyline points="7 10 12 15 17 10" />
                  <line x1="12" y1="15" x2="12" y2="3" />
                </svg>
                Install
              </button>
            </div>
          )}
        </div>
      </div>
    </div>
  );
};

export default AddModelModal;
