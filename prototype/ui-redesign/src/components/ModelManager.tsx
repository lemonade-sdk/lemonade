import React, { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import api, { ModelInfo, LoadedModel, PullCallbacks, PullVariantsResult, HFModelResult, searchHuggingFace, friendlyErrorMessage, DownloadProgressEvent } from '../api';
import { canSelectInComposer, capabilityFromLoaded, capabilityFromModelInfo, capabilityIcon, capabilityLabel } from '../modelCapabilities';
import type { AccountSession } from '../features/accounts/accountStore';
import { CUSTOM_CAPABILITIES, CustomModelCapability, customLoadOptions, customModelToModelInfo, customRegistrationOptions, deleteCustomModel, loadCustomModels, upsertCustomModel } from '../features/customModels/customModelStore';
import { collectionComponentLabel, getCollectionComponents, isCollectionModel, isCollectionFullyDownloaded, withVirtualLoadedCollections } from '../features/collections/collectionModels';

/* ── Helpers ─────────────────────────────────────────────────── */

function formatSize(gb: number): string {
  if (gb >= 1) return `${gb.toFixed(1)} GB`;
  if (gb >= 0.01) return `${(gb * 1000).toFixed(0)} MB`;
  return `< 1 MB`;
}

function modelName(m: ModelInfo): string {
  return (m as any).model_name || m.name || m.id;
}

function recipeIcon(recipe: string): string {
  switch (recipe) {
    case 'llamacpp': return '🦙';
    case 'vllm': return '🚀';
    case 'flm': return '⚡';
    case 'ryzenai-llm': return '🔶';
    case 'sd-cpp': return '🎨';
    case 'whispercpp': return '🎤';
    case 'kokoro': return '🔊';
    case 'collection.omni': return '✦';
    case 'collection': return '📦';
    default: return '🤖';
  }
}

function recipeLabel(recipe: string): string {
  switch (recipe) {
    case 'llamacpp': return 'llama.cpp';
    case 'vllm': return 'vLLM';
    case 'flm': return 'FastFlowLM';
    case 'ryzenai-llm': return 'RyzenAI';
    case 'sd-cpp': return 'Stable Diffusion';
    case 'whispercpp': return 'Whisper';
    case 'kokoro': return 'Kokoro TTS';
    case 'collection.omni': return 'Omni Collection';
    case 'collection': return 'Collection';
    default: return recipe;
  }
}

function modelType(m: ModelInfo): string {
  const cap = capabilityFromModelInfo(m);
  return cap === 'chat' || cap === 'unknown' ? 'llm' : cap;
}

function typeColor(type: string): string {
  switch (type) {
    case 'llm': return 'var(--accent)';
    case 'omni': return '#facc15';
    case 'image': return '#c084fc';
    case 'audio': return '#60a5fa';
    case 'tts': return '#34d399';
    case 'embedding': return '#f97316';
    case 'reranking': return '#f43f5e';
    default: return 'var(--text-tertiary)';
  }
}

function labelDisplay(label: string): string {
  const map: Record<string, string> = {
    'tool-calling': '🔧 Tools',
    'vision': '👁 Vision',
    'omni': '✦ Omni',
    'multimodal': '✦ Multimodal',
    'vision-language': '✦ Vision Language',
    'reasoning': '🧠 Reasoning',
    'coding': '💻 Code',
    'hot': '🔥 Popular',
    'mtp': '🚀 MTP',
    'embeddings': '📐 Embeddings',
    'reranking': '🔀 Reranking',
    'transcription': '🎤 Transcription',
    'realtime-transcription': '🎙 Realtime',
    'chat-transcription': '💬 Chat ASR',
    'tts': '🔊 TTS',
    'image': '🎨 Image',
    'edit': '✏️ Edit',
    'upscaling': '🔍 Upscale',
    'custom': '⚙️ Custom',
  };
  return map[label] || label;
}

function hfUrl(checkpoint: string): string | null {
  if (!checkpoint) return null;
  const parts = checkpoint.split(':')[0];
  if (!parts.includes('/')) return null;
  return `https://huggingface.co/${parts}`;
}

function formatDownloads(n: number): string {
  if (n >= 1_000_000) return `${(n / 1_000_000).toFixed(1)}M`;
  if (n >= 1_000) return `${(n / 1_000).toFixed(1)}K`;
  return String(n);
}

function formatBytes(bytes: number): string {
  if (bytes >= 1_073_741_824) return `${(bytes / 1_073_741_824).toFixed(1)} GB`;
  if (bytes >= 1_048_576) return `${(bytes / 1_048_576).toFixed(0)} MB`;
  if (bytes >= 1_024) return `${(bytes / 1_024).toFixed(0)} KB`;
  return `${bytes} B`;
}


async function copyTextToClipboard(text: string): Promise<void> {
  if (!text) return;
  if (navigator.clipboard?.writeText) {
    await navigator.clipboard.writeText(text);
    return;
  }
  const textarea = document.createElement('textarea');
  textarea.value = text;
  textarea.setAttribute('readonly', 'true');
  textarea.style.position = 'fixed';
  textarea.style.opacity = '0';
  textarea.style.pointerEvents = 'none';
  document.body.appendChild(textarea);
  textarea.select();
  document.execCommand('copy');
  textarea.remove();
}

const CopyInlineButton: React.FC<{ text: string; title?: string }> = ({ text, title = 'Copy model name' }) => {
  const [copied, setCopied] = useState(false);
  const handleClick = async (event: React.MouseEvent<HTMLButtonElement>) => {
    event.preventDefault();
    event.stopPropagation();
    try {
      await copyTextToClipboard(text);
      setCopied(true);
      window.setTimeout(() => setCopied(false), 1200);
    } catch {
      setCopied(false);
    }
  };
  return (
    <button
      type="button"
      className={`copy-inline${copied ? ' copy-inline--copied' : ''}`}
      onClick={handleClick}
      title={copied ? 'Copied' : title}
      aria-label={copied ? 'Copied' : title}
    >
      {copied ? '✓' : '⧉'}
    </button>
  );
};

const RECIPE_BADGES: Record<string, string> = {
  llamacpp: '🦙 llama.cpp',
  vllm: '🚀 vLLM',
  'ryzenai-llm': '🔷 RyzenAI',
  'collection.omni': '✦ Omni Collection',
};

/* ── Filter / search types ─────────────────────────────────── */

type FilterTab = 'all' | 'llm' | 'omni' | 'image' | 'audio' | 'tts' | 'embedding';
type CustomFormMode = 'model' | 'omni-collection';
type OmniComponentRole = 'llm' | 'vision' | 'image' | 'edit' | 'transcription' | 'speech';
type CustomModelDraftState = {
  name: string;
  displayName: string;
  checkpoint: string;
  recipe: string;
  capability: CustomModelCapability;
  maxContextWindow: string;
  labels: string;
  omniSource: 'single' | 'collection';
  llmComponent: string;
  visionComponent: string;
  imageComponent: string;
  editComponent: string;
  transcriptionComponent: string;
  speechComponent: string;
};

const FILTER_TABS: { key: FilterTab; label: string; icon: string }[] = [
  { key: 'all', label: 'All', icon: '✦' },
  { key: 'llm', label: 'LLM', icon: '💬' },
  { key: 'omni', label: 'Omni', icon: '✦' },
  { key: 'image', label: 'Image', icon: '🎨' },
  { key: 'audio', label: 'Audio', icon: '🎤' },
  { key: 'tts', label: 'TTS', icon: '🔊' },
  { key: 'embedding', label: 'Embed', icon: '📐' },
];

function createEmptyCustomDraft(mode: CustomFormMode = 'model'): CustomModelDraftState {
  const isOmniCollection = mode === 'omni-collection';
  return {
    name: '',
    displayName: '',
    checkpoint: '',
    recipe: isOmniCollection ? 'collection.omni' : 'llamacpp',
    capability: isOmniCollection ? 'omni' : 'chat',
    maxContextWindow: '4096',
    labels: '',
    omniSource: isOmniCollection ? 'collection' : 'single',
    llmComponent: '',
    visionComponent: '',
    imageComponent: '',
    editComponent: '',
    transcriptionComponent: '',
    speechComponent: '',
  };
}

function loadedIsVirtualOmniCollection(model: LoadedModel): boolean {
  const recipe = String(model.recipe || '').toLowerCase();
  const components = model.recipe_options?.components;
  return (recipe === 'collection.omni' || recipe === 'collection')
    && model.recipe_options?.virtual_collection === true
    && Array.isArray(components)
    && components.some(component => typeof component === 'string' && component.trim().length > 0);
}

type OmniComponentOptionSource = 'custom' | 'downloaded' | 'registered';

interface OmniComponentOption {
  id: string;
  label: string;
  detail: string;
  source: OmniComponentOptionSource;
  downloaded: boolean;
  custom: boolean;
  recipe: string;
  labels: string[];
}

const OMNI_COMPONENT_ROLE_CONFIG: Record<OmniComponentRole, { label: string; placeholder: string; help: string; required?: boolean }> = {
  llm: {
    label: 'Planner LLM',
    placeholder: 'Search downloaded, registry, or custom LLMs…',
    help: 'Required planner model for chat and tool calls.',
    required: true,
  },
  vision: {
    label: 'Vision',
    placeholder: 'Search vision/VLM components…',
    help: 'Optional model used for image analysis.',
  },
  image: {
    label: 'Image generation',
    placeholder: 'Search image generation components…',
    help: 'Optional model used to generate images.',
  },
  edit: {
    label: 'Image editing',
    placeholder: 'Search image edit components…',
    help: 'Optional model used to edit existing images.',
  },
  transcription: {
    label: 'Transcription',
    placeholder: 'Search Whisper/audio components…',
    help: 'Optional speech-to-text model.',
  },
  speech: {
    label: 'Text to speech',
    placeholder: 'Search TTS/speech components…',
    help: 'Optional text-to-speech model.',
  },
};

const NON_PLANNER_LABELS = new Set(['image', 'image-generation', 'edit', 'upscaling', 'speech', 'tts', 'text-to-speech', 'transcription', 'embeddings', 'embedding', 'reranking', 'reranker']);

function lowerLabels(m: ModelInfo): string[] {
  return (m.labels || []).map(label => label.toLowerCase().trim()).filter(Boolean);
}

function hasAnyLabel(m: ModelInfo, labels: string[]): boolean {
  const wanted = new Set(labels.map(label => label.toLowerCase()));
  return lowerLabels(m).some(label => wanted.has(label));
}

function isOmniComponentEligible(m: ModelInfo, role: OmniComponentRole): boolean {
  if (isCollectionModel(m)) return false;
  const cap = capabilityFromModelInfo(m);
  const labels = lowerLabels(m);

  switch (role) {
    case 'llm':
      return cap === 'chat' || cap === 'omni' || !labels.some(label => NON_PLANNER_LABELS.has(label));
    case 'vision':
      return cap === 'omni' || hasAnyLabel(m, ['vision', 'vlm', 'vision-language', 'image-input']);
    case 'image':
      return cap === 'image' || hasAnyLabel(m, ['image', 'image-generation', 'diffusion']);
    case 'edit':
      return hasAnyLabel(m, ['edit', 'image-edit', 'image-editing', 'upscaling']);
    case 'transcription':
      return cap === 'audio' || hasAnyLabel(m, ['audio', 'transcription', 'realtime-transcription', 'asr']);
    case 'speech':
      return cap === 'tts' || hasAnyLabel(m, ['tts', 'speech', 'text-to-speech']);
    default:
      return false;
  }
}

function omniComponentOptionFromModel(m: ModelInfo): OmniComponentOption {
  const id = modelName(m);
  const downloaded = Boolean((m as any).downloaded);
  const custom = Boolean((m as any).custom);
  const recipe = String((m as any).recipe || '');
  const labels = lowerLabels(m);
  const source: OmniComponentOptionSource = custom ? 'custom' : downloaded ? 'downloaded' : 'registered';
  const detailParts = [
    custom ? 'custom' : downloaded ? 'downloaded' : 'registered · will download when pulled',
    recipeLabel(recipe),
    labels.slice(0, 3).map(labelDisplay).join(', '),
  ].filter(Boolean);
  return {
    id,
    label: String(m.display_name || id),
    detail: detailParts.join(' · '),
    source,
    downloaded,
    custom,
    recipe,
    labels,
  };
}

function compareOmniComponentOptions(a: OmniComponentOption, b: OmniComponentOption): number {
  const sourceRank: Record<OmniComponentOptionSource, number> = { custom: 0, downloaded: 1, registered: 2 };
  const diff = sourceRank[a.source] - sourceRank[b.source];
  if (diff !== 0) return diff;
  return a.label.localeCompare(b.label);
}

interface OmniComponentPickerProps {
  role: OmniComponentRole;
  value: string;
  options: OmniComponentOption[];
  onChange: (value: string) => void;
  onHuggingFaceSearch?: (query: string) => void;
}

const OmniComponentPicker: React.FC<OmniComponentPickerProps> = ({ role, value, options, onChange, onHuggingFaceSearch }) => {
  const config = OMNI_COMPONENT_ROLE_CONFIG[role];
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState('');
  const selected = options.find(option => option.id === value);
  const queryText = query.trim().toLowerCase();
  const visibleOptions = useMemo(() => {
    const filtered = queryText
      ? options.filter(option => {
        const haystack = `${option.label} ${option.id} ${option.detail} ${option.recipe} ${option.labels.join(' ')}`.toLowerCase();
        return haystack.includes(queryText);
      })
      : options;
    return filtered.slice(0, 40);
  }, [options, queryText]);
  const groups: Array<{ source: OmniComponentOptionSource; label: string; options: OmniComponentOption[] }> = [
    { source: 'custom' as OmniComponentOptionSource, label: 'Custom models', options: visibleOptions.filter(option => option.source === 'custom') },
    { source: 'downloaded' as OmniComponentOptionSource, label: 'Downloaded locally', options: visibleOptions.filter(option => option.source === 'downloaded') },
    { source: 'registered' as OmniComponentOptionSource, label: 'Registered registry models', options: visibleOptions.filter(option => option.source === 'registered') },
  ].filter(group => group.options.length > 0);

  return (
    <div className="omni-component-picker">
      <label className="omni-component-picker__label" title={config.help}>{config.label}{config.required ? ' *' : ''}</label>
      <div className="omni-component-picker__control">
        <input
          value={open ? query : (selected ? selected.label : '')}
          onFocus={() => { setOpen(true); setQuery(''); }}
          onChange={e => { setQuery(e.target.value); setOpen(true); }}
          onBlur={() => window.setTimeout(() => setOpen(false), 120)}
          placeholder={config.placeholder}
          aria-label={`${config.label} component`}
          autoComplete="off"
        />
        {value && !config.required && (
          <button
            type="button"
            className="omni-component-picker__clear"
            onMouseDown={e => e.preventDefault()}
            onClick={() => onChange('')}
            title={`Clear ${config.label}`}
          >×</button>
        )}
        <span className="omni-component-picker__chevron">⌄</span>
        {open && (
          <div className="omni-component-picker__menu" role="listbox">
            {groups.length > 0 ? groups.map(group => (
              <div className="omni-component-picker__group" key={group.source}>
                <div className="omni-component-picker__group-label">{group.label}</div>
                {group.options.map(option => (
                  <button
                    type="button"
                    key={option.id}
                    className={`omni-component-picker__option${option.id === value ? ' omni-component-picker__option--selected' : ''}`}
                    onMouseDown={e => e.preventDefault()}
                    onClick={() => { onChange(option.id); setQuery(''); setOpen(false); }}
                    role="option"
                    aria-selected={option.id === value}
                  >
                    <span className="omni-component-picker__option-name">{option.label}</span>
                    <span className="omni-component-picker__option-id">{option.id}</span>
                    <span className="omni-component-picker__option-detail">{option.detail}</span>
                  </button>
                ))}
              </div>
            )) : (
              <div className="omni-component-picker__empty">
                No compatible {config.label.toLowerCase()} model found. Use the main search or HuggingFace zone to download/register one first.
              </div>
            )}
            {queryText.length >= 2 && onHuggingFaceSearch && (
              <button
                type="button"
                className="omni-component-picker__hf-search"
                onMouseDown={e => e.preventDefault()}
                onClick={() => { onHuggingFaceSearch(query.trim()); setOpen(false); }}
              >
                🤗 Search HuggingFace for “{query.trim()}”
              </button>
            )}
          </div>
        )}
      </div>
      <div className="omni-component-picker__help">{selected ? selected.detail : config.help}</div>
    </div>
  );
};

/* ── Component ─────────────────────────────────────────────── */

interface ModelManagerProps {
  onModelSelect: (model: string) => void;
  selectedModel: string | null;
  accountSession: AccountSession;
}

const ModelManager: React.FC<ModelManagerProps> = ({ onModelSelect, selectedModel, accountSession }) => {
  const [models, setModels] = useState<ModelInfo[]>([]);
  const [loadedModels, setLoadedModels] = useState<LoadedModel[]>([]);
  const [connectionStatus, setConnectionStatus] = useState(api.status);
  const [loadingModel, setLoadingModel] = useState<string | null>(null);
  const [pulling, setPulling] = useState<Record<string, number>>({});  // model → percent
  const pullAbortRef = useRef<Record<string, AbortController>>({});
  const [expandedModel, setExpandedModel] = useState<string | null>(null);
  const [searchQuery, setSearchQuery] = useState('');
  const [filterTab, setFilterTab] = useState<FilterTab>('all');
  const [showAllAvailable, setShowAllAvailable] = useState(false);
  const searchRef = useRef<HTMLInputElement>(null);

  // HuggingFace search state
  const [hfResults, setHfResults] = useState<HFModelResult[]>([]);
  const [hfLoading, setHfLoading] = useState(false);
  const [hfError, setHfError] = useState<string | null>(null);
  const [expandedHfModel, setExpandedHfModel] = useState<string | null>(null);
  const [pullingHf, setPullingHf] = useState<Record<string, number>>({}); // hf id → percent
  const pullHfAbortRef = useRef<Record<string, AbortController>>({});
  const [hfVariants, setHfVariants] = useState<Record<string, PullVariantsResult>>({}); // hf id → variants data
  const [hfVariantsLoading, setHfVariantsLoading] = useState<Record<string, boolean>>({}); // hf id → loading

  const [customModels, setCustomModels] = useState<ModelInfo[]>(() => loadCustomModels(accountSession.storageScope).map(customModelToModelInfo));
  const [showCustomForm, setShowCustomForm] = useState(false);
  const [customError, setCustomError] = useState<string | null>(null);
  const [customDraft, setCustomDraft] = useState<CustomModelDraftState>(() => createEmptyCustomDraft());

  const reloadCustomModels = useCallback(() => {
    setCustomModels(loadCustomModels(accountSession.storageScope).map(customModelToModelInfo));
  }, [accountSession.storageScope]);

  useEffect(() => { reloadCustomModels(); }, [reloadCustomModels]);

  const refresh = useCallback(async () => {
    if (!api.isConnected) return;
    const result = await api.refresh();
    if (result) {
      setModels(result.models.data);
      setLoadedModels(result.health.all_models_loaded);
    }
  }, []);

  useEffect(() => { refresh(); }, [refresh]);

  // Re-fetch when server connection status changes (e.g. connects after initial mount)
  useEffect(() => {
    const unsub = api.onStatusChange((status) => {
      setConnectionStatus(status);
      if (status === 'connected') refresh();
    });
    return unsub;
  }, [refresh]);

  // Re-fetch when models are loaded/unloaded/deleted via any path (tools, other views)
  useEffect(() => {
    return api.onModelsChanged(() => { refresh(); });
  }, [refresh]);

  const applyServerDownloads = useCallback((downloads: DownloadProgressEvent[]) => {
    const active: Record<string, number> = {};
    let sawCompletedModel = false;
    downloads.forEach(download => {
      const type = String(download.type || '').toLowerCase();
      const id = String(download.id || '');
      if (type && type !== 'model') return;
      if (!type && id && !id.startsWith('model:')) return;
      const name = String(download.model_name || download.name || (id.startsWith('model:') ? id.slice('model:'.length) : '')).trim();
      if (!name) return;
      const status = String(download.status || '').toLowerCase();
      const isActive = download.running === true || status === 'downloading' || status === 'paused';
      if (isActive) active[name] = typeof download.percent === 'number' ? download.percent : 0;
      if (download.complete || status === 'completed' || status === 'error' || status === 'cancelled') sawCompletedModel = true;
    });
    setPulling(prev => {
      const next: Record<string, number> = {};
      Object.entries(prev).forEach(([name, value]) => {
        if (pullAbortRef.current[name]) next[name] = value;
      });
      return { ...next, ...active };
    });
    if (sawCompletedModel) refresh();
  }, [refresh]);

  useEffect(() => {
    if (connectionStatus !== 'connected') return;
    let cancelled = false;
    let timer: ReturnType<typeof setTimeout> | null = null;
    const tick = async () => {
      if (cancelled) return;
      try {
        applyServerDownloads(await api.downloads());
      } catch {
        // Older servers might not expose /downloads; normal SSE fallback still works.
      }
      if (!cancelled) timer = setTimeout(tick, 2000);
    };
    tick();
    return () => {
      cancelled = true;
      if (timer) clearTimeout(timer);
    };
  }, [applyServerDownloads, connectionStatus]);

  /* ── HuggingFace debounced search ────────────────────────── */

  useEffect(() => {
    const q = searchQuery.trim();
    if (q.length < 2 || filterTab === 'omni') {
      setHfResults([]);
      setHfLoading(false);
      setHfError(null);
      setExpandedHfModel(null);
      return;
    }

    setHfLoading(true);
    setHfError(null);
    const ac = new AbortController();

    const timer = setTimeout(async () => {
      try {
        const results = await searchHuggingFace(q, ac.signal);
        setHfResults(results);
        setHfError(null);
      } catch (err) {
        if (!ac.signal.aborted) {
          setHfResults([]);
          setHfError(friendlyErrorMessage(err));
        }
      } finally {
        if (!ac.signal.aborted) setHfLoading(false);
      }
    }, 400);

    return () => {
      clearTimeout(timer);
      ac.abort();
    };
  }, [searchQuery, filterTab]);

  /* ── Actions ─────────────────────────────────────────────── */

  const handleLoad = async (model: ModelInfo) => {
    if (loadingModel) return;
    const name = modelName(model);
    setLoadingModel(name);
    try {
      if ((model as any).custom) {
        await api.pullModel(name, {}, customRegistrationOptions(model));
      }
      await api.loadModel(name, customLoadOptions(model));
      await refresh();
      onModelSelect(name);
    } catch { /* keep going */ }
    setLoadingModel(null);
  };

  const handleUnload = async (model: LoadedModel) => {
    setLoadingModel(model.model_name);
    const virtualComponents = Array.isArray(model.recipe_options?.components) && model.recipe_options?.virtual_collection === true
      ? model.recipe_options.components.filter((component): component is string => typeof component === 'string')
      : [];
    if (virtualComponents.length > 0) {
      await Promise.allSettled(virtualComponents.map(component => api.unloadModel(component)));
    } else {
      await api.unloadModel(model.model_name);
    }
    await refresh();
    setLoadingModel(null);
  };

  const handleDelete = async (model: ModelInfo) => {
    const name = modelName(model);
    if ((model as any).custom) {
      if (!confirm(`Delete custom model definition "${model.display_name || name}"? This does not remove external model files.`)) return;
      deleteCustomModel(accountSession.storageScope, String((model as any).id || name));
      reloadCustomModels();
      return;
    }
    if (!confirm(`Delete "${model.display_name || name}"? This removes the downloaded files. If the model is loaded, it will be unloaded first.`)) return;
    setLoadingModel(name);
    try {
      await api.deleteModel(name);
      await refresh();
    } catch (err: any) {
      console.error('Delete failed:', err);
    }
    setLoadingModel(null);
  };

  const handlePull = async (model: ModelInfo) => {
    const name = modelName(model);
    if (pulling[name] !== undefined) return;
    const ac = new AbortController();
    pullAbortRef.current[name] = ac;
    setPulling(p => ({ ...p, [name]: 0 }));

    const callbacks: PullCallbacks = {
      onProgress: (data) => {
        if (data.percent !== undefined) {
          setPulling(p => ({ ...p, [name]: data.percent! }));
        }
      },
      onComplete: () => {
        delete pullAbortRef.current[name];
        setPulling(p => { const next = { ...p }; delete next[name]; return next; });
        refresh();
      },
      onError: () => {
        delete pullAbortRef.current[name];
        setPulling(p => { const next = { ...p }; delete next[name]; return next; });
      },
      signal: ac.signal,
    };

    await api.pullModel(name, callbacks, customRegistrationOptions(model));
  };

  const handleCancelPull = async (name: string) => {
    pullAbortRef.current[name]?.abort();
    await api.controlDownload(`model:${name}`, 'cancel').catch(() => undefined);
    delete pullAbortRef.current[name];
    setPulling(p => { const next = { ...p }; delete next[name]; return next; });
  };

  const handlePullAndLoad = async (model: ModelInfo) => {
    const name = modelName(model);
    const ac = new AbortController();
    pullAbortRef.current[name] = ac;
    setPulling(p => ({ ...p, [name]: 0 }));

    const callbacks: PullCallbacks = {
      onProgress: (data) => {
        if (data.percent !== undefined) {
          setPulling(p => ({ ...p, [name]: data.percent! }));
        }
      },
      onComplete: async () => {
        delete pullAbortRef.current[name];
        setPulling(p => { const next = { ...p }; delete next[name]; return next; });
        await refresh();
        setLoadingModel(name);
        try {
          await api.loadModel(name, customLoadOptions(model));
          await refresh();
          onModelSelect(name);
        } catch { /* keep going */ }
        setLoadingModel(null);
      },
      onError: () => {
        delete pullAbortRef.current[name];
        setPulling(p => { const next = { ...p }; delete next[name]; return next; });
      },
      signal: ac.signal,
    };

    await api.pullModel(name, callbacks, customRegistrationOptions(model));
  };

  const handleHfPull = async (hfId: string, variantName: string, recipe: string) => {
    if (pullingHf[hfId] !== undefined) return;
    const vdata = hfVariants[hfId];
    const suggestedName = vdata?.suggested_name || hfId.split('/').pop() || hfId;
    const modelName = `user.${suggestedName}`;
    const checkpoint = `${hfId}:${variantName}`;
    const ac = new AbortController();
    pullHfAbortRef.current[hfId] = ac;
    setPullingHf(p => ({ ...p, [hfId]: 0 }));

    const callbacks: PullCallbacks = {
      onProgress: (data) => {
        if (data.percent !== undefined) {
          setPullingHf(p => ({ ...p, [hfId]: data.percent! }));
        }
      },
      onComplete: () => {
        delete pullHfAbortRef.current[hfId];
        setPullingHf(p => { const next = { ...p }; delete next[hfId]; return next; });
        refresh();
      },
      onError: (err) => {
        console.error('HF pull failed:', err);
        delete pullHfAbortRef.current[hfId];
        setPullingHf(p => { const next = { ...p }; delete next[hfId]; return next; });
      },
      signal: ac.signal,
    };

    await api.pullModel(modelName, callbacks, { checkpoint, recipe });
  };

  const handleCancelHfPull = async (hfId: string) => {
    pullHfAbortRef.current[hfId]?.abort();
    const vdata = hfVariants[hfId];
    const suggestedName = vdata?.suggested_name || hfId.split('/').pop() || hfId;
    await api.controlDownload(`model:user.${suggestedName}`, 'cancel').catch(() => undefined);
    delete pullHfAbortRef.current[hfId];
    setPullingHf(p => { const next = { ...p }; delete next[hfId]; return next; });
  };

  const fetchHfVariants = async (hfId: string) => {
    if (hfVariants[hfId] || hfVariantsLoading[hfId]) return;
    setHfVariantsLoading(prev => ({ ...prev, [hfId]: true }));
    try {
      const result = await api.pullVariants(hfId);
      setHfVariants(prev => ({ ...prev, [hfId]: result }));
    } catch (err) {
      console.error('Failed to fetch variants for', hfId, err);
    }
    setHfVariantsLoading(prev => ({ ...prev, [hfId]: false }));
  };


  const handleCustomDraftChange = (patch: Partial<CustomModelDraftState>) => {
    setCustomDraft(prev => ({ ...prev, ...patch }));
    setCustomError(null);
  };

  const openCustomForm = (mode: CustomFormMode = 'model') => {
    setCustomDraft(createEmptyCustomDraft(mode));
    setCustomError(null);
    setShowCustomForm(true);
  };

  const closeCustomForm = () => {
    setShowCustomForm(false);
    setCustomError(null);
  };

  const defaultRecipeForCapability = (capability: CustomModelCapability, omniSource: 'single' | 'collection' = customDraft.omniSource) => {
    if (capability === 'image') return 'sd-cpp';
    if (capability === 'audio') return 'whispercpp';
    if (capability === 'tts') return 'kokoro';
    if (capability === 'omni' && omniSource === 'collection') return 'collection.omni';
    return 'llamacpp';
  };

  const handleSaveCustomModel = (e: React.FormEvent) => {
    e.preventDefault();
    setCustomError(null);
    try {
      if (customDraft.capability === 'omni' && customDraft.omniSource === 'collection' && !customDraft.llmComponent.trim()) {
        throw new Error('Select a planner LLM for the Omni collection.');
      }
      const componentRoles = {
        llm: customDraft.llmComponent,
        vision: customDraft.visionComponent,
        image: customDraft.imageComponent,
        edit: customDraft.editComponent,
        transcription: customDraft.transcriptionComponent,
        speech: customDraft.speechComponent,
      };
      const components = customDraft.omniSource === 'collection'
        ? Object.values(componentRoles).map(v => v.trim()).filter(Boolean)
        : [];
      const saved = upsertCustomModel(accountSession.storageScope, {
        name: customDraft.name,
        displayName: customDraft.displayName,
        checkpoint: customDraft.checkpoint,
        recipe: customDraft.recipe,
        capability: customDraft.capability,
        maxContextWindow: customDraft.maxContextWindow.trim() && Number.isFinite(Number(customDraft.maxContextWindow)) ? Number(customDraft.maxContextWindow) : undefined,
        labels: customDraft.labels.split(',').map(l => l.trim()).filter(Boolean),
        components,
        componentRoles,
      });
      reloadCustomModels();
      setShowCustomForm(false);
      setSearchQuery(saved.name);
      setCustomDraft(createEmptyCustomDraft());
    } catch (err) {
      setCustomError(err instanceof Error ? err.message : 'Could not save custom model.');
    }
  };

  /* ── Derived data ────────────────────────────────────────── */

  const allModels = useMemo(() => {
    const seen = new Set<string>();
    const merged: ModelInfo[] = [];
    for (const m of customModels) {
      const name = modelName(m).toLowerCase();
      seen.add(name);
      merged.push(m);
    }
    for (const m of models) {
      const name = modelName(m).toLowerCase();
      if (!seen.has(name)) merged.push(m);
    }
    return merged;
  }, [customModels, models]);

  const omniComponentOptions = useMemo(() => {
    const roles: Record<OmniComponentRole, OmniComponentOption[]> = {
      llm: [],
      vision: [],
      image: [],
      edit: [],
      transcription: [],
      speech: [],
    };
    const seenByRole: Record<OmniComponentRole, Set<string>> = {
      llm: new Set<string>(),
      vision: new Set<string>(),
      image: new Set<string>(),
      edit: new Set<string>(),
      transcription: new Set<string>(),
      speech: new Set<string>(),
    };
    for (const m of allModels) {
      for (const role of Object.keys(roles) as OmniComponentRole[]) {
        if (!isOmniComponentEligible(m, role)) continue;
        const option = omniComponentOptionFromModel(m);
        const key = option.id.toLowerCase();
        if (seenByRole[role].has(key)) continue;
        seenByRole[role].add(key);
        roles[role].push(option);
      }
    }
    for (const role of Object.keys(roles) as OmniComponentRole[]) {
      roles[role].sort(compareOmniComponentOptions);
    }
    return roles;
  }, [allModels]);

  const displayLoadedModels = useMemo(
    () => withVirtualLoadedCollections(loadedModels, allModels),
    [loadedModels, allModels]
  );

  const loadedNames = useMemo(
    () => new Set(displayLoadedModels.map(m => m.model_name)),
    [displayLoadedModels]
  );

  const { downloaded, available } = useMemo(() => {
    const dl: ModelInfo[] = [];
    const av: ModelInfo[] = [];
    for (const m of allModels) {
      const name = modelName(m);
      if (loadedNames.has(name)) continue;
      if ((m as any).downloaded || isCollectionFullyDownloaded(m, allModels)) dl.push(m);
      else av.push(m);
    }
    return { downloaded: dl, available: av };
  }, [allModels, loadedNames]);

  const applyFilter = useCallback((list: ModelInfo[]) => {
    let filtered = list;

    // Type filter. The Omni tab is intentionally collection-only: single VLMs
    // are useful chat/vision models, but they are not Omni collections.
    if (filterTab !== 'all') {
      filtered = filtered.filter(m => {
        if (filterTab === 'omni') return isCollectionModel(m);
        const type = modelType(m);
        if (filterTab === 'embedding') return type === 'embedding' || type === 'reranking';
        return type === filterTab;
      });
    }

    // Search
    if (searchQuery.trim()) {
      const q = searchQuery.toLowerCase();
      filtered = filtered.filter(m => {
        const name = modelName(m).toLowerCase();
        const labels = (m.labels || []).join(' ').toLowerCase();
        const recipe = ((m as any).recipe || '').toLowerCase();
        return name.includes(q) || labels.includes(q) || recipe.includes(q);
      });
    }

    return filtered;
  }, [filterTab, searchQuery]);

  const filteredDownloaded = useMemo(() => applyFilter(downloaded), [applyFilter, downloaded]);
  const filteredAvailable = useMemo(() => applyFilter(available), [applyFilter, available]);

  // Filter running models by search/type too
  const filteredRunning = useMemo(() => {
    if (filterTab === 'all' && !searchQuery.trim()) return displayLoadedModels;
    return displayLoadedModels.filter(m => {
      // Type filter
      if (filterTab !== 'all') {
        const info = allModels.find(mi => modelName(mi) === m.model_name);
        if (filterTab === 'omni') {
          if (!(info ? isCollectionModel(info) : loadedIsVirtualOmniCollection(m))) return false;
        } else {
          const cap = info ? capabilityFromModelInfo(info) : capabilityFromLoaded(m);
          const type = cap === 'chat' || cap === 'unknown' ? 'llm' : cap;
          if (filterTab === 'embedding') {
            if (type !== 'embedding' && type !== 'reranking') return false;
          } else if (type !== filterTab) return false;
        }
      }
      // Search
      if (searchQuery.trim()) {
        const q = searchQuery.toLowerCase();
        if (!m.model_name.toLowerCase().includes(q) && !m.recipe.toLowerCase().includes(q)) return false;
      }
      return true;
    });
  }, [displayLoadedModels, filterTab, searchQuery, allModels]);

  // Available zone: show first N unless expanded or searching
  const AVAILABLE_INITIAL = 20;
  const visibleAvailable = useMemo(() => {
    if (showAllAvailable || searchQuery.trim() || filterTab !== 'all') return filteredAvailable;
    return filteredAvailable.slice(0, AVAILABLE_INITIAL);
  }, [filteredAvailable, showAllAvailable, searchQuery, filterTab]);
  const hiddenAvailableCount = filteredAvailable.length - visibleAvailable.length;

  // HuggingFace results — exclude models already in local registry
  const filteredHfResults = useMemo(() => {
    if (hfResults.length === 0) return [];
    // The Omni filter is collection-only. HuggingFace search returns individual
    // GGUF checkpoints, so showing them here would leak LLMs into the Omni tab.
    if (filterTab === 'omni') return [];
    const localIds = new Set(allModels.map(m => modelName(m).toLowerCase()));
    return hfResults.filter(r => !localIds.has(r.id.toLowerCase()));
  }, [hfResults, allModels, filterTab]);

  /* ── Toggle detail ───────────────────────────────────────── */
  const toggleDetail = (name: string) => {
    setExpandedModel(prev => prev === name ? null : name);
  };

  /* ── Render helpers ──────────────────────────────────────── */

  const renderLabels = (labels: string[]) => {
    if (!labels || labels.length === 0) return null;
    // Filter out the 'llamacpp' label since it's redundant with recipe
    const displayLabels = labels.filter(l => l !== 'llamacpp');
    if (displayLabels.length === 0) return null;
    return (
      <div className="row__labels">
        {displayLabels.map(l => (
          <span key={l} className="row__label">{labelDisplay(l)}</span>
        ))}
      </div>
    );
  };

  const renderModelDetail = (m: ModelInfo, liveCtxSize?: number) => {
    const name = modelName(m);
    const checkpoint = (m as any).checkpoint || '';
    const checkpoints = (m as any).checkpoints || {};
    const recipe = (m as any).recipe || '';
    const maxCtx = liveCtxSize || (m as any).max_context_window;
    const compositeModels = (m as any).composite_models || [];
    const collectionComponents = getCollectionComponents(m);
    const url = hfUrl(checkpoint);

    return (
      <div className="row__detail">
        <div className="detail__grid">
          {/* Left column: metadata */}
          <div className="detail__meta">
            <div className="detail__field">
              <span className="detail__label">Backend</span>
              <span className="detail__value">{recipeIcon(recipe)} {recipeLabel(recipe)}</span>
            </div>
            {m.size && (
              <div className="detail__field">
                <span className="detail__label">Size</span>
                <span className="detail__value">{formatSize(m.size)}</span>
              </div>
            )}
            {maxCtx && (
              <div className="detail__field">
                <span className="detail__label">Context</span>
                <span className="detail__value">{(maxCtx / 1024).toFixed(0)}K tokens</span>
              </div>
            )}
            {m.labels && m.labels.length > 0 && (
              <div className="detail__field">
                <span className="detail__label">Capabilities</span>
                <div className="detail__caps">
                  {m.labels.filter(l => l !== 'llamacpp').map(l => (
                    <span key={l} className="detail__cap">{labelDisplay(l)}</span>
                  ))}
                </div>
              </div>
            )}
          </div>

          {/* Right column: checkpoint / HF link */}
          <div className="detail__source">
            <div className="detail__field">
              <span className="detail__label">Checkpoint</span>
              <span className="detail__value detail__value--mono">{checkpoint || '—'}</span>
            </div>
            {Object.keys(checkpoints).length > 1 && (
              <div className="detail__field">
                <span className="detail__label">Components</span>
                {Object.entries(checkpoints).map(([k, v]) => (
                  <div key={k} className="detail__checkpoint">
                    <span className="detail__ck-key">{k}</span>
                    <span className="detail__ck-val">{String(v)}</span>
                  </div>
                ))}
              </div>
            )}
            {compositeModels.length > 0 && (
              <div className="detail__field">
                <span className="detail__label">Includes</span>
                <div className="detail__caps">
                  {compositeModels.map((c: string) => (
                    <span key={c} className="detail__cap">{c}</span>
                  ))}
                </div>
              </div>
            )}
            {collectionComponents.length > 0 && (
              <div className="detail__field">
                <span className="detail__label">Omni components</span>
                <div className="detail__caps">
                  {collectionComponents.map(component => (
                    <span key={component} className="detail__cap">{component}</span>
                  ))}
                </div>
              </div>
            )}
            {url && (
              <a className="detail__hf-link" href={url} target="_blank" rel="noopener noreferrer">
                🤗 View on Hugging Face
              </a>
            )}
          </div>
        </div>
      </div>
    );
  };

  const renderRunningModel = (m: LoadedModel) => {
    const info = allModels.find(mi => modelName(mi) === m.model_name);
    const cap = info ? capabilityFromModelInfo(info) : capabilityFromLoaded(m);
    const type = cap === 'chat' || cap === 'unknown' ? 'llm' : cap;
    const componentCount = Array.isArray(m.recipe_options?.components) ? m.recipe_options.components.length : 0;
    const isActive = selectedModel === m.model_name;
    const selectable = canSelectInComposer(m) || (cap === 'chat' || cap === 'omni' || cap === 'image' || cap === 'audio' || cap === 'tts');
    return (
      <div className={`row row--running${isActive ? ' row--active' : ''}`} key={m.model_name}>
        <div className="row__content" onClick={() => toggleDetail(m.model_name)}>
          <div className="row__main">
            <div className="row__icon row__icon--running" style={{ borderColor: typeColor(type) }}>
              {recipeIcon(m.recipe)}
            </div>
            <div className="row__text">
              <span className="row__name-wrap"><span className="row__name">{m.model_name}</span><CopyInlineButton text={m.model_name} /></span>
              <span className="row__sub">
                {recipeLabel(m.recipe)} · {(m.device || 'device').toUpperCase()}
                {` · ${capabilityIcon(cap)} ${capabilityLabel(cap)}`}
                {componentCount > 0 ? ` · ${componentCount} components loaded` : ''}
              </span>
            </div>
          </div>
          <div className="row__right">
            <span className="row__status-pill row__status-pill--running">
              <span className="row__pulse" /> {isActive ? `Active ${capabilityLabel(cap)} mode` : 'Running'}
            </span>
            {selectable && !isActive && (
              <button className="row__action" onClick={(e) => { e.stopPropagation(); onModelSelect(m.model_name); }}>
                Use in {capabilityLabel(cap)} mode
              </button>
            )}
            <button
              className="row__action row__action--unload"
              onClick={(e) => { e.stopPropagation(); handleUnload(m); }}
              disabled={loadingModel === m.model_name}
            >
              {loadingModel === m.model_name ? '⏳' : 'Unload'}
            </button>
            <button
              className="row__action row__action--delete"
              onClick={(e) => {
                e.stopPropagation();
                const infoForDelete = allModels.find(mi => modelName(mi) === m.model_name);
                if (infoForDelete) handleDelete(infoForDelete);
              }}
              disabled={loadingModel === m.model_name}
              title={info && (info as any).custom ? 'Delete custom model definition' : 'Delete model files'}
            >
              🗑
            </button>
            <span className="row__expand">{expandedModel === m.model_name ? '▾' : '▸'}</span>
          </div>
        </div>

        {expandedModel === m.model_name && (() => {
          // find matching ModelInfo for detail
          const info = allModels.find(mi => modelName(mi) === m.model_name);
          if (!info) return null;
          // Merge loaded model's live recipe_options over static registry data
          const liveCtx = m.recipe_options?.ctx_size as number | undefined;
          return (
            <>
              {renderModelDetail(info, liveCtx)}
              {m.recipe_options && Object.keys(m.recipe_options).length > 0 && (
                <div className="row__detail row__detail--live">
                  <div className="detail__field">
                    <span className="detail__label">Active Recipe Options</span>
                    <div className="detail__recipe-options">
                      {Object.entries(m.recipe_options).map(([k, v]) => (
                        <span key={k} className="detail__recipe-opt">
                          <span className="detail__ro-key">{k}</span>
                          <span className="detail__ro-val">{String(v)}</span>
                        </span>
                      ))}
                    </div>
                  </div>
                </div>
              )}
            </>
          );
        })()}
      </div>
    );
  };

  const renderModelRow = (m: ModelInfo, isDownloaded: boolean) => {
    const name = modelName(m);
    const type = modelType(m);
    const isCollection = isCollectionModel(m);
    const isLoading = loadingModel === name;
    const pullPercent = pulling[name];
    const isPulling = pullPercent !== undefined;

    return (
      <div className={`row${expandedModel === name ? ' row--expanded' : ''}`} key={name}>
        <div className="row__content" onClick={() => toggleDetail(name)}>
          <div className="row__main">
            <div className="row__icon" style={{ borderColor: typeColor(type) }}>
              {recipeIcon((m as any).recipe)}
            </div>
            <div className="row__text">
              <span className="row__name-wrap"><span className="row__name">{m.display_name || name}</span><CopyInlineButton text={name} /></span>
              <span className="row__sub">
                {recipeLabel((m as any).recipe || '')}
                {isCollection ? ` · ${collectionComponentLabel(m)}` : ''}
                {m.size ? ` · ${formatSize(m.size)}` : ''}
                {(m as any).max_context_window ? ` · ${((m as any).max_context_window / 1024).toFixed(0)}K ctx` : ''}
              </span>
              {renderLabels(m.labels || [])}
            </div>
          </div>
          <div className="row__right">
            {isPulling ? (
              <div className="row__progress">
                <div className="row__progress-bar">
                  <div className="row__progress-fill" style={{ width: `${pullPercent}%` }} />
                </div>
                <span className="row__progress-text">{pullPercent.toFixed(0)}%</span>
                <button
                  className="row__action row__action--cancel"
                  onClick={(e) => { e.stopPropagation(); handleCancelPull(name); }}
                  title="Cancel download"
                >✕</button>
              </div>
            ) : isDownloaded ? (
              <>
                <span className="row__status-pill row__status-pill--ready">{(m as any).custom ? 'Custom' : 'Ready'}</span>
                <button
                  className="row__action"
                  onClick={(e) => { e.stopPropagation(); handleLoad(m); }}
                  disabled={isLoading}
                >
                  {isLoading ? '⏳ Loading…' : '▶ Load'}
                </button>
                <button
                  className="row__action row__action--delete"
                  onClick={(e) => { e.stopPropagation(); handleDelete(m); }}
                  disabled={isLoading}
                  title={(m as any).custom ? 'Delete custom model definition' : 'Delete model files'}
                >
                  🗑
                </button>
              </>
            ) : (
              <>
                <button
                  className="row__action row__action--download"
                  onClick={(e) => { e.stopPropagation(); handlePull(m); }}
                  disabled={isPulling}
                >
                  ↓ Download
                </button>
                <button
                  className="row__action"
                  onClick={(e) => { e.stopPropagation(); handlePullAndLoad(m); }}
                  disabled={isPulling}
                >
                  ↓▶ Get & Load
                </button>
              </>
            )}
            <span className="row__expand">{expandedModel === name ? '▾' : '▸'}</span>
          </div>
        </div>

        {expandedModel === name && renderModelDetail(m)}
      </div>
    );
  };

  const renderHfRow = (r: HFModelResult) => {
    const isExpanded = expandedHfModel === r.id;
    const pipelineTag = r.pipeline_tag || '';
    const displayTags = (r.tags || [])
      .filter(t => t !== 'gguf' && t !== 'transformers' && t !== 'pytorch' && t !== 'safetensors')
      .slice(0, 5);
    const hfPullPercent = pullingHf[r.id];
    const isHfPulling = hfPullPercent !== undefined;

    // Variant data from the server (fetched on expand)
    const vdata = hfVariants[r.id];
    const isLoadingVariants = hfVariantsLoading[r.id] || false;
    const recipeBadge = vdata ? (RECIPE_BADGES[vdata.recipe] || vdata.recipe) : '';

    const handleExpand = () => {
      const next = isExpanded ? null : r.id;
      setExpandedHfModel(next);
      if (next) fetchHfVariants(r.id);
    };

    return (
      <div className={`row row--hf${isExpanded ? ' row--expanded' : ''}`} key={r.id}>
        <div className="row__content" onClick={handleExpand}>
          <div className="row__main">
            <div className="row__icon row__icon--hf">🤗</div>
            <div className="row__text">
              <span className="row__name-wrap"><span className="row__name">{r.id}</span><CopyInlineButton text={r.id} title="Copy repository name" /></span>
              <span className="row__sub">
                {recipeBadge ? `${recipeBadge} · ` : ''}{pipelineTag && `${pipelineTag} · `}
                {formatDownloads(r.downloads)} downloads · {formatDownloads(r.likes)} likes
              </span>
              {displayTags.length > 0 && (
                <div className="row__labels">
                  {displayTags.map(t => (
                    <span key={t} className="row__label row__label--hf">{t}</span>
                  ))}
                </div>
              )}
            </div>
          </div>
          <div className="row__right">
            {isHfPulling ? (
              <div className="row__progress">
                <div className="row__progress-bar">
                  <div className="row__progress-fill" style={{ width: `${hfPullPercent}%` }} />
                </div>
                <span className="row__progress-text">{hfPullPercent.toFixed(0)}%</span>
                <button
                  className="row__action row__action--cancel"
                  onClick={(e) => { e.stopPropagation(); handleCancelHfPull(r.id); }}
                  title="Cancel download"
                >✕</button>
              </div>
            ) : (
              <button
                className="row__action row__action--download"
                onClick={(e) => { e.stopPropagation(); handleExpand(); }}
                title="Expand to pick a variant to download"
              >
                ↓ Download
              </button>
            )}
            <a
              className="row__action row__action--hf-link"
              href={`https://huggingface.co/${r.id}`}
              target="_blank"
              rel="noopener noreferrer"
              onClick={e => e.stopPropagation()}
            >
              🤗 View
            </a>
            <span className="row__expand">{isExpanded ? '▾' : '▸'}</span>
          </div>
        </div>

        {isExpanded && (
          <div className="row__detail row__detail--hf">
            <div className="detail__grid">
              <div className="detail__meta">
                <div className="detail__field">
                  <span className="detail__label">Repository</span>
                  <span className="detail__value detail__value--mono">{r.id}</span>
                </div>
                {pipelineTag && (
                  <div className="detail__field">
                    <span className="detail__label">Pipeline</span>
                    <span className="detail__value">{pipelineTag}</span>
                  </div>
                )}
                {vdata && (
                  <>
                    <div className="detail__field">
                      <span className="detail__label">Backend</span>
                      <span className="detail__value">{RECIPE_BADGES[vdata.recipe] || vdata.recipe}</span>
                    </div>
                    {vdata.suggested_labels.length > 0 && (
                      <div className="detail__field">
                        <span className="detail__label">Capabilities</span>
                        <span className="detail__value">{vdata.suggested_labels.join(', ')}</span>
                      </div>
                    )}
                  </>
                )}
                {r.createdAt && (
                  <div className="detail__field">
                    <span className="detail__label">Created</span>
                    <span className="detail__value">{new Date(r.createdAt).toLocaleDateString()}</span>
                  </div>
                )}
              </div>
              <div className="detail__source">
                {isLoadingVariants && (
                  <div className="detail__field">
                    <span className="detail__label">Loading variants…</span>
                  </div>
                )}
                {vdata && vdata.variants.length > 0 && (
                  <div className="detail__field">
                    <span className="detail__label">Variants — pick one to download</span>
                    <div className="hf-detail__gguf-list">
                      {vdata.variants.map(v => (
                        <button
                          key={v.name}
                          className="hf-detail__gguf-btn"
                          disabled={isHfPulling}
                          onClick={() => handleHfPull(r.id, v.name, vdata.recipe)}
                        >
                          <span className="hf-detail__gguf-name">
                            {v.name}{v.sharded ? ' (sharded)' : ''}
                          </span>
                          <span className="hf-detail__gguf-size">{formatBytes(v.size_bytes)}</span>
                          <span className="hf-detail__gguf-action">↓ Download</span>
                        </button>
                      ))}
                    </div>
                  </div>
                )}
                <a
                  className="detail__hf-link"
                  href={`https://huggingface.co/${r.id}`}
                  target="_blank"
                  rel="noopener noreferrer"
                >
                  🤗 View on Hugging Face
                </a>
              </div>
            </div>
          </div>
        )}
      </div>
    );
  };

  /* ── Keyboard shortcut ───────────────────────────────────── */
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && e.key === 'k') {
        e.preventDefault();
        searchRef.current?.focus();
      }
    };
    window.addEventListener('keydown', handler);
    return () => window.removeEventListener('keydown', handler);
  }, []);

  /* ── Stats ───────────────────────────────────────────────── */
  const showManagerEmpty = filteredRunning.length === 0 && filteredDownloaded.length === 0 && filteredAvailable.length === 0;
  const isCustomOmniCollectionDraft = customDraft.capability === 'omni' && customDraft.omniSource === 'collection';
  const customFormTitle = isCustomOmniCollectionDraft ? 'Custom Omni collection' : 'Custom model';
  const showHuggingFaceZone = filterTab !== 'omni';
  const totalDownloaded = downloaded.length + displayLoadedModels.length;
  const totalPulling = Object.keys(pulling).length;
  const updateOmniComponent = (role: OmniComponentRole, value: string) => {
    if (role === 'llm') {
      const selectedInfo = allModels.find(m => modelName(m) === value);
      const plannerHasVision = !!selectedInfo && isOmniComponentEligible(selectedInfo, 'vision');
      const visionWasPlannerDefault = !customDraft.visionComponent || customDraft.visionComponent === customDraft.llmComponent;
      handleCustomDraftChange({
        llmComponent: value,
        ...(plannerHasVision && visionWasPlannerDefault ? { visionComponent: value } : {}),
        ...(!plannerHasVision && customDraft.visionComponent === customDraft.llmComponent ? { visionComponent: '' } : {}),
      });
      return;
    }
    if (role === 'image') {
      const selectedInfo = allModels.find(m => modelName(m) === value);
      const imageHasEdit = !!selectedInfo && isOmniComponentEligible(selectedInfo, 'edit');
      const editWasImageDefault = !customDraft.editComponent || customDraft.editComponent === customDraft.imageComponent;
      handleCustomDraftChange({
        imageComponent: value,
        ...(imageHasEdit && editWasImageDefault ? { editComponent: value } : {}),
        ...(!imageHasEdit && customDraft.editComponent === customDraft.imageComponent ? { editComponent: '' } : {}),
      });
      return;
    }
    handleCustomDraftChange({ [`${role}Component`]: value } as Partial<CustomModelDraftState>);
  };
  const searchHuggingFaceFromPicker = (query: string) => {
    setFilterTab('all');
    setSearchQuery(query);
    setShowAllAvailable(true);
  };

  return (
    <div className="manager">
      <div className="manager__head">
        <div className="manager__title">
          <h1>Models</h1>
          <div className="manager__stats">
            <span className="manager__stat">
              <span className="manager__stat-num">{displayLoadedModels.length}</span> running
            </span>
            <span className="manager__stat-sep">·</span>
            <span className="manager__stat">
              <span className="manager__stat-num">{totalDownloaded}</span> downloaded
            </span>
            <span className="manager__stat-sep">·</span>
            <span className="manager__stat">
              <span className="manager__stat-num">{allModels.length}</span> available
            </span>
            {totalPulling > 0 && (
              <>
                <span className="manager__stat-sep">·</span>
                <span className="manager__stat manager__stat--active">
                  <span className="manager__stat-num">{totalPulling}</span> downloading
                </span>
              </>
            )}
          </div>
        </div>

        {/* Search & filter bar */}
        <div className="manager__toolbar">
          <div className="manager__search">
            <span className="manager__search-icon">⌕</span>
            <input
              ref={searchRef}
              className="manager__search-input"
              type="text"
              placeholder="Search models… (Ctrl+K)"
              value={searchQuery}
              onChange={e => setSearchQuery(e.target.value)}
            />
            {searchQuery && (
              <button className="manager__search-clear" onClick={() => setSearchQuery('')}>×</button>
            )}
          </div>
          <div className="manager__custom-actions">
            <button className="btn btn--ghost manager__custom-btn" onClick={() => (showCustomForm && !isCustomOmniCollectionDraft) ? closeCustomForm() : openCustomForm('model')}>+ Custom model</button>
            <button className="btn btn--ghost manager__custom-btn manager__custom-btn--omni" onClick={() => openCustomForm('omni-collection')}>+ Omni collection</button>
          </div>
          <div className="manager__filters">
            {FILTER_TABS.map(tab => (
              <button
                key={tab.key}
                className={`manager__filter${filterTab === tab.key ? ' manager__filter--active' : ''}`}
                onClick={() => setFilterTab(tab.key)}
              >
                <span className="manager__filter-icon">{tab.icon}</span>
                {tab.label}
              </button>
            ))}
          </div>
        </div>
      </div>

      <div className="manager__body">

        {showCustomForm && (
          <section className="zone custom-model-form" aria-label={`Add ${customFormTitle}`}>
            <div className="zone__head">
              <span className="zone__dot zone__dot--available" />
              <span className="zone__title">{customFormTitle}</span>
              {isCustomOmniCollectionDraft && <span className="zone__count">collection wrapper</span>}
              <span className="zone__rule" />
            </div>
            <form className="custom-model-form__grid" onSubmit={handleSaveCustomModel}>
              <label>Model name
                <input value={customDraft.name} onChange={e => handleCustomDraftChange({ name: e.target.value })} placeholder="user.my-model" />
              </label>
              <label>Display name
                <input value={customDraft.displayName} onChange={e => handleCustomDraftChange({ displayName: e.target.value })} placeholder="My custom model" />
              </label>
              <label>Capability
                <select value={customDraft.capability} onChange={e => {
                  const nextCapability = e.target.value as CustomModelCapability;
                  handleCustomDraftChange({
                    capability: nextCapability,
                    omniSource: nextCapability === 'omni' ? customDraft.omniSource : 'single',
                    recipe: defaultRecipeForCapability(nextCapability, nextCapability === 'omni' ? customDraft.omniSource : 'single'),
                  });
                }}>
                  {CUSTOM_CAPABILITIES.map(c => <option key={c.value} value={c.value}>{c.label} — {c.hint}</option>)}
                </select>
              </label>
              {customDraft.capability === 'omni' && (
                <label>Omni type
                  <select value={customDraft.omniSource} onChange={e => {
                    const omniSource = e.target.value as 'single' | 'collection';
                    handleCustomDraftChange({ omniSource, recipe: defaultRecipeForCapability('omni', omniSource) });
                  }}>
                    <option value="single">Single multimodal checkpoint</option>
                    <option value="collection">Collection wrapper from existing components</option>
                  </select>
                </label>
              )}
              <label>Recipe/backend
                <input value={customDraft.recipe} onChange={e => handleCustomDraftChange({ recipe: e.target.value })} placeholder={isCustomOmniCollectionDraft ? 'collection.omni' : 'llamacpp'} />
              </label>
              <label className="custom-model-form__wide">{isCustomOmniCollectionDraft ? 'Optional collection checkpoint/alias' : 'Checkpoint, HF repo, or local path'}
                <input
                  value={customDraft.checkpoint}
                  onChange={e => handleCustomDraftChange({ checkpoint: e.target.value })}
                  placeholder={isCustomOmniCollectionDraft ? 'Optional; first component is used when left empty' : 'org/model:Q4_K_M.gguf or /path/to/model.gguf'}
                />
              </label>
              {customDraft.capability === 'omni' && customDraft.omniSource === 'collection' && (
                <>
                  <div className="custom-model-form__hint custom-model-form__wide">
                    Build a visible Omni collection from downloaded, registered, or custom models. Pick components from the searchable dropdowns; use the HuggingFace zone to download/register new components before selecting them here.
                  </div>
                  <OmniComponentPicker role="llm" value={customDraft.llmComponent} options={omniComponentOptions.llm} onChange={value => updateOmniComponent('llm', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                  <OmniComponentPicker role="vision" value={customDraft.visionComponent} options={omniComponentOptions.vision} onChange={value => updateOmniComponent('vision', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                  <OmniComponentPicker role="image" value={customDraft.imageComponent} options={omniComponentOptions.image} onChange={value => updateOmniComponent('image', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                  <OmniComponentPicker role="edit" value={customDraft.editComponent} options={omniComponentOptions.edit} onChange={value => updateOmniComponent('edit', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                  <OmniComponentPicker role="transcription" value={customDraft.transcriptionComponent} options={omniComponentOptions.transcription} onChange={value => updateOmniComponent('transcription', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                  <OmniComponentPicker role="speech" value={customDraft.speechComponent} options={omniComponentOptions.speech} onChange={value => updateOmniComponent('speech', value)} onHuggingFaceSearch={searchHuggingFaceFromPicker} />
                </>
              )}
              <label>Context tokens
                <input value={customDraft.maxContextWindow} onChange={e => handleCustomDraftChange({ maxContextWindow: e.target.value })} inputMode="numeric" placeholder="4096" />
              </label>
              <label>Extra labels
                <input value={customDraft.labels} onChange={e => handleCustomDraftChange({ labels: e.target.value })} placeholder="tool-calling, reasoning" />
              </label>
              {customError && <div className="custom-model-form__error">⚠ {customError}</div>}
              <div className="custom-model-form__actions">
                <button className="btn btn--primary" type="submit">Save {isCustomOmniCollectionDraft ? 'Omni collection' : 'custom model'}</button>
                <button className="btn btn--ghost" type="button" onClick={closeCustomForm}>Cancel</button>
              </div>
            </form>
          </section>
        )}

        {/* Running zone */}
        {filteredRunning.length > 0 && (
          <section className="zone zone--running">
            <div className="zone__head">
              <span className="zone__dot zone__dot--running" />
              <span className="zone__title">Loaded Models</span>
              <span className="zone__count">{filteredRunning.length}</span>
              <span className="zone__rule" />
            </div>
            {filteredRunning.map(m => renderRunningModel(m))}
          </section>
        )}

        {/* Downloaded / Ready zone */}
        {filteredDownloaded.length > 0 && (
          <section className="zone zone--downloaded">
            <div className="zone__head">
              <span className="zone__dot zone__dot--ready" />
              <span className="zone__title">Downloaded</span>
              <span className="zone__count">{filteredDownloaded.length}</span>
              <span className="zone__rule" />
            </div>
            {filteredDownloaded.map(m => renderModelRow(m, true))}
          </section>
        )}

        {/* Available zone */}
        {filteredAvailable.length > 0 && (
          <section className="zone">
            <div className="zone__head">
              <span className="zone__dot zone__dot--available" />
              <span className="zone__title">Lemonade Registry</span>
              <span className="zone__count">{filteredAvailable.length}</span>
              <span className="zone__rule" />
            </div>
            {visibleAvailable.map(m => renderModelRow(m, false))}
            {hiddenAvailableCount > 0 && (
              <button
                className="zone__show-more"
                onClick={() => setShowAllAvailable(true)}
              >
                Show {hiddenAvailableCount} more models
              </button>
            )}
          </section>
        )}

        {/* HuggingFace zone — registry checkpoint search, hidden for collection-only Omni tab */}
        {showHuggingFaceZone && <section className="zone zone--hf">
          <div className="zone__head">
            <span className="zone__dot zone__dot--hf" />
            <span className="zone__title">HuggingFace</span>
            {!hfLoading && hfResults.length > 0 && <span className="zone__count">{filteredHfResults.length}</span>}
            <span className="zone__rule" />
          </div>
          {hfLoading ? (
            <div className="hf-zone__loading">
              <span className="hf-zone__spinner" />
              <span>Searching HuggingFace…</span>
            </div>
          ) : hfError ? (
            <div className="hf-zone__empty hf-zone__empty--error">
              <span>⚠</span>
              <span>HuggingFace search is unavailable: {hfError}</span>
            </div>
          ) : searchQuery.trim().length >= 2 && filteredHfResults.length > 0 ? (
            filteredHfResults.map(r => renderHfRow(r))
          ) : (
            <div className="hf-zone__empty">
              <span>🤗</span>
              <span>{searchQuery.trim().length < 2 ? 'Type at least 2 characters to search HuggingFace' : 'No HuggingFace results for this query'}</span>
            </div>
          )}
        </section>}

        <div className={`manager__empty${showManagerEmpty ? '' : ' manager__empty--hidden'}`} aria-hidden={!showManagerEmpty}>
          <span className="manager__empty-icon">{api.isConnected ? '🤖' : '🔌'}</span>
          <p>{api.isConnected
            ? 'No models found matching your search.'
            : 'Connect to a Lemonade server to see models.'
          }</p>
        </div>
      </div>
    </div>
  );
};

export default ModelManager;
