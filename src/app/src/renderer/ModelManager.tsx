import React, { useState, useEffect, useCallback, useRef, useMemo } from 'react';
import { Boxes, Brain, ChevronRight, Cpu, Eye, Flame, Layers, ListOrdered, Settings, SlidersHorizontal, Sparkles, SquareCode, Store, User, Wrench, XIcon } from './components/Icons';
import { ModelInfo, USER_MODEL_PREFIX } from './utils/modelData';
import { CANONICAL_PREFIXES, getModelDisplayName } from './utils/modelDisplayName';
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
import { COLLECTION_ROUTER_MODEL_RECIPE, RECIPE_DISPLAY_NAMES } from './utils/recipeNames';
import { EjectIcon, PinIcon } from './components/Icons';
import { getCollectionComponents, isCollectionFullyDownloaded, isCollectionModel, isModelEffectivelyDownloaded, isModelEffectivelyLoaded } from './utils/collectionModels';
import { getCollectionDisplayName, isCollectionEditableAsCustom } from './utils/customCollections';
import { mergeWithDefaultSettings } from './utils/appSettings';
import { tauriReady } from './tauriShim';

interface ModelFamily {
  displayName: string;
  regex: RegExp;
  recipe?: string;
}

const SIZE_TOKEN = String.raw`(\d+\.?\d*B(?:-A\d+\.?\d*B)?)`;
const FLM_SIZE_TOKEN = String.raw`(\d+\.?\d*[bm])`;

function buildFamilyRegex(prefix: string, suffix = '-GGUF$'): RegExp {
  return new RegExp(`^${prefix}-${SIZE_TOKEN}${suffix}`);
}

function buildRecipePrefixFamilyRegex(prefix: string): RegExp {
  return new RegExp(`^${prefix}-${SIZE_TOKEN}(?:$|[-_.])`);
}

function buildRecipeRemainderFamilyRegex(prefix: string): RegExp {
  return new RegExp(`^${prefix}-(.+)`);
}

function buildFlmFamilyRegex(prefix: string): RegExp {
  return new RegExp(`^${prefix}-${FLM_SIZE_TOKEN}-FLM$`);
}

