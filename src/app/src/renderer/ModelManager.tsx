import React, { useState, useEffect, useCallback, useRef } from 'react';
import { ModelInfo } from './utils/modelData';
import { ToastContainer, useToast } from './Toast';
import { useConfirmDialog } from './ConfirmDialog';
import { serverFetch } from './utils/serverConfig';
import { downloadTracker } from './utils/downloadTracker';
import { useModels } from './hooks/useModels';
import ModelOptionsModal from "./ModelOptionsModal";
import AddModelPanel from "./AddModelPanel";
import { RecipeOptions, recipeOptionsToApi } from "./recipes/recipeOptions";
import logoSvg from '../../assets/logo.svg';

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
  size?: number;
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
  size?: number;
}

interface DetectedBackend {
  recipe: string;
  label: string;
  quantizations?: GGUFQuantization[];
}

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

// Type for model installation data from AddModelModal
interface ModelInstallData {
  name: string;
  checkpoint: string;
  recipe: string;
  mmproj?: string;
  reasoning?: boolean;
  vision?: boolean;
  embedding?: boolean;
  reranking?: boolean;
}

const ModelManager: React.FC<ModelManagerProps> = ({ isVisible, width = 280 }) => {
  // Get shared model data from context
  const { modelsData, suggestedModels, refresh: refreshModels } = useModels();

  const [expandedCategories, setExpandedCategories] = useState<Set<string>>(new Set(['all']));
  const [modalityFilter, setModalityFilter] = useState<'text' | 'image' | 'audio' | null>(null);
  const [showDownloadedOnly, setShowDownloadedOnly] = useState(false);
  const [showAddModelModal, setShowAddModelModal] = useState(false);
  const [searchQuery, setSearchQuery] = useState('');
  const [loadedModels, setLoadedModels] = useState<Set<string>>(new Set());
  const [loadingModels, setLoadingModels] = useState<Set<string>>(new Set());
  const [hoveredModel, setHoveredModel] = useState<string | null>(null);
  const [showModelOptionsModal, setShowModelOptionsModal] = useState(false);

  // Avatar cache: author -> avatarUrl
  const [avatarCache, setAvatarCache] = useState<Record<string, string>>({});

  // HuggingFace search state
  const [hfSearchResults, setHfSearchResults] = useState<HFModelInfo[]>([]);
  const [isSearchingHF, setIsSearchingHF] = useState(false);
  const [hfModelBackends, setHfModelBackends] = useState<Record<string, DetectedBackend | null>>({});
  const [hfSelectedQuantizations, setHfSelectedQuantizations] = useState<Record<string, string>>({});
  const [hfModelSizes, setHfModelSizes] = useState<Record<string, number | undefined>>({});
  const [detectingBackendFor, setDetectingBackendFor] = useState<string | null>(null);
  const searchTimeoutRef = useRef<NodeJS.Timeout | null>(null);

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
    const groupedModels = groupModelsByRecipe();
    const categories = Object.keys(groupedModels);

    // If only one category exists and it's not already expanded, expand it
    if (categories.length === 1 && !expandedCategories.has(categories[0])) {
      setExpandedCategories(new Set([categories[0]]));
    }
  }, [suggestedModels, modalityFilter, showDownloadedOnly, searchQuery]);

  // Fetch avatar URL from Hugging Face API
  const fetchAvatarUrl = useCallback(async (author: string) => {
    // Skip if already cached or empty
    if (!author || avatarCache[author]) return;

    try {
      const response = await fetch(`https://huggingface.co/api/organizations/${author}/avatar`);
      if (!response.ok) return;
      const data = await response.json();
      if (data.avatarUrl) {
        setAvatarCache(prev => ({ ...prev, [author]: data.avatarUrl }));
      }
    } catch (error) {
      // Silently fail - avatar is not critical
    }
  }, [avatarCache]);

  // Extract author from checkpoint (format: "author/model:quant" or "author/model")
  const getAuthorFromCheckpoint = (checkpoint: string): string => {
    if (!checkpoint) return '';
    const withoutQuant = checkpoint.split(':')[0];
    const parts = withoutQuant.split('/');
    return parts.length > 1 ? parts[0] : '';
  };

  // Fetch avatars when suggested models change
  useEffect(() => {
    const authors = new Set<string>();
    suggestedModels.forEach(model => {
      const author = getAuthorFromCheckpoint(model.info.checkpoint);
      if (author && !avatarCache[author]) {
        authors.add(author);
      }
    });
    authors.forEach(author => fetchAvatarUrl(author));
  }, [suggestedModels, avatarCache, fetchAvatarUrl]);

  // HuggingFace search function - only searches compatible models (GGUF, ONNX, etc.)
  const searchHuggingFace = useCallback(async (query: string) => {
    if (!query.trim() || query.length < 2) {
      setHfSearchResults([]);
      return;
    }

    setIsSearchingHF(true);
    try {
      // Search for all compatible formats in parallel:
      // - gguf: llama.cpp compatible models
      // - onnx: ONNX Runtime models (RyzenAI, NPU, CPU, iGPU, Hybrid)
      const encodedQuery = encodeURIComponent(query);
      const compatibleFilters = ['gguf', 'onnx'];
      
      const searchPromises = compatibleFilters.map(filter =>
        fetch(`https://huggingface.co/api/models?search=${encodedQuery}&filter=${filter}&limit=8&sort=downloads&direction=-1`)
          .then(res => res.ok ? res.json() : [])
          .catch(() => [])
      );

      const results = await Promise.all(searchPromises);
      
      // Merge and deduplicate results by model ID
      const seenIds = new Set<string>();
      const mergedResults: HFModelInfo[] = [];
      
      for (const resultSet of results) {
        for (const model of resultSet) {
          if (!seenIds.has(model.id)) {
            seenIds.add(model.id);
            mergedResults.push(model);
          }
        }
      }
      
      // Sort merged results by downloads and limit to 12
      mergedResults.sort((a, b) => b.downloads - a.downloads);
      const finalResults = mergedResults.slice(0, 12);
      
      setHfSearchResults(finalResults);

      // Fetch avatars for HF results
      const authors = new Set(finalResults.map(model => model.id.split('/')[0]));
      authors.forEach(author => {
        if (!avatarCache[author]) {
          fetchAvatarUrl(author);
        }
      });
    } catch (error) {
      console.error('Error searching Hugging Face:', error);
      setHfSearchResults([]);
    } finally {
      setIsSearchingHF(false);
    }
  }, [avatarCache, fetchAvatarUrl]);

  // Debounced search effect
  useEffect(() => {
    if (searchTimeoutRef.current) {
      clearTimeout(searchTimeoutRef.current);
    }

    if (searchQuery.trim().length >= 2) {
      searchTimeoutRef.current = setTimeout(() => {
        searchHuggingFace(searchQuery);
      }, 400);
    } else {
      setHfSearchResults([]);
    }

    return () => {
      if (searchTimeoutRef.current) {
        clearTimeout(searchTimeoutRef.current);
      }
    };
  }, [searchQuery, searchHuggingFace]);

  // Detect backend for HuggingFace model
  const detectBackend = useCallback(async (modelId: string) => {
    if (hfModelBackends[modelId] !== undefined) return; // Already detected

    setDetectingBackendFor(modelId);

    try {
      // Fetch model details and tree (for file sizes) in parallel
      const [modelResponse, treeResponse] = await Promise.all([
        fetch(`https://huggingface.co/api/models/${modelId}`),
        fetch(`https://huggingface.co/api/models/${modelId}/tree/main`).catch(() => null)
      ]);

      if (!modelResponse.ok) {
        throw new Error('Failed to fetch model details');
      }

      const data: HFModelDetails = await modelResponse.json();
      const files = data.siblings.map(s => s.rfilename.toLowerCase());
      const tags = data.tags || [];

      // Parse tree data for file sizes (path -> size mapping)
      interface TreeFile { path: string; size?: number; type: string }
      let fileSizes: Record<string, number> = {};
      if (treeResponse && treeResponse.ok) {
        try {
          const treeData: TreeFile[] = await treeResponse.json();
          
          // Add root level files to fileSizes
          treeData.forEach(f => {
            if (f.path && f.size !== undefined && f.type === 'file') {
              fileSizes[f.path] = f.size;
            }
          });
          
          // Check if any siblings are in subdirectories (contain '/')
          const subdirs = new Set<string>();
          data.siblings.forEach(s => {
            const slashIdx = s.rfilename.indexOf('/');
            if (slashIdx > 0) {
              subdirs.add(s.rfilename.substring(0, slashIdx));
            }
          });
          
          // Fetch subdirectory contents for file sizes
          if (subdirs.size > 0) {
            const subDirPromises = Array.from(subdirs).map(async (dir) => {
              try {
                const subResponse = await fetch(`https://huggingface.co/api/models/${modelId}/tree/main/${dir}`);
                if (subResponse.ok) {
                  const subData: TreeFile[] = await subResponse.json();
                  subData.forEach(f => {
                    if (f.path && f.size !== undefined && f.type === 'file') {
                      fileSizes[f.path] = f.size;
                    }
                  });
                }
              } catch {
                // Subdirectory fetch failed, continue
              }
            });
            await Promise.all(subDirPromises);
          }
        } catch {
          // Tree parsing failed, continue without sizes
        }
      }

      // Check for GGUF files (llama.cpp)
      const ggufFiles = data.siblings.filter(s =>
        s.rfilename.toLowerCase().endsWith('.gguf')
      );

      if (ggufFiles.length > 0) {
        const quantizations = ggufFiles.map(f => {
          const filename = f.rfilename;
          const quantMatch = filename.match(/[-._](Q\d+(?:_\d)?(?:_[KS])?(?:_[MSL])?|F(?:16|32)|IQ\d+(?:_[A-Z]+)?|BF16)[-._]/i) ||
            filename.match(/[-._](Q\d+(?:_\d)?(?:_[KS])?(?:_[MSL])?|F(?:16|32)|IQ\d+(?:_[A-Z]+)?|BF16)\.gguf$/i);
          const quantization = quantMatch ? quantMatch[1].toUpperCase() : 'Unknown';
          // Get size from tree endpoint
          const size = fileSizes[filename];
          return { filename, quantization, size };
        });

        setHfModelBackends(prev => ({
          ...prev,
          [modelId]: { recipe: 'llamacpp', label: 'GGUF', quantizations }
        }));

        // Auto-select first quantization and set initial size
        if (quantizations.length > 0 && !hfSelectedQuantizations[modelId]) {
          setHfSelectedQuantizations(prev => ({
            ...prev,
            [modelId]: quantizations[0].filename
          }));
          // Set initial size for first quantization
          if (quantizations[0].size !== undefined) {
            setHfModelSizes(prev => ({
              ...prev,
              [modelId]: quantizations[0].size
            }));
          }
        }
        return;
      }

      // Check for ONNX files
      const hasOnnx = files.some(f => f.endsWith('.onnx') || f.endsWith('.onnx_data'));
      if (hasOnnx) {
        const modelIdLower = modelId.toLowerCase();
        // Check for ryzenai naming convention in model ID (e.g., model-ryzenai-npu)
        const hasRyzenaiNpu = modelIdLower.endsWith('-ryzenai-npu') || modelIdLower.includes('-ryzenai-npu-');
        const hasRyzenaiHybrid = modelIdLower.endsWith('-ryzenai-hybrid') || modelIdLower.includes('-ryzenai-hybrid-');
        const hasRyzenaiCpu = modelIdLower.endsWith('-ryzenai-cpu') || modelIdLower.includes('-ryzenai-cpu-');
        
        // Also check tags and file names as fallback
        const hasNpu = hasRyzenaiNpu || tags.includes('npu') || files.some(f => f.includes('npu'));
        const hasHybrid = hasRyzenaiHybrid || tags.includes('hybrid') || files.some(f => f.includes('hybrid'));
        const hasCpu = hasRyzenaiCpu;
        const hasIgpu = tags.includes('igpu') || files.some(f => f.includes('igpu'));

        let backend: DetectedBackend;
        if (hasNpu) {
          backend = { recipe: 'oga-npu', label: 'ONNX NPU' };
        } else if (hasHybrid) {
          backend = { recipe: 'oga-hybrid', label: 'ONNX Hybrid' };
        } else if (hasIgpu) {
          backend = { recipe: 'oga-igpu', label: 'ONNX iGPU' };
        } else if (hasCpu) {
          backend = { recipe: 'oga-cpu', label: 'ONNX CPU' };
        } else {
          backend = { recipe: 'oga-cpu', label: 'ONNX CPU' };
        }
        setHfModelBackends(prev => ({ ...prev, [modelId]: backend }));
        
        // Calculate total size from ONNX-related files using tree data
        const onnxSize = data.siblings
          .filter(s => s.rfilename.toLowerCase().endsWith('.onnx') || 
                      s.rfilename.toLowerCase().endsWith('.onnx_data') ||
                      s.rfilename.toLowerCase().endsWith('.onnx.data'))
          .reduce((sum, s) => sum + (fileSizes[s.rfilename] || 0), 0);
        if (onnxSize > 0) {
          setHfModelSizes(prev => ({ ...prev, [modelId]: onnxSize }));
        }
        return;
      }

      // Check for FLM files
      const hasFlm = tags.includes('flm') || files.some(f => f.includes('flm') || f.endsWith('.flm'));
      if (hasFlm) {
        setHfModelBackends(prev => ({ ...prev, [modelId]: { recipe: 'flm', label: 'FLM NPU' } }));
        return;
      }

      // Check for Whisper cpp
      const hasWhisper = tags.includes('whisper') || modelId.toLowerCase().includes('whisper');
      const hasBin = files.some(f => f.endsWith('.bin'));
      if (hasWhisper && hasBin) {
        setHfModelBackends(prev => ({ ...prev, [modelId]: { recipe: 'whispercpp', label: 'Whisper' } }));
        return;
      }

      // Check for Stable Diffusion cpp
      const hasSdTag = tags.includes('stable-diffusion') || tags.includes('text-to-image') ||
        modelId.toLowerCase().includes('stable-diffusion') || modelId.toLowerCase().includes('flux');
      if (hasSdTag) {
        setHfModelBackends(prev => ({ ...prev, [modelId]: { recipe: 'sd-cpp', label: 'SD.cpp' } }));
        return;
      }

      // Default fallback
      if (tags.includes('text-generation') || tags.includes('conversational')) {
        setHfModelBackends(prev => ({ ...prev, [modelId]: { recipe: 'llamacpp', label: 'Needs GGUF' } }));
      } else {
        setHfModelBackends(prev => ({ ...prev, [modelId]: null }));
      }
    } catch (error) {
      console.error('Error detecting backend:', error);
      setHfModelBackends(prev => ({ ...prev, [modelId]: null }));
    } finally {
      setDetectingBackendFor(null);
    }
  }, [hfModelBackends, hfSelectedQuantizations]);

  // Fetch backend details for all HF search results immediately
  useEffect(() => {
    if (hfSearchResults.length > 0) {
      hfSearchResults.forEach(model => {
        if (hfModelBackends[model.id] === undefined) {
          detectBackend(model.id);
        }
      });
    }
  }, [hfSearchResults, hfModelBackends, detectBackend]);

  // Format download count
  const formatDownloads = (downloads: number): string => {
    if (downloads >= 1000000) {
      return `${(downloads / 1000000).toFixed(1)}M`;
    } else if (downloads >= 1000) {
      return `${(downloads / 1000).toFixed(1)}K`;
    }
    return downloads.toString();
  };

  // Get avatar URL for a model
  const getModelAvatarUrl = (model: { name: string; info: ModelInfo }): string | undefined => {
    const author = getAuthorFromCheckpoint(model.info.checkpoint);
    return author ? avatarCache[author] : undefined;
  };

  // Determine which modality a model belongs to
  const getModelModalities = (model: { name: string; info: ModelInfo }): Set<'text' | 'image' | 'audio'> => {
    const modalities = new Set<'text' | 'image' | 'audio'>();
    const recipe = model.info.recipe || '';
    const labels = model.info.labels || [];

    // Text models: LLMs, embeddings, reranking
    if (['llamacpp', 'oga-cpu', 'oga-hybrid', 'oga-npu', 'oga-igpu', 'flm'].includes(recipe)) {
      modalities.add('text');
    }
    if (labels.includes('embeddings') || labels.includes('reranking')) {
      modalities.add('text');
    }

    // Image models: stable diffusion, vision-capable LLMs
    if (recipe === 'sd-cpp') {
      modalities.add('image');
    }
    if (labels.includes('vision')) {
      modalities.add('image');
    }

    // Audio models: whisper (speech-to-text), TTS
    if (recipe === 'whispercpp' || recipe === 'tts') {
      modalities.add('audio');
    }

    // Default to text if no modality detected
    if (modalities.size === 0) {
      modalities.add('text');
    }

    return modalities;
  };

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

    // Filter by modality
    if (modalityFilter) {
      filtered = filtered.filter(model => {
        const modalities = getModelModalities(model);
        return modalities.has(modalityFilter);
      });
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

  const groupedModels = groupModelsByRecipe();
  const categories = Object.keys(groupedModels).sort();

  // Auto-expand all categories when searching
  const shouldShowCategory = (category: string): boolean => {
    if (searchQuery.trim()) {
      return true; // Show all categories when searching
    }
    return expandedCategories.has(category);
  };

  const getDisplayLabel = (key: string): string => {
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

  const handleInstallModel = useCallback((modelData: ModelInstallData) => {
    // Close the modal
    setShowAddModelModal(false);

    // Generate the model name with user. prefix
    const modelName = `user.${modelData.name}`;

    // Use the same download flow as registered models, but include registration data
    handleDownloadModel(modelName, {
      checkpoint: modelData.checkpoint,
      recipe: modelData.recipe,
      mmproj: modelData.mmproj,
      reasoning: modelData.reasoning,
      vision: modelData.vision,
      embedding: modelData.embedding,
      reranking: modelData.reranking,
    });
  }, [handleDownloadModel]);

  // Handle installing a HuggingFace model
  const handleInstallHFModel = useCallback((hfModel: HFModelInfo) => {
    const backend = hfModelBackends[hfModel.id];
    if (!backend) return;

    let checkpoint = hfModel.id;

    // For GGUF, append the selected quantization file
    if (backend.recipe === 'llamacpp' && hfSelectedQuantizations[hfModel.id]) {
      checkpoint = `${hfModel.id}:${hfSelectedQuantizations[hfModel.id]}`;
    }

    // Generate a clean model name from the HF model ID
    const modelName = `user.${hfModel.id.split('/').pop() || hfModel.id}`;

    // Use the same download flow as registered models
    handleDownloadModel(modelName, {
      checkpoint,
      recipe: backend.recipe,
    });
  }, [hfModelBackends, hfSelectedQuantizations, handleDownloadModel]);

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
        <div className="organization-toggle modality-toggle">
          <button
            className={`toggle-button ${modalityFilter === 'text' ? 'active' : ''}`}
            onClick={() => setModalityFilter(modalityFilter === 'text' ? null : 'text')}
            title="Show text models (LLMs, embeddings)"
          >
            Text
          </button>
          <button
            className={`toggle-button ${modalityFilter === 'image' ? 'active' : ''}`}
            onClick={() => setModalityFilter(modalityFilter === 'image' ? null : 'image')}
            title="Show image models (Stable Diffusion, Vision)"
          >
            Image
          </button>
          <button
            className={`toggle-button ${modalityFilter === 'audio' ? 'active' : ''}`}
            onClick={() => setModalityFilter(modalityFilter === 'audio' ? null : 'audio')}
            title="Show audio models (Speech-to-text, TTS)"
          >
            Audio
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

                  const avatarUrl = getModelAvatarUrl(model);
                  return (
                    <div
                      key={model.name}
                      className={`model-item ${isDownloaded ? 'downloaded' : ''}`}
                      onMouseEnter={() => setHoveredModel(model.name)}
                      onMouseLeave={() => setHoveredModel(null)}
                    >
                      <div className="model-item-content">
                        <div className="model-info-left">
                          <img
                            src={avatarUrl || logoSvg}
                            alt=""
                            className="model-avatar"
                          />
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
                                  {!['sd-cpp'].includes(model.info.recipe) && <button
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
                                  {!['sd-cpp'].includes(model.info.recipe) && <button
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

        {/* HuggingFace Search Results */}
        {searchQuery.trim().length >= 2 && (
          <div className="model-category hf-category">
            <div className="model-category-header hf-header">
              <svg className="hf-icon" width="14" height="14" viewBox="0 0 120 120" fill="currentColor">
                <path d="M37.6,76.5c-3.8,0-6.9-3.1-6.9-6.9V50.5c0-3.8,3.1-6.9,6.9-6.9s6.9,3.1,6.9,6.9v19.2C44.5,73.5,41.4,76.5,37.6,76.5z"/>
                <path d="M82.4,76.5c-3.8,0-6.9-3.1-6.9-6.9V50.5c0-3.8,3.1-6.9,6.9-6.9s6.9,3.1,6.9,6.9v19.2C89.3,73.5,86.2,76.5,82.4,76.5z"/>
                <path d="M60,95.4c-19.6,0-35.5-15.9-35.5-35.5V60c0-3.8,3.1-6.9,6.9-6.9s6.9,3.1,6.9,6.9v0c0,12,9.8,21.8,21.8,21.8s21.8-9.8,21.8-21.8v0c0-3.8,3.1-6.9,6.9-6.9s6.9,3.1,6.9,6.9v0C95.5,79.5,79.6,95.4,60,95.4z"/>
                <circle cx="37.6" cy="36.8" r="6.9"/>
                <circle cx="82.4" cy="36.8" r="6.9"/>
              </svg>
              <span className="category-label">Hugging Face</span>
              {hfSearchResults.length > 0 && (
                <span className="category-count">({hfSearchResults.length})</span>
              )}
            </div>

            <div className="model-list hf-model-list">
              {!isSearchingHF && hfSearchResults.length === 0 && searchQuery.length >= 2 && (
                <div className="hf-empty">No results on Hugging Face</div>
              )}

              {hfSearchResults.map(hfModel => {
                const hfAvatarUrl = avatarCache[hfModel.id.split('/')[0]];
                const backend = hfModelBackends[hfModel.id];
                const isDetecting = detectingBackendFor === hfModel.id;
                const isHovered = hoveredModel === `hf:${hfModel.id}`;
                const quantizations = backend?.quantizations || [];
                const selectedQuant = hfSelectedQuantizations[hfModel.id] || '';
                const selectedQuantInfo = quantizations.find(q => q.filename === selectedQuant);

                return (
                  <div
                    key={`hf:${hfModel.id}`}
                    className="model-item hf-model-item"
                    onMouseEnter={() => setHoveredModel(`hf:${hfModel.id}`)}
                    onMouseLeave={() => setHoveredModel(null)}
                  >
                    <div className="model-item-content">
                      <div className="model-info-left">
                        <img
                          src={hfAvatarUrl || logoSvg}
                          alt=""
                          className="model-avatar"
                        />
                        <span className="model-name hf-model-name">
                          <span className="model-status-indicator hf-indicator" title="Hugging Face">●</span>
                          {hfModel.id}
                          {hfModelSizes[hfModel.id] !== undefined && (
                            <span className="model-size">{formatSize(hfModelSizes[hfModel.id]! / (1024 * 1024 * 1024))}</span>
                          )}
                          {/* Download button - show for non-GGUF backends or GGUF with quantizations */}
                          {isHovered && backend && (backend.recipe !== 'llamacpp' || (backend.quantizations && backend.quantizations.length > 0)) && (
                            <button
                              className="model-action-btn download-btn"
                              onClick={(e) => {
                                e.stopPropagation();
                                handleInstallHFModel(hfModel);
                              }}
                              title="Download from Hugging Face"
                            >
                              <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                                <polyline points="7 10 12 15 17 10" />
                                <line x1="12" y1="15" x2="12" y2="3" />
                              </svg>
                            </button>
                          )}
                        </span>
                      </div>

                      <div className="hf-model-right">
                        <span className="hf-stats">
                          <span className="hf-downloads" title="Downloads">↓{formatDownloads(hfModel.downloads)}</span>
                        </span>
                        {/* Quantization: show as text when not hovered, dropdown when hovered */}
                        {quantizations.length > 0 && (
                          isHovered ? (
                            <select
                              className="hf-quant-dropdown"
                              value={selectedQuant}
                              onChange={(e) => {
                                e.stopPropagation();
                                const newFilename = e.target.value;
                                setHfSelectedQuantizations(prev => ({
                                  ...prev,
                                  [hfModel.id]: newFilename
                                }));
                                // Update size for the selected quantization
                                const selectedQ = quantizations.find(q => q.filename === newFilename);
                                if (selectedQ?.size !== undefined) {
                                  setHfModelSizes(prev => ({
                                    ...prev,
                                    [hfModel.id]: selectedQ.size
                                  }));
                                }
                              }}
                              onClick={(e) => e.stopPropagation()}
                            >
                              {quantizations.map(q => (
                                <option key={q.filename} value={q.filename}>
                                  {q.quantization}
                                </option>
                              ))}
                            </select>
                          ) : (
                            <span className="hf-quant-text">
                              {selectedQuantInfo?.quantization || quantizations[0]?.quantization || ''}
                            </span>
                          )
                        )}

                        {/* Backend badge */}
                        {isDetecting ? (
                          <span className="hf-backend-badge detecting">...</span>
                        ) : backend ? (
                          <span className={`hf-backend-badge ${backend.recipe}`}>{backend.label}</span>
                        ) : null}
                      </div>
                    </div>
                  </div>
                );
              })}
            </div>
          </div>
        )}
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
        {!showAddModelModal ? (
          <button
            className="add-model-button"
            onClick={() => setShowAddModelModal(true)}
          >
            Add a model
          </button>
        ) : (
          <AddModelPanel
            onClose={() => setShowAddModelModal(false)}
            onInstall={handleInstallModel}
          />
        )}
      </div>
    </div>
  );
};

export default ModelManager;
