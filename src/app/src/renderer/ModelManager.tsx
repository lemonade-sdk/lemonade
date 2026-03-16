import React, { useState, useEffect, useCallback, useRef, useMemo } from 'react';
import { Boxes, ChevronRight, Cpu, Settings, SlidersHorizontal, Store, XIcon } from './components/Icons';
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

// Types for Hugging Face API responses
interface HFModelInfo {
  id: string;
  author: string;
  downloads: number;
  likes: number;
  tags: string[];
  pipeline_tag?: string;
}

interface HFSibling {
  rfilename: string;
}

interface HFModelDetails {
  id: string;
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
  mmprojFiles?: string[];
}

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


const ModelManager: React.FC<ModelManagerProps> = ({ isContentVisible, onContentVisibilityChange, width = 280, currentView, onViewChange }) => {
  // Get shared model data from context
  const { modelsData, suggestedModels, refresh: refreshModels } = useModels();
  // Get system context for lazy loading system info
  const { ensureSystemInfoLoaded, systemInfo } = useSystem();

  const [expandedCategories, setExpandedCategories] = useState<Set<string>>(new Set(['all']));
  const [organizationMode, setOrganizationMode] = useState<'recipe' | 'category'>('recipe');
  const [showDownloadedOnly, setShowDownloadedOnly] = useState(false);
  const [showFilterPanel, setShowFilterPanel] = useState(false);
const [searchQuery, setSearchQuery] = useState('');
  const [loadedModels, setLoadedModels] = useState<Set<string>>(new Set());
  const [loadingModels, setLoadingModels] = useState<Set<string>>(new Set());
  const [hoveredModel, setHoveredModel] = useState<string | null>(null);
  const [optionsModel, setOptionsModel] = useState<string | null>(null);
  const [showModelOptionsModal, setShowModelOptionsModal] = useState(false);
  const [selectedMarketplaceCategory, setSelectedMarketplaceCategory] = useState<string>('all');
  const [marketplaceCategories, setMarketplaceCategories] = useState<MarketplaceCategory[]>([]);
  const [expandedFamilies, setExpandedFamilies] = useState<Set<string>>(new Set());
  const filterAnchorRef = useRef<HTMLDivElement | null>(null);
  // HuggingFace search state
  const [hfSearchResults, setHfSearchResults] = useState<HFModelInfo[]>([]);
  const [isSearchingHF, setIsSearchingHF] = useState(false);
  const [hfRateLimited, setHfRateLimited] = useState(false);
  const [hfModelBackends, setHfModelBackends] = useState<Record<string, DetectedBackend | null>>({});
  const [hfSelectedQuantizations, setHfSelectedQuantizations] = useState<Record<string, string>>({});
  const [hfModelSizes, setHfModelSizes] = useState<Record<string, number | undefined>>({});
  const [detectingBackendFor, setDetectingBackendFor] = useState<string | null>(null);
  const hfSearchTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null);


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
    if (recipes && !showDownloadedOnly && !searchQuery.trim()) {
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

  // Auto-expand all categories when a search query is entered
  useEffect(() => {
    if (searchQuery.trim()) {
      setExpandedCategories(new Set(categories));
    }
  }, [searchQuery]);

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

  const shouldShowCategory = (category: string): boolean => {
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



  const formatDownloads = (downloads: number): string => {
    if (downloads >= 1_000_000) return `${(downloads / 1_000_000).toFixed(1)}M`;
    if (downloads >= 1_000) return `${(downloads / 1_000).toFixed(1)}K`;
    return String(downloads);
  };

  const searchHuggingFace = useCallback(async (query: string) => {  //Searching the HF API for GGUF, ONNX, and FastFlowLM models
    if (!query.trim() || query.length < 3) {
      setHfSearchResults([]);
      setHfRateLimited(false);
      return;
    }
    setIsSearchingHF(true);
    setHfRateLimited(false);
    try {
      const encoded = encodeURIComponent(query);
      const ggufRes = await fetch(`https://huggingface.co/api/models?search=${encoded}&filter=gguf&limit=12&sort=downloads&direction=-1`);
      if (ggufRes.status === 429) {
        setHfRateLimited(true);
        setHfSearchResults([]);
        return;
      }
      const ggufData: HFModelInfo[] = ggufRes.ok ? await ggufRes.json() : [];
      setHfSearchResults(ggufData);
    } catch {
      setHfSearchResults([]);
    } finally {
      setIsSearchingHF(false);
    }
  }, []);

  const detectBackend = useCallback(async (modelId: string) => {
    if (hfModelBackends[modelId] !== undefined) return;
    setDetectingBackendFor(modelId);
    try {
      const [modelRes, treeRes] = await Promise.all([
        fetch(`https://huggingface.co/api/models/${modelId}`),
        fetch(`https://huggingface.co/api/models/${modelId}/tree/main`).catch(() => null),
      ]);
      if (!modelRes.ok) throw new Error('Failed to fetch model');
      const data: HFModelDetails = await modelRes.json();
      const files = data.siblings.map(s => s.rfilename.toLowerCase());
      const tags = data.tags || [];

      // Build file-size map from tree
      const fileSizes: Record<string, number> = {};
      if (treeRes && treeRes.ok) {
        try {
          const tree: { path: string; size?: number; type: string }[] = await treeRes.json();
          tree.forEach(f => { if (f.type === 'file' && f.size !== undefined) fileSizes[f.path] = f.size; });
        } catch { /* ignore */ }
      }

      // GGUF detection
      const allGgufFiles = data.siblings.filter(s => s.rfilename.toLowerCase().endsWith('.gguf'));
      if (allGgufFiles.length > 0) {
        // Separate mmproj files from regular model files
        const mmprojGgufs = allGgufFiles.filter(s => s.rfilename.toLowerCase().includes('mmproj'));
        const ggufFiles = allGgufFiles.filter(s => !s.rfilename.toLowerCase().includes('mmproj'));
        const mmprojFiles = mmprojGgufs.map(s => {
          const parts = s.rfilename.split('/');
          return parts[parts.length - 1];
        });

        const folderGroups: Record<string, { files: string[]; totalSize: number }> = {};
        const rootFiles: { filename: string; size?: number }[] = [];
        ggufFiles.forEach(f => {
          const slashIdx = f.rfilename.indexOf('/');
          if (slashIdx > 0) {
            const folder = f.rfilename.substring(0, slashIdx);
            if (!folderGroups[folder]) folderGroups[folder] = { files: [], totalSize: 0 };
            folderGroups[folder].files.push(f.rfilename);
            if (fileSizes[f.rfilename]) folderGroups[folder].totalSize += fileSizes[f.rfilename];
          } else {
            rootFiles.push({ filename: f.rfilename, size: fileSizes[f.rfilename] });
          }
        });
        const quantizations: GGUFQuantization[] = [];
        Object.entries(folderGroups).forEach(([folder, g]) => {
          const m = folder.match(/(Q\d+(?:_\d)?(?:_[KS])?(?:_[MSL])?|F(?:16|32)|IQ\d+(?:_[A-Z]+)?|BF16)/i);
          quantizations.push({ filename: folder, quantization: m ? m[1].toUpperCase() : folder, size: g.totalSize || undefined });
        });
        rootFiles.forEach(({ filename, size }) => {
          const m = filename.match(/[-._](Q\d+(?:_\d)?(?:_[KS])?(?:_[MSL])?|F(?:16|32)|IQ\d+(?:_[A-Z]+)?|BF16)(?:[-._]|\.gguf$)/i);
          if (m) quantizations.push({ filename, quantization: m[1].toUpperCase(), size });
        });
        if (quantizations.length > 0) {
          const priority: Record<string, number> = { Q4_K_M: 1, Q4_K_S: 2, Q5_K_M: 3, Q5_K_S: 4, Q6_K: 5, Q8_0: 6 };
          quantizations.sort((a, b) => (priority[a.quantization] ?? 100) - (priority[b.quantization] ?? 100));
          setHfModelBackends((prev: Record<string, DetectedBackend | null>) => ({
            ...prev,
            [modelId]: {
              recipe: 'llamacpp',
              label: 'GGUF',
              quantizations,
              mmprojFiles: mmprojFiles.length > 0 ? mmprojFiles : undefined,
            },
          }));
          if (!hfSelectedQuantizations[modelId]) {
            setHfSelectedQuantizations((prev: Record<string, string>) => ({ ...prev, [modelId]: quantizations[0].filename }));
            if (quantizations[0].size !== undefined) setHfModelSizes((prev: Record<string, number | undefined>) => ({ ...prev, [modelId]: quantizations[0].size }));
          }
          return;
        }
      }

      const totalFileSize = Object.values(fileSizes).reduce((a, b) => a + b, 0) || undefined;

      // FLM detection (FastFlowLM)
      if (modelId.toLowerCase().startsWith('fastflowlm/') || tags.includes('flm') || files.some(f => f.endsWith('.flm'))) {
        setHfModelBackends((prev: Record<string, DetectedBackend | null>) => ({ ...prev, [modelId]: { recipe: 'flm', label: 'FLM NPU' } }));
        if (totalFileSize) setHfModelSizes((prev: Record<string, number | undefined>) => ({ ...prev, [modelId]: totalFileSize }));
        return;
      }

      // ONNX detection
      if (files.some(f => f.endsWith('.onnx') || f.endsWith('.onnx_data'))) {
        const id = modelId.toLowerCase();
        let recipe = 'ryzenai-llm', label = 'ONNX CPU';
        if (id.includes('-ryzenai-npu') || tags.includes('npu')) { recipe = 'ryzenai-llm'; label = 'ONNX NPU'; }
        else if (id.includes('-ryzenai-hybrid') || tags.includes('hybrid')) { recipe = 'ryzenai-llm'; label = 'ONNX Hybrid'; }
        else if (tags.includes('igpu')) { recipe = 'ryzenai-llm'; label = 'ONNX iGPU'; }
        setHfModelBackends((prev: Record<string, DetectedBackend | null>) => ({ ...prev, [modelId]: { recipe, label } }));
        if (totalFileSize) setHfModelSizes((prev: Record<string, number | undefined>) => ({ ...prev, [modelId]: totalFileSize }));
        return;
      }

      // Whisper
      if ((tags.includes('whisper') || modelId.toLowerCase().includes('whisper')) && files.some(f => f.endsWith('.bin'))) {
        setHfModelBackends((prev: Record<string, DetectedBackend | null>) => ({ ...prev, [modelId]: { recipe: 'whispercpp', label: 'Whisper' } }));
        if (totalFileSize) setHfModelSizes((prev: Record<string, number | undefined>) => ({ ...prev, [modelId]: totalFileSize }));
        return;
      }

      // Stable Diffusion
      if (tags.includes('stable-diffusion') || tags.includes('text-to-image') || modelId.toLowerCase().includes('stable-diffusion') || modelId.toLowerCase().includes('flux')) {
        setHfModelBackends((prev: Record<string, DetectedBackend | null>) => ({ ...prev, [modelId]: { recipe: 'sd-cpp', label: 'SD.cpp' } }));
        if (totalFileSize) setHfModelSizes((prev: Record<string, number | undefined>) => ({ ...prev, [modelId]: totalFileSize }));
        return;
      }

      setHfModelBackends((prev: Record<string, DetectedBackend | null>) => ({ ...prev, [modelId]: null }));
    } catch {
      setHfModelBackends((prev: Record<string, DetectedBackend | null>) => ({ ...prev, [modelId]: null }));
    } finally {
      setDetectingBackendFor(null);
    }
  }, [hfModelBackends, hfSelectedQuantizations]);


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

  // Build a GGUF checkpoint using the extracted quant type (e.g. Q4_K_M), not the raw filename
  const resolveGgufCheckpoint = useCallback((modelId: string, backend: DetectedBackend): string => {
    const selectedFilename = hfSelectedQuantizations[modelId];
    if (!selectedFilename) return modelId;
    const quantObj = backend.quantizations?.find(q => q.filename === selectedFilename);
    return `${modelId}:${quantObj?.quantization ?? selectedFilename}`;
  }, [hfSelectedQuantizations]);

  const handleInstallHFModel = useCallback((hfModel: HFModelInfo) => {
    const backend = hfModelBackends[hfModel.id];
    if (!backend) return;
    const checkpoint = backend.recipe === 'llamacpp'
      ? resolveGgufCheckpoint(hfModel.id, backend)
      : hfModel.id;
    const modelName = `user.${hfModel.id.split('/').pop() ?? hfModel.id}`;
    handleDownloadModel(modelName, { checkpoint, recipe: backend.recipe });
  }, [hfModelBackends, resolveGgufCheckpoint, handleDownloadModel]);

  // Debounced HF search effect - to avoid HF API rate limit error
  useEffect(() => {
    if (currentView !== 'models') return;
    if (hfSearchTimeoutRef.current) clearTimeout(hfSearchTimeoutRef.current);
    if (searchQuery.trim().length >= 3) {
      hfSearchTimeoutRef.current = setTimeout(() => searchHuggingFace(searchQuery), 1200);
    } else {
      setHfSearchResults([]);
      setHfRateLimited(false);
    }
    return () => { if (hfSearchTimeoutRef.current) clearTimeout(hfSearchTimeoutRef.current); };
  }, [searchQuery, currentView, searchHuggingFace]);

  // Trigger backend detection for new HF results
  useEffect(() => {
    hfSearchResults.forEach((model: HFModelInfo) => {
      if (hfModelBackends[model.id] === undefined) detectBackend(model.id);
    });
  }, [hfSearchResults, hfModelBackends, detectBackend]);

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

  useEffect(() => {
    const handleInstallModel = (e: Event) => {
      const { name, registrationData } = (e as CustomEvent).detail;
      if (name) handleDownloadModel(name, registrationData);
    };
    const handleInstallFromJSON = (e: Event) => {
      const json = (e as CustomEvent).detail;
      if (json) uploadModelJSON(json);
    };
    window.addEventListener('installModel', handleInstallModel);
    window.addEventListener('installModelFromJSON', handleInstallFromJSON);
    return () => {
      window.removeEventListener('installModel', handleInstallModel);
      window.removeEventListener('installModelFromJSON', handleInstallFromJSON);
    };
  }, [handleDownloadModel]);

  const viewTitle = currentView === 'models'
    ? 'Model Manager'
    : currentView === 'backends'
      ? 'Backend Manager'
      : currentView === 'marketplace'
        ? 'Marketplace'
        : 'Settings';

  const searchPlaceholder = currentView === 'models'
    ? 'Search models...'
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
              {searchQuery.length > 0 && (
                <button
                  className="left-panel-inline-filter-btn"
                  onClick={() => setSearchQuery('')}
                  title="Clear search"
                  aria-label="Clear search"
                >
                  <XIcon size={13} strokeWidth={2} />
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
                  <div className="loaded-model-label">SUGGESTED MODELS</div>
                  <div className="loaded-model-count-pill">{availableModelCount} shown</div>
                </div>
                {renderModelsView()}
              </div>
            )}
            {currentView === 'models' && searchQuery.trim().length >= 3 && ( // Rendering the HF models by searching
              <div className="hf-search-section widget">
                <div className="available-models-header">
                  <div className="loaded-model-label">FROM HUGGING FACE</div>
                  {isSearchingHF && <span className="hf-search-spinner" />}
                </div>
                {hfRateLimited && (
                  <div className="hf-search-message">Rate limited — try again shortly.</div>
                )}
                {!hfRateLimited && !isSearchingHF && (
                  hfSearchResults.length === 0 ||
                  (hfSearchResults.length > 0 &&
                    detectingBackendFor === null &&
                    hfSearchResults.every((m: HFModelInfo) => hfModelBackends[m.id] === null))
                ) && (
                  <div className="hf-search-message">No compatible models found.</div>
                )}
                {hfSearchResults.filter((hfModel: HFModelInfo) => hfModelBackends[hfModel.id] !== null).map((hfModel: HFModelInfo) => {
                  const backend = hfModelBackends[hfModel.id];
                  const isDetecting = detectingBackendFor === hfModel.id;
                  const quants = backend?.quantizations ?? [];
                  const selectedQuant = hfSelectedQuantizations[hfModel.id];
                  const size = hfModelSizes[hfModel.id];
                  return (
                    <div key={hfModel.id} className="hf-model-item">
                      <div className="hf-model-left">
                        <span className="hf-model-name" title={hfModel.id}>{hfModel.id}</span>
                        {size !== undefined && <span className="hf-model-size">{formatSize(size / (1024 ** 3))}</span>}
                        <span className="hf-model-meta">↓ {formatDownloads(hfModel.downloads)}</span>
                        {isDetecting && <span className="hf-search-spinner" />}
                        <div className="hf-model-actions">
                          {!isDetecting && backend && (
                            <>
                              <button
                                className="model-action-btn edit-btn"
                                title="Edit before adding"
                                onClick={(e: React.MouseEvent) => {
                                  e.stopPropagation();
                                  const checkpoint = backend.recipe === 'llamacpp'
                                    ? resolveGgufCheckpoint(hfModel.id, backend)
                                    : hfModel.id;
                                  const idLower = hfModel.id.toLowerCase();
                                  window.dispatchEvent(new CustomEvent('openAddModel', {
                                    detail: {
                                      initialValues: {
                                        name: hfModel.id.split('/').pop() || hfModel.id,
                                        checkpoint,
                                        recipe: backend.recipe,
                                        mmprojOptions: backend.mmprojFiles,
                                        vision: (backend.mmprojFiles?.length ?? 0) > 0,
                                        reranking: idLower.includes('rerank'),
                                        embedding: idLower.includes('embed'),
                                      },
                                    },
                                  }));
                                }}
                              >
                                <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                  <path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7" />
                                  <path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z" />
                                </svg>
                              </button>
                              <button
                                className="model-action-btn download-btn"
                                title="Download from Hugging Face"
                                onClick={() => handleInstallHFModel(hfModel)}
                              >
                                <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                  <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                                  <polyline points="7 10 12 15 17 10" />
                                  <line x1="12" y1="15" x2="12" y2="3" />
                                </svg>
                              </button>
                            </>
                          )}
                        </div>
                      </div>
                      <div className="hf-model-right">
                        {!isDetecting && backend && quants.length > 1 && (
                          <select
                            className="hf-quant-select"
                            value={selectedQuant ?? ''}
                            onChange={(e: React.ChangeEvent<HTMLSelectElement>) => {
                              const q = quants.find((x: GGUFQuantization) => x.filename === e.target.value);
                              setHfSelectedQuantizations((prev: Record<string, string>) => ({ ...prev, [hfModel.id]: e.target.value }));
                              if (q?.size !== undefined) setHfModelSizes((prev: Record<string, number | undefined>) => ({ ...prev, [hfModel.id]: q.size }));
                            }}
                          >
                            {quants.map((q: GGUFQuantization) => (
                              <option key={q.filename} value={q.filename}>{q.quantization}</option>
                            ))}
                          </select>
                        )}
                        {!isDetecting && backend && <span className="hf-backend-badge">{backend.label}</span>}
                      </div>
                    </div>
                  );
                })}
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

        </div>}
      </div>
    </div>
  );
};

export default ModelManager;