const MODEL_FAMILIES: ModelFamily[] = [
  // Standardized family matching: capture *B or *B-A*B.
  {
    displayName: 'Bonsai',
    regex: buildRecipeRemainderFamilyRegex('Bonsai'),
    recipe: 'llamacpp',
  },
  {
    displayName: 'Gemma-4',
    regex: buildRecipeRemainderFamilyRegex('Gemma-4'),
    recipe: 'llamacpp',
  },
  {
    displayName: 'Qwen2.5-Omni',
    regex: buildRecipeRemainderFamilyRegex('Qwen2\\.5-Omni'),
    recipe: 'llamacpp',
  },
  {
    displayName: 'Qwen3-Instruct-2507',
    regex: buildFamilyRegex('Qwen3', '-Instruct-2507-GGUF$'),
  },
  {
    displayName: 'Qwen3.6',
    regex: buildRecipeRemainderFamilyRegex('Qwen3\\.6'),
    recipe: 'llamacpp',
  },
  {
    displayName: 'Qwen3',
    regex: buildRecipePrefixFamilyRegex('Qwen3'),
    recipe: 'llamacpp',
  },
  {
    displayName: 'Qwen3.5',
    regex: buildRecipePrefixFamilyRegex('Qwen3\\.5'),
    recipe: 'llamacpp',
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

const isUserDefinedModelName = (modelName: string): boolean => {
  return modelName.startsWith(USER_MODEL_PREFIX);
};

const getModelListItemSortName = (item: ModelListItem): string => {
  return item.type === 'family' ? item.family.displayName : getModelDisplayName(item.name);
};

const getModelListItemSortRank = (item: ModelListItem): number => {
  return item.type === 'model' && isUserDefinedModelName(item.name) ? 1 : 0;
};

const MODEL_LABEL_DISPLAY_ORDER = [
  'reasoning',
  'coding',
  'vision',
  'hot',
  'embeddings',
  'reranking',
  'tool-calling',
  'custom',
  'experience',
];

const sortModelLabelsForDisplay = (labels: string[]): string[] => {
  const order = new Map(MODEL_LABEL_DISPLAY_ORDER.map((label, index) => [label, index]));
  return [...labels].sort((a, b) => {
    const aOrder = order.get(a) ?? Number.MAX_SAFE_INTEGER;
    const bOrder = order.get(b) ?? Number.MAX_SAFE_INTEGER;
    return aOrder - bOrder;
  });
};

const ModalityIcon: React.FC<{ label: string; title: string }> = ({ label, title }) => {
  const size = 11;
  const strokeWidth = 2.2;
  const icon = (() => {
    switch (label) {
      case 'reasoning': return <Brain size={size} strokeWidth={strokeWidth} />;
      case 'coding': return <SquareCode size={size} strokeWidth={strokeWidth} />;
      case 'vision': return <Eye size={size} strokeWidth={strokeWidth} />;
      case 'hot': return <Flame size={size} strokeWidth={strokeWidth} />;
      case 'embeddings': return <Layers size={size} strokeWidth={strokeWidth} />;
      case 'reranking': return <ListOrdered size={size} strokeWidth={strokeWidth} />;
      case 'tool-calling': return <Wrench size={size} strokeWidth={strokeWidth} />;
      case 'custom': return <User size={size} strokeWidth={strokeWidth} />;
      case 'experience': return <Sparkles size={size} strokeWidth={strokeWidth} />;
      default: return null;
    }
  })();

  if (!icon) return null;

  return (
    <span className={`model-label-icon label-${label}`} title={title}>
      {icon}
    </span>
  );
};

// Types for Hugging Face API responses
interface HFModelInfo {
  id: string;
  author: string;
  downloads: number;
  likes: number;
  tags: string[];
  pipeline_tag?: string;
}

interface RegistrySearchResult {
  repository_id?: string;
  display_name?: string;
  source?: string;
  description?: string;
  tags?: string[];
  task?: string;
  downloads?: number;
  likes?: number;
  has_gguf?: boolean;
}

interface RegistrySearchResponse {
  results?: RegistrySearchResult[];
  error?: string | {
    message?: string;
    path?: string;
  };
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
  suggestedName?: string;
  quantizations?: GGUFQuantization[];
  mmprojFiles?: string[];
  suggestedLabels?: string[];
}

interface ModelScopeVariantCacheEntry {
  backend: DetectedBackend;
  selectedQuantization: string;
  size?: number;
}

interface ValidatedModelScopeResult extends ModelScopeVariantCacheEntry {
  model: HFModelInfo;
  order: number;
}

const MODELSCOPE_SEARCH_LIMIT = 14;
const MODELSCOPE_MAX_RESULTS = 10;
const MODELSCOPE_VALIDATION_CONCURRENCY = 4;
const MODELSCOPE_SEARCH_DEBOUNCE_MS = 400;

// Strip the canonical prefix (if any) to get the bare model name. Used for
// family-regex matching and family grouping.
const stripCanonicalPrefix = (modelName: string): string => {
  const match = CANONICAL_PREFIXES.find(p => modelName.startsWith(p.prefix));
  return match ? modelName.slice(match.prefix.length) : modelName;
};

const hasCanonicalPrefix = (modelName: string): boolean =>
  CANONICAL_PREFIXES.some(p => modelName.startsWith(p.prefix));

const getSourceSortRank = (modelName: string): number => {
  const match = CANONICAL_PREFIXES.find(p => modelName.startsWith(p.prefix));
  return match?.sourceRank ?? 0;
};

const stripSourceSuffix = (label: string): string => {
  const match = CANONICAL_PREFIXES.find(p => label.endsWith(p.suffix));
  return match ? label.slice(0, -match.suffix.length) : label;
};

const getFamilyMemberLabel = (modelName: string, family: ModelFamily): string => {
  const prefixInfo = CANONICAL_PREFIXES.find(p => modelName.startsWith(p.prefix));
  const bare = stripCanonicalPrefix(modelName);
  const relativeName = bare.startsWith(family.displayName)
    ? bare.slice(family.displayName.length).replace(/^[-_.]/, '')
    : bare;
  const label = relativeName.endsWith('-GGUF') ? relativeName.slice(0, -'-GGUF'.length) : relativeName;
  // Keep the source suffix on collapsed family rows so shadowed sources stay distinguishable.
  return label + (prefixInfo?.suffix ?? '');
};

function buildModelList(
  models: Array<{ name: string; info: ModelInfo }>
): ModelListItem[] {
  // Build family groups
  const consumed = new Set<string>();
  const familyItems: ModelListItem[] = [];

  for (const family of MODEL_FAMILIES) {
    const members: { label: string; name: string; info: ModelInfo }[] = [];
    for (const m of models) {
      if (consumed.has(m.name)) continue;
      // Cloud models are grouped under their provider and rendered with the
      // provider prefix stripped; keep them as flat individual rows rather
      // than folding them into local model families (whose labels assume the
      // bare/canonical-prefixed local naming, not "<provider>.<model>").
      if (m.info.recipe === 'cloud') continue;
      if (family.recipe && m.info.recipe !== family.recipe) continue;
      const match = family.regex.exec(stripCanonicalPrefix(m.name));
      if (match) {
        members.push({ label: getFamilyMemberLabel(m.name, family), name: m.name, info: m.info });
        consumed.add(m.name);
      }
    }
    if (members.length > 1) {
      members.sort((a, b) => {
        const baseLabelA = stripSourceSuffix(a.label);
        const baseLabelB = stripSourceSuffix(b.label);
        const sizeA = parseFloat(baseLabelA);
        const sizeB = parseFloat(baseLabelB);
        if (Number.isFinite(sizeA) && Number.isFinite(sizeB) && sizeA !== sizeB) return sizeA - sizeB;

        const baseCompare = baseLabelA.localeCompare(baseLabelB, undefined, { numeric: true });
        if (baseCompare !== 0) return baseCompare;

        const sourceCompare = getSourceSortRank(a.name) - getSourceSortRank(b.name);
        if (sourceCompare !== 0) return sourceCompare;

        return a.label.localeCompare(b.label, undefined, { numeric: true });
      });
      familyItems.push({ type: 'family', family, members });
    } else {
      members.forEach(m => consumed.delete(m.name));
    }
  }

  // Build individual items for non-consumed models
  const individualItems: ModelListItem[] = models
    .filter(m => !consumed.has(m.name))
    .map(m => ({ type: 'model' as const, name: m.name, info: m.info }));

  // Merge and sort alphabetically by display name. User-defined entries
  // stay below built-in entries inside each category, matching custom models.
  const allItems = [...familyItems, ...individualItems];
  allItems.sort((a, b) => {
    const rankDiff = getModelListItemSortRank(a) - getModelListItemSortRank(b);
    if (rankDiff !== 0) return rankDiff;
    return getModelListItemSortName(a).localeCompare(getModelListItemSortName(b)) || (
      a.type === 'model' && b.type === 'model' ? a.name.localeCompare(b.name) : 0
    );
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
  checkpoints?: Record<string, string>,
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
  const [pinnedModels, setPinnedModels] = useState<Set<string>>(new Set());
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

  // ModelScope mirrors the proven Hugging Face UX, but uses lemond for search
  // and variant discovery so endpoint/token configuration stays server-side.
  const [modelScopeSearchResults, setModelScopeSearchResults] = useState<HFModelInfo[]>([]);
  const [isSearchingModelScope, setIsSearchingModelScope] = useState(false);
  const [modelScopeRateLimited, setModelScopeRateLimited] = useState(false);
  const [modelScopeSearchError, setModelScopeSearchError] = useState<string | null>(null);
  const [modelScopeModelBackends, setModelScopeModelBackends] = useState<Record<string, DetectedBackend | null>>({});
  const [modelScopeSelectedQuantizations, setModelScopeSelectedQuantizations] = useState<Record<string, string>>({});
  const [modelScopeModelSizes, setModelScopeModelSizes] = useState<Record<string, number | undefined>>({});
  const modelScopeSearchTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const modelScopeSearchRequestRef = useRef(0);
  const modelScopeVariantCacheRef = useRef(new Map<string, ModelScopeVariantCacheEntry>());

  const updateCheckDoneRef = useRef(false);


  const { toasts, removeToast, showError, showSuccess, showWarning } = useToast();
  const { confirm, ConfirmDialog } = useConfirmDialog();

  useEffect(() => {
    const loadModelManagerSettings = async () => {
      try {
        await tauriReady;
        if (!window.api?.getSettings) return;
        const settings = mergeWithDefaultSettings(await window.api.getSettings());
        setShowDownloadedOnly(settings.modelManager.showDownloadedOnly);
      } catch (error) {
        console.error('Failed to load model manager settings:', error);
      }
    };

    loadModelManagerSettings();
  }, []);

  const updateShowDownloadedOnly = useCallback(async (checked: boolean) => {
    setShowDownloadedOnly(checked);
    setShowFilterPanel(false);

    try {
      await tauriReady;
      if (!window.api?.getSettings || !window.api?.saveSettings) return;
      const currentSettings = await window.api.getSettings();
      await window.api.saveSettings({
        ...currentSettings,
        modelManager: {
          ...currentSettings.modelManager,
          showDownloadedOnly: checked,
        },
      });
    } catch (error) {
      console.error('Failed to save model manager settings:', error);
    }
  }, []);

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

        // Extract pinned models from the all_models_loaded array
        const pinnedModelNames = new Set<string>(
          data.all_models_loaded
            .filter((model: any) => model.pinned === true)
            .map((model: any) => model.model_name)
        );
        setPinnedModels(pinnedModelNames);

        // Remove loaded models from loading state
        setLoadingModels(prev => {
          const newSet = new Set(prev);
          loadedModelNames.forEach(modelName => newSet.delete(modelName));
          return newSet;
        });

        // Once the server's background update check completes, refresh models
        // to pick up update_available flags
        if (data.update_check_done && !updateCheckDoneRef.current) {
          updateCheckDoneRef.current = true;
          refreshModels();
        }
      } else {
        setLoadedModels(new Set());
        setPinnedModels(new Set());
      }
    } catch (error) {
      setLoadedModels(new Set());
      setPinnedModels(new Set());
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

    // Hide ESRGAN upscaler models (managed via the Image Generation panel)
    filtered = filtered.filter(model => !model.info?.labels?.includes('upscaling'));

    // Filter by downloaded status
    if (showDownloadedOnly) {
      filtered = filtered.filter(model => isModelEffectivelyDownloaded(model.name, modelsData[model.name], modelsData));
    }

    // Filter by search query
    if (searchQuery.trim()) {
      const query = searchQuery.toLowerCase();
      filtered = filtered.filter(model =>
        model.name.toLowerCase().includes(query) ||
        getModelDisplayName(model.name).toLowerCase().includes(query)
      );
    }

    return filtered;
  };

  // Cloud models all share recipe='cloud', but each configured provider
  // should get its own bucket so adding a second provider produces a
  // second sub-heading rather than mixing into one. The bucket key for
  // a cloud model is `<provider>-cloud` (e.g. "fireworks-cloud"); falls
  // back to plain "cloud" if cloud_provider isn't on the entry yet.
  const recipeBucketKey = (info: ModelInfo): string => {
    const recipe = info.recipe || 'other';
    if (recipe !== 'cloud') return recipe;
    const provider = (info as { cloud_provider?: unknown }).cloud_provider;
    return typeof provider === 'string' && provider.length > 0
      ? `${provider}-cloud`
      : 'cloud';
  };

  const groupModelsByRecipe = () => {
    const grouped: { [key: string]: Array<{ name: string; info: ModelInfo }> } = {};
    const filteredModels = getFilteredModels();

    filteredModels.forEach(model => {
      const bucket = recipeBucketKey(model.info);
      if (!grouped[bucket]) {
        grouped[bucket] = [];
      }
      grouped[bucket].push(model);
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
    if (!isCollectionModel(info)) {
      return info.size;
    }
    const components = getCollectionComponents(info);
    if (components.length === 0) return info.size;
    const total = components.reduce((sum, component) => sum + (modelsData[component]?.size || 0), 0);
    return total > 0 ? total : info.size;
  };

  const getDisplayLabelsForModel = (modelName: string, info: ModelInfo): string[] => {
    if (isCollectionModel(info)) {
      // Experiences intentionally show a single, consistent legend marker.
      return ['collection'];
    }
    return (info.labels || []).filter((label): label is string => typeof label === 'string' && label.length > 0);
  };

  const getModelDownloadedState = (modelName: string, info: ModelInfo): boolean => {
    return isModelEffectivelyDownloaded(modelName, info, modelsData);
  };

  const getModelLoadedState = (modelName: string, info: ModelInfo): boolean => {
    return isModelEffectivelyLoaded(modelName, info, modelsData, loadedModels);
  };

  const getModelLoadingState = (modelName: string): boolean => {
    return loadingModels.has(modelName);
  };

  const getCategoryLabel = (category: string): string => {
    const labels: { [key: string]: string } = {
      'collection': 'Lemonade',
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

  // Proper-cased display names for known cloud providers. The provider
  // name in config is constrained to lowercase (so we have a single
  // canonical id used for env-var lookup, model prefix, etc.); this map
  // is the one place we restore camel/acronym casing for the UI.
  const PROVIDER_DISPLAY_NAMES: Record<string, string> = {
    'openai':     'OpenAI',
    'fireworks':  'Fireworks',
    'together':   'Together',
    'openrouter': 'OpenRouter',
    'groq':       'Groq',
    'deepinfra':  'DeepInfra',
    'mistral':    'Mistral',
    'mistralai':  'MistralAI',
    'anthropic':  'Anthropic',
    'cohere':     'Cohere',
  };

  const getDisplayLabel = (key: string): string => {
    if (organizationMode === 'recipe') {
      // Per-provider cloud buckets ("fireworks-cloud" -> "Fireworks") are
      // synthesised in recipeBucketKey and won't be in RECIPE_DISPLAY_NAMES,
      // so format them here. The bucket is labelled with the provider's
      // registered name (with camel/acronym casing restored) — no " Cloud"
      // suffix, matching how the model names themselves are prefixed.
      if (key.endsWith('-cloud') && key !== 'cloud') {
        const provider = key.slice(0, -'-cloud'.length);
        return PROVIDER_DISPLAY_NAMES[provider]
          ?? `${provider.charAt(0).toUpperCase()}${provider.slice(1)}`;
      }
      return RECIPE_DISPLAY_NAMES[key] || key;
    } else {
      return getCategoryLabel(key);
    }
  };

  // Merge loaded and loading models so the list shows components as they
  // start loading, not just after /health confirms them. Loading entries
  // get an `isLoading` flag so the UI can render a pending indicator.
  // Skip collection entries themselves — only show component models.
  const loadedModelEntries = (() => {
    const entries: Array<{ modelName: string; isLoading: boolean }> = [];
    const seen = new Set<string>();
    for (const modelName of loadedModels) {
      if (isCollectionModel(modelsData[modelName])) continue;
      if (seen.has(modelName)) continue;
      seen.add(modelName);
      entries.push({ modelName, isLoading: false });
    }
    for (const modelName of loadingModels) {
      if (isCollectionModel(modelsData[modelName])) continue;
      if (seen.has(modelName)) continue;
      seen.add(modelName);
      entries.push({ modelName, isLoading: true });
    }
    return entries.sort((a, b) =>
      getModelDisplayName(a.modelName).localeCompare(getModelDisplayName(b.modelName)) ||
      a.modelName.localeCompare(b.modelName)
    );
  })();



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
      const EXCLUDED_PIPELINE_TAGS = new Set([
        // Audio input/output
        'automatic-speech-recognition',
        'text-to-speech',
        'audio-text-to-text',
        'text-to-audio',
        'audio-to-audio',
        'voice-activity-detection',
        // Image/video input/output
        'text-to-image',
        'image-to-image',
        'image-to-video',
        'image-to-3d',
        'image-text-to-image',
        'image-text-to-video',
        'unconditional-image-generation',
        'image-segmentation',
        'object-detection',
        'depth-estimation',
        'mask-generation',
        'zero-shot-object-detection',
        // Video
        'text-to-video',
        'text-to-3d',
        'video-to-video'
      ]);
      const filteredData = ggufData.filter(m => !m.pipeline_tag || !EXCLUDED_PIPELINE_TAGS.has(m.pipeline_tag.toLowerCase()));
      setHfSearchResults(filteredData);
    } catch {
      setHfSearchResults([]);
    } finally {
      setIsSearchingHF(false);
    }
  }, []);

  const searchModelScope = useCallback(async (
    query: string,
    requestId: number,
    signal: AbortSignal,
  ) => {
    const normalizedQuery = query.trim();
    if (normalizedQuery.length < 3) return;

    setIsSearchingModelScope(true);
    setModelScopeRateLimited(false);
    setModelScopeSearchError(null);
    setModelScopeSearchResults([]);
    setModelScopeModelBackends({});
    setModelScopeSelectedQuantizations({});
    setModelScopeModelSizes({});

    try {
      const response = await serverFetch(
        `/registry/search?source=modelscope&query=${encodeURIComponent(normalizedQuery)}&limit=${MODELSCOPE_SEARCH_LIMIT}&format=gguf`,
        { signal },
      );

      let payload: RegistrySearchResponse = {};
      try {
        payload = await response.json();
      } catch {
        // Use the HTTP status below when the response body is not JSON.
      }

      if (!response.ok) {
        const errorPayload = payload.error;
        const baseMessage = typeof errorPayload === 'string'
          ? errorPayload
          : errorPayload?.message || `ModelScope search failed (${response.status})`;
        const path = typeof errorPayload === 'object' ? errorPayload?.path : undefined;
        const error = new Error(path ? `${baseMessage} [${path}]` : baseMessage) as Error & { status?: number };
        error.status = response.status;
        throw error;
      }

      if (requestId !== modelScopeSearchRequestRef.current) return;

      const deduplicated = new Map<string, HFModelInfo>();
      for (const result of Array.isArray(payload.results) ? payload.results : []) {
        const id = typeof result.repository_id === 'string' ? result.repository_id.trim() : '';
        const source = typeof result.source === 'string' ? result.source.toLowerCase() : 'modelscope';
        if (!id || (source !== 'modelscope' && source !== 'ms')) continue;
        if (!deduplicated.has(id)) {
          deduplicated.set(id, {
            id,
            author: id.split('/')[0] ?? '',
            downloads: typeof result.downloads === 'number' ? result.downloads : 0,
            likes: typeof result.likes === 'number' ? result.likes : 0,
            tags: Array.isArray(result.tags) ? result.tags : [],
            pipeline_tag: typeof result.task === 'string' ? result.task : undefined,
          });
        }
      }

      // The server performs provider-aware relevance ranking. Preserve that
      // order while the file-level compatibility checks finish asynchronously.
      const candidates = [...deduplicated.values()]
        .slice(0, MODELSCOPE_SEARCH_LIMIT)
        .map((model, order) => ({ model, order }));

      const validated: ValidatedModelScopeResult[] = [];
      const maxResults = MODELSCOPE_MAX_RESULTS;
      let nextCandidate = 0;

      const publishValidated = (): void => {
        if (requestId !== modelScopeSearchRequestRef.current) return;
        const ordered = [...validated]
          .sort((a, b) => a.order - b.order)
          .slice(0, maxResults);

        const backends: Record<string, DetectedBackend | null> = {};
        const selectedQuantizations: Record<string, string> = {};
        const sizes: Record<string, number | undefined> = {};
        for (const result of ordered) {
          backends[result.model.id] = result.backend;
          selectedQuantizations[result.model.id] = result.selectedQuantization;
          sizes[result.model.id] = result.size;
        }

        setModelScopeModelBackends(backends);
        setModelScopeSelectedQuantizations(selectedQuantizations);
        setModelScopeModelSizes(sizes);
        setModelScopeSearchResults(ordered.map(result => result.model));
        // Stop presenting the search as blocked once the first usable result is
        // available; remaining probes continue to fill the list incrementally.
        if (ordered.length > 0) setIsSearchingModelScope(false);
      };

      const validateCandidate = async (
        model: HFModelInfo,
        order: number,
      ): Promise<ValidatedModelScopeResult | null> => {
        const cached = modelScopeVariantCacheRef.current.get(model.id);
        if (cached) return { model, order, ...cached };

        const variantsRes = await serverFetch(
          `/pull/variants?source=modelscope&checkpoint=${encodeURIComponent(model.id)}`,
          { signal },
        );
        if (!variantsRes.ok) return null;

        const variantsPayload: {
          variants?: Array<{
            name: string;
            primary_file: string;
            files: string[];
            sharded: boolean;
            size_bytes: number;
          }>;
          mmproj_files?: string[];
          recipe?: string;
          suggested_name?: string;
          suggested_labels?: string[];
        } = await variantsRes.json();

        const variants = Array.isArray(variantsPayload.variants)
          ? variantsPayload.variants
          : [];
        if (variants.length === 0) return null;

        const quantizations: GGUFQuantization[] = variants.map(variant => ({
          filename: variant.name,
          quantization: variant.name,
          size: variant.size_bytes || undefined,
        }));
        const suggestedLabels = Array.isArray(variantsPayload.suggested_labels)
          ? variantsPayload.suggested_labels.filter(
              (label): label is string => typeof label === 'string'
            )
          : [];
        const firstVariant = quantizations[0];

        const cacheEntry: ModelScopeVariantCacheEntry = {
          selectedQuantization: firstVariant.filename,
          size: firstVariant.size,
          backend: {
            recipe: variantsPayload.recipe || 'llamacpp',
            label: 'GGUF',
            suggestedName: variantsPayload.suggested_name,
            quantizations,
            mmprojFiles: variantsPayload.mmproj_files?.length
              ? variantsPayload.mmproj_files
              : undefined,
            suggestedLabels,
          },
        };
        modelScopeVariantCacheRef.current.set(model.id, cacheEntry);
        return { model, order, ...cacheEntry };
      };

      const worker = async (): Promise<void> => {
        while (
          requestId === modelScopeSearchRequestRef.current &&
          !signal.aborted &&
          validated.length < maxResults &&
          nextCandidate < candidates.length
        ) {
          const candidate = candidates[nextCandidate++];
          try {
            const result = await validateCandidate(candidate.model, candidate.order);
            if (
              result &&
              requestId === modelScopeSearchRequestRef.current &&
              validated.length < maxResults &&
              !validated.some(item => item.model.id === result.model.id)
            ) {
              validated.push(result);
              // Publish immediately instead of waiting for every file-tree probe.
              // This is the main latency improvement for ModelScope search.
              publishValidated();
            }
          } catch {
            // Search metadata is only a candidate hint. Invalid or inaccessible
            // repositories are silently omitted from marketplace results.
          }
        }
      };

      await Promise.all(
        Array.from({
          length: Math.min(MODELSCOPE_VALIDATION_CONCURRENCY, candidates.length),
        }, () => worker())
      );
    } catch (error) {
      if (requestId !== modelScopeSearchRequestRef.current || signal.aborted) return;
      setModelScopeSearchResults([]);
      setModelScopeModelBackends({});
      setModelScopeSelectedQuantizations({});
      setModelScopeModelSizes({});
      const status = (error as Error & { status?: number }).status;
      if (status === 429) {
        setModelScopeRateLimited(true);
      } else {
        setModelScopeSearchError(
          error instanceof Error ? error.message : 'ModelScope search failed'
        );
      }
    } finally {
      if (requestId === modelScopeSearchRequestRef.current && !signal.aborted) {
        setIsSearchingModelScope(false);
      }
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

      // GGUF detection — delegate to the lemond /pull/variants endpoint so the
      // CLI and the app share one source of truth for variant enumeration,
      // mmproj detection, and label inference.
      const hasGguf = data.siblings.some(s => s.rfilename.toLowerCase().endsWith('.gguf'));
      if (hasGguf) {
        try {
          const variantsRes = await serverFetch(`/pull/variants?checkpoint=${encodeURIComponent(modelId)}`);
          if (variantsRes.ok) {
            const payload: {
              variants: { name: string; primary_file: string; files: string[]; sharded: boolean; size_bytes: number }[];
              mmproj_files: string[];
              recipe: string;
              suggested_name?: string;
              suggested_labels?: string[];
            } = await variantsRes.json();
            if (payload.variants && payload.variants.length > 0) {
              const quantizations: GGUFQuantization[] = payload.variants.map(v => ({
                filename: v.name,
                quantization: v.name,
                size: v.size_bytes || undefined,
              }));
              const suggestedLabels = Array.isArray(payload.suggested_labels)
                ? payload.suggested_labels.filter((label): label is string => typeof label === 'string')
                : [];
              setHfModelBackends((prev: Record<string, DetectedBackend | null>) => ({
                ...prev,
                [modelId]: {
                  recipe: payload.recipe || 'llamacpp',
                  label: 'GGUF',
                  suggestedName: payload.suggested_name,
                  quantizations,
                  mmprojFiles: payload.mmproj_files && payload.mmproj_files.length > 0 ? payload.mmproj_files : undefined,
                  suggestedLabels,
                },
              }));
              if (!hfSelectedQuantizations[modelId]) {
                setHfSelectedQuantizations((prev: Record<string, string>) => ({ ...prev, [modelId]: quantizations[0].filename }));
                if (quantizations[0].size !== undefined) setHfModelSizes((prev: Record<string, number | undefined>) => ({ ...prev, [modelId]: quantizations[0].size }));
              }
              return;
            }
          }
        } catch { /* fall through to backend detection below */ }
      }

      const totalFileSize = Object.values(fileSizes).reduce((a, b) => a + b, 0) || undefined;

      // FLM detection (FastFlowLM)
      if (modelId.toLowerCase().startsWith('fastflowlm/') || tags.includes('flm') || files.some(f => f.endsWith('.flm'))) {
        setHfModelBackends((prev: Record<string, DetectedBackend | null>) => ({ ...prev, [modelId]: { recipe: 'flm', label: 'FLM NPU' } }));
        if (totalFileSize) setHfModelSizes((prev: Record<string, number | undefined>) => ({ ...prev, [modelId]: totalFileSize }));
        return;
      }

      // Moonshine streaming STT — must run before the generic ONNX detection
      // (moonshine repos are ONNX-based and would be misclassified as ryzenai-llm)
      if (modelId.toLowerCase().includes('moonshine') && files.some(f => f.endsWith('.onnx'))) {
        setHfModelBackends((prev: Record<string, DetectedBackend | null>) => ({ ...prev, [modelId]: { recipe: 'moonshine', label: 'Moonshine' } }));
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

  const handleDownloadModel = useCallback(async (modelName: string, registrationData?: ModelRegistrationData, upgrade?: boolean) => {
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

      // Skip the download flow entirely if a collection is already complete
      const info = modelsData[modelName];
      if (isCollectionModel(info) && isCollectionFullyDownloaded(modelName, modelsData)) {
        showSuccess(`"${modelName}" is already downloaded.`);
        return;
      }

      // Do not start a second download if this renderer owns a live request or
      // the server-owned download registry reports one. A restored UI row alone
      // is not authoritative after reload, because it has no fetch stream or
      // AbortController.
      if (downloadTracker.isActive(modelName)) {
        showWarning(`Download for "${modelName}" is already in progress.`);
        return;
      }

      const serverDownloadActive = await downloadTracker.hasActiveServerDownload(modelName);
      if (serverDownloadActive) {
        showWarning(`Download for "${modelName}" is already in progress.`);
        return;
      }

      // If the only thing left is a stale renderer-local row, remove it so the
      // real /pull request can be sent and the server can resume from disk.
      downloadTracker.clearStaleModelDownload(modelName);

      // Add to loading state to show loading indicator
      setLoadingModels(prev => new Set(prev).add(modelName));

      const collectionComponents = isCollectionModel(info)
        ? getCollectionComponents(info)
        : undefined;

      // Use the single consolidated download function
      await pullModel(modelName, {
        registrationData,
        collectionComponents,
        declaredSizeGB: modelsData[modelName]?.size,
        upgrade,
      });

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

  const resolveHfModelName = useCallback((modelId: string, backend: DetectedBackend, checkpoint?: string): string => {
    const getOwnerSuffixName = (modelName: string): string => {
      const owner = modelId.split('/')[0]?.trim();
      if (!owner) return modelName;
      const safeOwner = owner.replace(/[^A-Za-z0-9_.-]+/g, '-').replace(/^-+|-+$/g, '');
      return safeOwner ? `${modelName}-${safeOwner}` : modelName;
    };
    const lookupModelInfo = (modelName: string): ModelInfo | undefined => {
      return modelsData[`user.${modelName}`] ?? modelsData[modelName];
    };

    const matchesCheckpoint = (info: ModelInfo | undefined): boolean => {
      if (!info || !checkpoint) return false;
      const registrySource = info.registry_source
        ?? (info.source === 'modelscope' || info.source === 'huggingface' ? info.source : 'huggingface');
      return registrySource === 'huggingface'
        && (info.checkpoint === checkpoint || info.checkpoints?.main === checkpoint);
    };

    const suggestedName = backend.suggestedName || modelId.split('/').pop() || modelId;
    let defaultName = suggestedName;

    if (backend.recipe === 'llamacpp') {
      const selectedFilename = hfSelectedQuantizations[modelId];
      if (selectedFilename) {
        const quantObj = backend.quantizations?.find(q => q.filename === selectedFilename);
        const variantName = quantObj?.quantization ?? selectedFilename;
        defaultName = `${suggestedName}-${variantName}`;
      }
    }

    if (!checkpoint) return defaultName;

    const existingDefault = lookupModelInfo(defaultName);
    if (!existingDefault || matchesCheckpoint(existingDefault)) {
      return defaultName;
    }

    const fallbackBase = getOwnerSuffixName(defaultName);
    let candidate = fallbackBase;
    let suffix = 2;
    let existingCandidate = lookupModelInfo(candidate);
    while (existingCandidate && !matchesCheckpoint(existingCandidate)) {
      candidate = `${fallbackBase}-${suffix}`;
      suffix += 1;
      existingCandidate = lookupModelInfo(candidate);
    }
    return candidate;
  }, [hfSelectedQuantizations, modelsData]);

  const handleInstallHFModel = useCallback((hfModel: HFModelInfo) => {
    const backend = hfModelBackends[hfModel.id];
    if (!backend) return;
    const checkpoint = backend.recipe === 'llamacpp'
      ? resolveGgufCheckpoint(hfModel.id, backend)
      : hfModel.id;
    const modelName = `user.${resolveHfModelName(hfModel.id, backend, checkpoint)}`;
    const labels = new Set(backend.suggestedLabels ?? []);
    const mmproj = backend.mmprojFiles?.[0];
    if (mmproj) labels.add('vision');
    handleDownloadModel(modelName, {
      checkpoint,
      recipe: backend.recipe,
      source: 'huggingface',
      mmproj,
      labels: Array.from(labels),
      vision: labels.has('vision'),
      embedding: labels.has('embeddings'),
      reranking: labels.has('reranking'),
    });
  }, [hfModelBackends, resolveGgufCheckpoint, resolveHfModelName, handleDownloadModel]);

  const resolveModelScopeGgufCheckpoint = useCallback((modelId: string, backend: DetectedBackend): string => {
    const selectedFilename = modelScopeSelectedQuantizations[modelId];
    if (!selectedFilename) return modelId;
    const quantObj = backend.quantizations?.find(q => q.filename === selectedFilename);
    return `${modelId}:${quantObj?.quantization ?? selectedFilename}`;
  }, [modelScopeSelectedQuantizations]);

  const resolveModelScopeModelName = useCallback((modelId: string, backend: DetectedBackend, checkpoint?: string): string => {
    const lookupModelInfo = (modelName: string): ModelInfo | undefined =>
      modelsData[`user.${modelName}`] ?? modelsData[modelName];

    const matchesModelScopeCheckpoint = (info: ModelInfo | undefined): boolean => {
      if (!info || !checkpoint) return false;
      const registrySource = info.registry_source
        ?? (info.source === 'modelscope' || info.source === 'huggingface' ? info.source : 'huggingface');
      return registrySource === 'modelscope'
        && (info.checkpoint === checkpoint || info.checkpoints?.main === checkpoint);
    };

    const suggestedName = backend.suggestedName || modelId.split('/').pop() || modelId;
    const selectedFilename = modelScopeSelectedQuantizations[modelId];
    const quantObj = backend.quantizations?.find(q => q.filename === selectedFilename);
    const defaultName = backend.recipe === 'llamacpp' && selectedFilename
      ? `${suggestedName}-${quantObj?.quantization ?? selectedFilename}`
      : suggestedName;

    const existingDefault = lookupModelInfo(defaultName);
    if (!existingDefault || matchesModelScopeCheckpoint(existingDefault)) return defaultName;

    // A mirrored HF repository may already own the default name. Keep the
    // ModelScope entry distinct while preserving the source+repository identity.
    const owner = modelId.split('/')[0]?.trim();
    const safeOwner = owner?.replace(/[^A-Za-z0-9_.-]+/g, '-').replace(/^-+|-+$/g, '');
    const fallbackBase = safeOwner ? `${defaultName}-${safeOwner}` : `${defaultName}-modelscope`;
    let candidate = fallbackBase;
    let suffix = 2;
    let existingCandidate = lookupModelInfo(candidate);
    while (existingCandidate && !matchesModelScopeCheckpoint(existingCandidate)) {
      candidate = `${fallbackBase}-${suffix}`;
      suffix += 1;
      existingCandidate = lookupModelInfo(candidate);
    }
    return candidate;
  }, [modelScopeSelectedQuantizations, modelsData]);

  const handleInstallModelScopeModel = useCallback((model: HFModelInfo) => {
    const backend = modelScopeModelBackends[model.id];
    if (!backend) return;

    const checkpoint = backend.recipe === 'llamacpp'
      ? resolveModelScopeGgufCheckpoint(model.id, backend)
      : model.id;
    const modelName = `user.${resolveModelScopeModelName(model.id, backend, checkpoint)}`;
    const labels = new Set(backend.suggestedLabels ?? []);
    const mmproj = backend.mmprojFiles?.[0];
    if (mmproj) labels.add('vision');

    handleDownloadModel(modelName, {
      checkpoint,
      recipe: backend.recipe,
      source: 'modelscope',
      mmproj,
      labels: Array.from(labels),
      vision: labels.has('vision'),
      embedding: labels.has('embeddings'),
      reranking: labels.has('reranking'),
    });
  }, [
    modelScopeModelBackends,
    resolveModelScopeGgufCheckpoint,
    resolveModelScopeModelName,
    handleDownloadModel,
  ]);

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

  // ModelScope follows the same query and placement as Hugging Face, but has
  // its own debounce and request generation so neither provider can cancel or
  // overwrite the other provider's state.
  useEffect(() => {
    if (modelScopeSearchTimeoutRef.current) {
      clearTimeout(modelScopeSearchTimeoutRef.current);
      modelScopeSearchTimeoutRef.current = null;
    }

    const requestId = ++modelScopeSearchRequestRef.current;

    if (currentView !== 'models') {
      setIsSearchingModelScope(false);
      setModelScopeSearchResults([]);
      setModelScopeModelBackends({});
      setModelScopeSelectedQuantizations({});
      setModelScopeModelSizes({});
      setModelScopeRateLimited(false);
      setModelScopeSearchError(null);
      return;
    }

    const query = searchQuery.trim();
    if (query.length >= 3) {
      setModelScopeSearchResults([]);
      setModelScopeModelBackends({});
      setModelScopeSelectedQuantizations({});
      setModelScopeModelSizes({});
      setModelScopeRateLimited(false);
      setModelScopeSearchError(null);
      // ModelScope is server-proxied and does not share Hugging Face's browser
      // rate limit, so a shorter debounce keeps the secondary provider responsive.
      setIsSearchingModelScope(true);
      const controller = new AbortController();
      modelScopeSearchTimeoutRef.current = setTimeout(() => {
        void searchModelScope(query, requestId, controller.signal);
      }, MODELSCOPE_SEARCH_DEBOUNCE_MS);

      return () => {
        controller.abort();
        if (modelScopeSearchTimeoutRef.current) {
          clearTimeout(modelScopeSearchTimeoutRef.current);
          modelScopeSearchTimeoutRef.current = null;
        }
      };
    } else {
      setIsSearchingModelScope(false);
      setModelScopeSearchResults([]);
      setModelScopeModelBackends({});
      setModelScopeSelectedQuantizations({});
      setModelScopeModelSizes({});
      setModelScopeRateLimited(false);
      setModelScopeSearchError(null);
    }

    return () => {
      if (modelScopeSearchTimeoutRef.current) {
        clearTimeout(modelScopeSearchTimeoutRef.current);
        modelScopeSearchTimeoutRef.current = null;
      }
    };
  }, [searchQuery, currentView, searchModelScope]);

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

      if (isCollectionModel(modelData)) {
        const components = getCollectionComponents(modelData);
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
        if (isCollectionModel(info)) {
          getCollectionComponents(info).forEach((component) => next.delete(component));
        }
        return next;
      });
      window.dispatchEvent(new CustomEvent('modelLoadEnd', { detail: { modelId: modelName } }));
    }
  };

  const handleUnloadModel = async (modelName: string) => {
    try {
      const modelData = modelsData[modelName];
      if (modelData && isCollectionModel(modelData)) {
        const components = getCollectionComponents(modelData);
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

  const handleTogglePin = async (modelName: string, pin: boolean) => {
    try {
      const response = await serverFetch('/internal/pin', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ model_name: modelName, pinned: pin })
      });
      if (!response.ok) {
        throw new Error(`Failed to pin model: ${response.statusText}`);
      }
      await fetchCurrentLoadedModel();
    } catch (error) {
      showError(`Failed to update pin: ${error instanceof Error ? error.message : 'Unknown'}`);
    }
  };

  const hasActiveDownloadForModel = (modelName: string): boolean => {
    const relatedNames = new Set<string>([modelName]);
    const info = modelsData[modelName];
    if (isCollectionModel(info)) {
      getCollectionComponents(info).forEach((component) => relatedNames.add(component));
    }

    return downloadTracker.getActiveDownloads().some((download) => {
      if (download.status === 'completed' || download.status === 'error') return false;
      const downloadNames = [download.modelName, ...(download.collectionComponents ?? [])];
      return downloadNames.some((name) => relatedNames.has(name));
    });
  };

  const handleDeleteModel = async (modelName: string) => {
    setHoveredModel(null);

    await downloadTracker.hydrateFromServer().catch(() => undefined);

    if (hasActiveDownloadForModel(modelName)) {
      showWarning('A download or setup involving this model is still active. Cancel or delete it from the Download Manager first.');
      return;
    }

    const info = modelsData[modelName];
    const collectionComponents = isCollectionModel(info) ? getCollectionComponents(info) : [];
    const isCollection = collectionComponents.length > 0;
    const displayName = isCollection ? getCollectionDisplayName(modelName) : getModelDisplayName(modelName);

    const message = isCollection
      ? `"${displayName}" is an Omni Model. Deleting it removes only the Omni Model entry. Its ${collectionComponents.length} component model${collectionComponents.length === 1 ? '' : 's'} stay on disk.`
      : `Are you sure you want to delete the model "${displayName}"? This action cannot be undone.`;

    const confirmed = await confirm({
      title: isCollection ? 'Delete Omni Model' : 'Delete Model',
      message,
      confirmText: 'Delete',
      cancelText: 'Cancel',
      danger: true
    });

    if (!confirmed) {
      return;
    }

    if (hasActiveDownloadForModel(modelName)) {
      showWarning('A download or setup involving this model is still active. Cancel or delete it from the Download Manager first.');
      return;
    }

    try {
      await deleteModel(modelName);
      showSuccess(isCollection
        ? `Omni Model "${displayName}" deleted. Component models were kept.`
        : `Model "${displayName}" deleted successfully.`);
      await fetchCurrentLoadedModel();
    } catch (error) {
      console.error('Error deleting model:', error);
      showError('Failed to delete model: ' + (error instanceof Error ? error.message : 'Unknown error'));
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
    const info = modelsData[modelName];
    // collections: downloaded when all components are downloaded,
    // loaded when all components are loaded. Fall back to naive checks
    // for regular models.
    const isDownloaded = isModelEffectivelyDownloaded(modelName, info, modelsData);
    const isLoaded = isModelEffectivelyLoaded(modelName, info, modelsData, loadedModels);
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

  const gearIcon = (
    <svg width="12" height="12" viewBox="0 0 16 16" fill="none" xmlns="http://www.w3.org/2000/svg">
      <path
        d="M6.5 1.5H9.5L9.9 3.4C10.4 3.6 10.9 3.9 11.3 4.2L13.1 3.5L14.6 6L13.1 7.4C13.2 7.9 13.2 8.1 13.2 8.5C13.2 8.9 13.2 9.1 13.1 9.6L14.6 11L13.1 13.5L11.3 12.8C10.9 13.1 10.4 13.4 9.9 13.6L9.5 15.5H6.5L6.1 13.6C5.6 13.4 5.1 13.1 4.7 12.8L2.9 13.5L1.4 11L2.9 9.6C2.8 9.1 2.8 8.9 2.8 8.5C2.8 8.1 2.8 7.9 2.9 7.4L1.4 6L2.9 3.5L4.7 4.2C5.1 3.9 5.6 3.6 6.1 3.4L6.5 1.5Z"
        stroke="currentColor" strokeWidth="1.2" strokeLinecap="round" strokeLinejoin="round"/>
      <circle cx="8" cy="8.5" r="2.5" stroke="currentColor" strokeWidth="1.2"/>
    </svg>
  );

  const renderCustomCollectionOptionsButton = (modelName: string) => {
    const recipe = modelsData[modelName]?.recipe;
    const isRouter = recipe === COLLECTION_ROUTER_MODEL_RECIPE;
    return (
      <button
        className="model-action-btn load-btn"
        onClick={(e) => {
          e.stopPropagation();
          const event = isRouter ? 'editRouterCollection' : 'editCustomCollection';
          window.dispatchEvent(new CustomEvent(event, { detail: { collectionId: modelName } }));
        }}
        title={isRouter ? 'Hybrid Router options' : 'Omni Model options'}
      >
        {gearIcon}
      </button>
    );
  };

  const renderDeleteButton = (modelName: string, title = 'Delete model') => {
    const blockedByDownload = hasActiveDownloadForModel(modelName);
    return (
      <button
        className="model-action-btn delete-btn"
        onClick={(e) => {
          e.stopPropagation();
          if (blockedByDownload) {
            showWarning('A download or setup involving this model is still active. Cancel or delete it from the Download Manager first.');
            return;
          }
          handleDeleteModel(modelName);
        }}
        title={blockedByDownload ? 'Cancel or delete the active download first' : title}
        disabled={blockedByDownload}
      >
        <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
          <polyline points="3 6 5 6 21 6" />
          <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
        </svg>
      </button>
    );
  };

  const renderActionButtonsContent = (modelName: string) => {
    const { isDownloaded, isLoaded, isLoading } = getModelStatus(modelName);
    const info = modelsData[modelName];
    const isUpscaling = info?.labels?.includes('upscaling');
    const hasUpdate = info?.update_available === true;
    const isCollection = isCollectionModel(info);
    // Cloud-recipe rows have no local artifact (Delete is meaningless and
    // dynamic discovery would re-add anyway) and no per-model knobs the
    // ModelOptionsModal can edit (provider config lives in the Backends
    // panel). Show Load / Unload only.
    const isCloud = info?.recipe === 'cloud';
    const isEditableCollection = isCollectionEditableAsCustom(info);
    const isBuiltInCollection = isCollection && info?.suggested === true &&
      !(info?.labels ?? []).includes('custom') &&
      !modelName.startsWith(USER_MODEL_PREFIX) &&
      info?.source !== 'user' && info?.source !== 'user_models' && info?.source !== 'custom';
    const canDeleteFromRow = !isCollection || !isBuiltInCollection;
    return (
      <>
        {!isDownloaded && (
          <>
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
            {canDeleteFromRow && renderDeleteButton(modelName, isCollection ? 'Delete Omni Model' : 'Delete model')}
            {isEditableCollection && renderCustomCollectionOptionsButton(modelName)}
          </>
        )}
        {isDownloaded && hasUpdate && (
          <button
            className="model-action-btn update-btn"
            onClick={(e) => { e.stopPropagation(); handleDownloadModel(modelName, undefined, true); }}
            title="Update available — click to re-download"
          >
            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
              <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
              <polyline points="7 10 12 15 17 10" />
              <line x1="12" y1="15" x2="12" y2="3" />
            </svg>
          </button>
        )}
        {isDownloaded && !isLoaded && !isLoading && isUpscaling && (
          <>
            {renderDeleteButton(modelName)}
          </>
        )}
        {isDownloaded && !isLoaded && !isLoading && !isUpscaling && (
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
            {canDeleteFromRow && !isCloud && renderDeleteButton(modelName, isCollection ? 'Delete Omni Model' : 'Delete model')}
            {isEditableCollection && renderCustomCollectionOptionsButton(modelName)}
            {!isCloud && !isCollection && renderLoadOptionsButton(modelName)}
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
            {canDeleteFromRow && !isCloud && renderDeleteButton(modelName, isCollection ? 'Delete Omni Model' : 'Delete model')}
            {isEditableCollection && renderCustomCollectionOptionsButton(modelName)}
            {!isCloud && !isCollection && renderLoadOptionsButton(modelName)}
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

    // For collections, show the component list (with sizes) as a
    // tooltip so users can see what gets downloaded/loaded.
    let nameTooltip: string | undefined;
    if (isCollectionModel(modelInfo)) {
      const components = getCollectionComponents(modelInfo);
      const lines = components.map((c) => {
        const s = modelsData[c]?.size;
        return s ? `• ${c} (${s.toFixed(1)} GB)` : `• ${c}`;
      });
      nameTooltip = `Omni Model with ${components.length} component models:\n${lines.join('\n')}`;
    } else if (displayName || getModelDisplayName(modelName, modelInfo) !== modelName) {
      nameTooltip = modelName;
    }

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
            <span className="model-name" title={nameTooltip}>{displayName ?? (isCollectionModel(modelInfo) ? getCollectionDisplayName(modelName) : getModelDisplayName(modelName, modelInfo))}</span>
            {modelInfo.recipe !== 'cloud' && (
              <span className="model-size">{formatSize(getModelSize(modelName, modelInfo))}</span>
            )}
            {renderActionButtons(modelName, isHovered)}
          </div>
          {modelInfo.labels && modelInfo.labels.length > 0 && (
            <span className="model-labels">
              {sortModelLabelsForDisplay(modelInfo.labels).map(label => (
                <ModalityIcon key={label} label={label} title={getCategoryLabel(label)} />
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
              {sortModelLabelsForDisplay(sharedLabels).map(label => (
                <ModalityIcon key={label} label={label} title={getCategoryLabel(label)} />
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
    if (!info || info.state === 'installed' || info.state === 'update_available') return null;
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

  const compatibleHfSearchResults = hfSearchResults.filter((model: HFModelInfo) => {
    const backend = hfModelBackends[model.id];
    return backend !== null && !(backend != null && ['sd-cpp', 'whispercpp', 'moonshine'].includes(backend.recipe));
  });

  const noCompatibleModelScopeResults = !isSearchingModelScope
    && !modelScopeRateLimited
    && !modelScopeSearchError
    && modelScopeSearchResults.length === 0;

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
              <>
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
              </>
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
                        updateShowDownloadedOnly(e.target.checked);
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
                <div className="loaded-model-count-pill">
                  {loadedModelEntries.filter(e => !e.isLoading).length} loaded
                </div>
              </div>
              {loadedModelEntries.length === 0 && <div className="loaded-model-empty">No models loaded</div>}
              <div className="loaded-model-list">
                {loadedModelEntries.map(({ modelName, isLoading }) => (
                  <div key={modelName} className="loaded-model-info">
                    <div className="loaded-model-details">
                      <span
                        className={`loaded-model-indicator${isLoading ? ' loading' : ''}`}
                        title={isLoading ? 'Loading' : 'Loaded'}
                      />
                      <span className="loaded-model-name" title={modelName}>{getModelDisplayName(modelName)}</span>
                    </div>
                    {!isLoading && (
                      <div className="active-model-actions">
                        <button
                          className={`model-action-btn pin-btn ${pinnedModels.has(modelName) ? 'pinned' : ''}`}
                          onClick={() => handleTogglePin(modelName, !pinnedModels.has(modelName))}
                          title={pinnedModels.has(modelName) ? "Unpin model" : "Pin model"}
                        >
                          <PinIcon fill={pinnedModels.has(modelName) ? 'currentColor' : 'none'} />
                        </button>
                        <button className="model-action-btn unload-btn active-model-eject-button" onClick={() => handleUnloadModel(modelName)} title="Eject model">
                          <EjectIcon />
                        </button>
                      </div>
                    )}
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
            {currentView === 'models' && searchQuery.trim().length >= 3 && (
              <div className="hf-search-section widget registry-search-section">
                <section className="registry-provider-group" aria-label="Hugging Face search results">
                  <div className="available-models-header registry-provider-header">
                    <div className="loaded-model-label">FROM HUGGING FACE</div>
                    {isSearchingHF && <span className="hf-search-spinner" />}
                  </div>
                  {hfRateLimited && (
                    <div className="hf-search-message">Rate limited — try again shortly.</div>
                  )}
                  {!hfRateLimited && !isSearchingHF && compatibleHfSearchResults.length === 0 && (
                    <div className="hf-search-message">No compatible models found.</div>
                  )}
                  <div className="registry-provider-results">
                    {compatibleHfSearchResults.map((hfModel: HFModelInfo) => {
                    const backend = hfModelBackends[hfModel.id];
                    const isDetecting = detectingBackendFor === hfModel.id;
                    const quants = backend?.quantizations ?? [];
                    const selectedQuant = hfSelectedQuantizations[hfModel.id];
                    const size = hfModelSizes[hfModel.id];
                    return (
                      <div key={hfModel.id} className="hf-model-item">
                        <div className="hf-model-left">
                          <span className="hf-model-name" title={hfModel.id}>{hfModel.id}</span>
                          <span className="registry-result-source-badge huggingface" title="Hugging Face">HF</span>
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
                                    const labels = backend.suggestedLabels ?? [];
                                    window.dispatchEvent(new CustomEvent('openAddModel', {
                                      detail: {
                                        initialValues: {
                                          name: resolveHfModelName(hfModel.id, backend, checkpoint),
                                          checkpoint,
                                          recipe: backend.recipe,
                                          source: 'huggingface',
                                          mmprojOptions: backend.mmprojFiles,
                                          labels,
                                          vision: labels.includes('vision') || (backend.mmprojFiles?.length ?? 0) > 0,
                                          reranking: labels.includes('reranking') || idLower.includes('rerank'),
                                          embedding: labels.includes('embeddings') || idLower.includes('embed'),
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
                </section>

                <section className="registry-provider-group modelscope-provider-group" aria-label="ModelScope search results">
                  <div className="available-models-header registry-provider-header modelscope-provider-header">
                    <div className="loaded-model-label">FROM MODELSCOPE</div>
                    {isSearchingModelScope && <span className="hf-search-spinner" />}
                  </div>
                  {modelScopeRateLimited && (
                    <div className="hf-search-message">Rate limited — try again shortly.</div>
                  )}
                  {!modelScopeRateLimited && modelScopeSearchError && (
                    <div className="hf-search-message">{modelScopeSearchError}</div>
                  )}
                  {noCompatibleModelScopeResults && (
                    <div className="hf-search-message">No compatible models found.</div>
                  )}
                  <div className="registry-provider-results">
                    {modelScopeSearchResults.map((model: HFModelInfo) => {
                      const backend = modelScopeModelBackends[model.id];
                      if (!backend) return null;
                      const quants = backend.quantizations ?? [];
                      const selectedQuant = modelScopeSelectedQuantizations[model.id];
                      const size = modelScopeModelSizes[model.id];
                      return (
                        <div key={`modelscope:${model.id}`} className="hf-model-item">
                          <div className="hf-model-left">
                            <span className="hf-model-name" title={model.id}>{model.id}</span>
                            <span className="registry-result-source-badge modelscope" title="ModelScope">MS</span>
                            {size !== undefined && <span className="hf-model-size">{formatSize(size / (1024 ** 3))}</span>}
                            <span className="hf-model-meta">↓ {formatDownloads(model.downloads)}</span>
                            <div className="hf-model-actions">
                              <button
                                className="model-action-btn edit-btn"
                                title="Edit before adding"
                                onClick={(e: React.MouseEvent) => {
                                  e.stopPropagation();
                                  const checkpoint = backend.recipe === 'llamacpp'
                                    ? resolveModelScopeGgufCheckpoint(model.id, backend)
                                    : model.id;
                                  const labels = backend.suggestedLabels ?? [];
                                  const idLower = model.id.toLowerCase();
                                  window.dispatchEvent(new CustomEvent('openAddModel', {
                                    detail: {
                                      initialValues: {
                                        name: resolveModelScopeModelName(model.id, backend, checkpoint),
                                        checkpoint,
                                        recipe: backend.recipe,
                                        source: 'modelscope',
                                        mmprojOptions: backend.mmprojFiles,
                                        labels,
                                        vision: labels.includes('vision') || (backend.mmprojFiles?.length ?? 0) > 0,
                                        reranking: labels.includes('reranking') || idLower.includes('rerank'),
                                        embedding: labels.includes('embeddings') || idLower.includes('embed'),
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
                                title="Download from ModelScope"
                                onClick={() => handleInstallModelScopeModel(model)}
                              >
                                <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                                  <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                                  <polyline points="7 10 12 15 17 10" />
                                  <line x1="12" y1="15" x2="12" y2="3" />
                                </svg>
                              </button>
                            </div>
                          </div>
                          <div className="hf-model-right">
                            {quants.length > 1 && (
                              <select
                                className="hf-quant-select"
                                value={selectedQuant ?? ''}
                                onChange={(e: React.ChangeEvent<HTMLSelectElement>) => {
                                  const quant = quants.find((item: GGUFQuantization) => item.filename === e.target.value);
                                  setModelScopeSelectedQuantizations(prev => ({ ...prev, [model.id]: e.target.value }));
                                  if (quant?.size !== undefined) {
                                    setModelScopeModelSizes(prev => ({ ...prev, [model.id]: quant.size }));
                                  }
                                }}
                              >
                                {quants.map((quant: GGUFQuantization) => (
                                  <option key={quant.filename} value={quant.filename}>{quant.quantization}</option>
                                ))}
                              </select>
                            )}
                            <span className="hf-backend-badge">{backend.label}</span>
                          </div>
                        </div>
                      );
                    })}
                  </div>
                </section>
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
                showWarning={showWarning}
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
