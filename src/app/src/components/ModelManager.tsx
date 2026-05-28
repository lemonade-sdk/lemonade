import React, { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import api, { ModelInfo, LoadedModel, PullCallbacks, HFModelResult, searchHuggingFace } from '../api';

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
    case 'flm': return '⚡';
    case 'ryzenai-llm': return '🔶';
    case 'sd-cpp': return '🎨';
    case 'whispercpp': return '🎤';
    case 'kokoro': return '🔊';
    case 'collection': return '📦';
    default: return '🤖';
  }
}

function recipeLabel(recipe: string): string {
  switch (recipe) {
    case 'llamacpp': return 'llama.cpp';
    case 'flm': return 'FastFlowLM';
    case 'ryzenai-llm': return 'RyzenAI';
    case 'sd-cpp': return 'Stable Diffusion';
    case 'whispercpp': return 'Whisper';
    case 'kokoro': return 'Kokoro TTS';
    case 'collection': return 'Collection';
    default: return recipe;
  }
}

function modelType(m: ModelInfo): string {
  const labels = m.labels || [];
  const recipe = (m as any).recipe || '';
  if (recipe === 'sd-cpp') return 'image';
  if (recipe === 'whispercpp') return 'audio';
  if (recipe === 'kokoro') return 'tts';
  if (labels.includes('embeddings')) return 'embedding';
  if (labels.includes('reranking')) return 'reranking';
  if (labels.includes('transcription') || labels.includes('realtime-transcription')) return 'audio';
  if (labels.includes('tts')) return 'tts';
  if (labels.includes('image')) return 'image';
  return 'llm';
}

function typeColor(type: string): string {
  switch (type) {
    case 'llm': return 'var(--accent)';
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

/* ── Filter / search types ─────────────────────────────────── */

type FilterTab = 'all' | 'llm' | 'image' | 'audio' | 'tts' | 'embedding';

const FILTER_TABS: { key: FilterTab; label: string; icon: string }[] = [
  { key: 'all', label: 'All', icon: '✦' },
  { key: 'llm', label: 'LLM', icon: '💬' },
  { key: 'image', label: 'Image', icon: '🎨' },
  { key: 'audio', label: 'Audio', icon: '🎤' },
  { key: 'tts', label: 'TTS', icon: '🔊' },
  { key: 'embedding', label: 'Embed', icon: '📐' },
];

/* ── Component ─────────────────────────────────────────────── */

interface ModelManagerProps {
  onModelSelect: (model: string) => void;
  selectedModel: string | null;
}

const ModelManager: React.FC<ModelManagerProps> = ({ onModelSelect, selectedModel }) => {
  const [models, setModels] = useState<ModelInfo[]>([]);
  const [loadedModels, setLoadedModels] = useState<LoadedModel[]>([]);
  const [loadingModel, setLoadingModel] = useState<string | null>(null);
  const [pulling, setPulling] = useState<Record<string, number>>({});  // model → percent
  const [expandedModel, setExpandedModel] = useState<string | null>(null);
  const [searchQuery, setSearchQuery] = useState('');
  const [filterTab, setFilterTab] = useState<FilterTab>('all');
  const [showAllAvailable, setShowAllAvailable] = useState(false);
  const searchRef = useRef<HTMLInputElement>(null);

  // HuggingFace search state
  const [hfResults, setHfResults] = useState<HFModelResult[]>([]);
  const [hfLoading, setHfLoading] = useState(false);
  const [expandedHfModel, setExpandedHfModel] = useState<string | null>(null);
  const [pullingHf, setPullingHf] = useState<Record<string, number>>({}); // hf id → percent

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
      if (status === 'connected') refresh();
    });
    return unsub;
  }, [refresh]);

  // Re-fetch when models are loaded/unloaded/deleted via any path (tools, other views)
  useEffect(() => {
    return api.onModelsChanged(() => { refresh(); });
  }, [refresh]);

  /* ── HuggingFace debounced search ────────────────────────── */

  useEffect(() => {
    const q = searchQuery.trim();
    if (q.length < 2) {
      setHfResults([]);
      setHfLoading(false);
      setExpandedHfModel(null);
      return;
    }

    setHfLoading(true);
    const ac = new AbortController();

    const timer = setTimeout(async () => {
      try {
        const results = await searchHuggingFace(q, ac.signal);
        setHfResults(results);
      } catch {
        // Network error / aborted — fail silently
        if (!ac.signal.aborted) setHfResults([]);
      } finally {
        if (!ac.signal.aborted) setHfLoading(false);
      }
    }, 400);

    return () => {
      clearTimeout(timer);
      ac.abort();
    };
  }, [searchQuery]);

  /* ── Actions ─────────────────────────────────────────────── */

  const handleLoad = async (model: ModelInfo) => {
    if (loadingModel) return;
    const name = modelName(model);
    setLoadingModel(name);
    try {
      await api.loadModel(name);
      onModelSelect(name);
      await refresh();
    } catch { /* keep going */ }
    setLoadingModel(null);
  };

  const handleUnload = async (model: LoadedModel) => {
    setLoadingModel(model.model_name);
    await api.unloadModel(model.model_name);
    await refresh();
    setLoadingModel(null);
  };

  const handleDelete = async (model: ModelInfo) => {
    const name = modelName(model);
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
    setPulling(p => ({ ...p, [name]: 0 }));

    const callbacks: PullCallbacks = {
      onProgress: (data) => {
        if (data.percent !== undefined) {
          setPulling(p => ({ ...p, [name]: data.percent! }));
        }
      },
      onComplete: () => {
        setPulling(p => { const next = { ...p }; delete next[name]; return next; });
        refresh();
      },
      onError: () => {
        setPulling(p => { const next = { ...p }; delete next[name]; return next; });
      },
    };

    await api.pullModel(name, callbacks);
  };

  const handlePullAndLoad = async (model: ModelInfo) => {
    const name = modelName(model);
    setPulling(p => ({ ...p, [name]: 0 }));

    const callbacks: PullCallbacks = {
      onProgress: (data) => {
        if (data.percent !== undefined) {
          setPulling(p => ({ ...p, [name]: data.percent! }));
        }
      },
      onComplete: async () => {
        setPulling(p => { const next = { ...p }; delete next[name]; return next; });
        await refresh();
        setLoadingModel(name);
        try {
          await api.loadModel(name);
          onModelSelect(name);
          await refresh();
        } catch { /* keep going */ }
        setLoadingModel(null);
      },
      onError: () => {
        setPulling(p => { const next = { ...p }; delete next[name]; return next; });
      },
    };

    await api.pullModel(name, callbacks);
  };

  const handleHfPull = async (hfId: string, ggufFile: string) => {
    if (pullingHf[hfId] !== undefined) return;
    // Create a user.* model name from the HF repo (e.g. "user.TheBloke/Llama-2-7B-GGUF")
    const modelName = `user.${hfId}`;
    const checkpoint = `${hfId}:${ggufFile}`;
    setPullingHf(p => ({ ...p, [hfId]: 0 }));

    const callbacks: PullCallbacks = {
      onProgress: (data) => {
        if (data.percent !== undefined) {
          setPullingHf(p => ({ ...p, [hfId]: data.percent! }));
        }
      },
      onComplete: () => {
        setPullingHf(p => { const next = { ...p }; delete next[hfId]; return next; });
        refresh();
      },
      onError: (err) => {
        console.error('HF pull failed:', err);
        setPullingHf(p => { const next = { ...p }; delete next[hfId]; return next; });
      },
    };

    await api.pullModel(modelName, callbacks, { checkpoint, recipe: 'llamacpp' });
  };

  /* ── Derived data ────────────────────────────────────────── */

  const loadedNames = useMemo(
    () => new Set(loadedModels.map(m => m.model_name)),
    [loadedModels]
  );

  const { downloaded, available } = useMemo(() => {
    const dl: ModelInfo[] = [];
    const av: ModelInfo[] = [];
    for (const m of models) {
      const name = modelName(m);
      if (loadedNames.has(name)) continue;
      if ((m as any).downloaded) dl.push(m);
      else av.push(m);
    }
    return { downloaded: dl, available: av };
  }, [models, loadedNames]);

  const applyFilter = useCallback((list: ModelInfo[]) => {
    let filtered = list;

    // Type filter
    if (filterTab !== 'all') {
      filtered = filtered.filter(m => {
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
    if (filterTab === 'all' && !searchQuery.trim()) return loadedModels;
    return loadedModels.filter(m => {
      // Type filter
      if (filterTab !== 'all') {
        const type = m.type || 'llm';
        if (filterTab === 'embedding') {
          if (type !== 'embedding' && type !== 'reranking') return false;
        } else if (type !== filterTab) return false;
      }
      // Search
      if (searchQuery.trim()) {
        const q = searchQuery.toLowerCase();
        if (!m.model_name.toLowerCase().includes(q) && !m.recipe.toLowerCase().includes(q)) return false;
      }
      return true;
    });
  }, [loadedModels, filterTab, searchQuery]);

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
    const localIds = new Set(models.map(m => modelName(m).toLowerCase()));
    return hfResults.filter(r => !localIds.has(r.id.toLowerCase()));
  }, [hfResults, models]);

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
    const type = m.type || 'llm';
    const isActive = selectedModel === m.model_name;
    return (
      <div className={`row row--running${isActive ? ' row--active' : ''}`} key={m.model_name}>
        <div className="row__content" onClick={() => toggleDetail(m.model_name)}>
          <div className="row__main">
            <div className="row__icon row__icon--running" style={{ borderColor: typeColor(type) }}>
              {recipeIcon(m.recipe)}
            </div>
            <div className="row__text">
              <span className="row__name">{m.model_name}</span>
              <span className="row__sub">
                {recipeLabel(m.recipe)} · {m.device.toUpperCase()}
                {type !== 'llm' && ` · ${type}`}
              </span>
            </div>
          </div>
          <div className="row__right">
            <span className="row__status-pill row__status-pill--running">
              <span className="row__pulse" /> Running
            </span>
            {type === 'llm' && !isActive && (
              <button className="row__action" onClick={(e) => { e.stopPropagation(); onModelSelect(m.model_name); }}>
                Use for Chat
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
                const info = models.find(mi => modelName(mi) === m.model_name);
                if (info) handleDelete(info);
              }}
              disabled={loadingModel === m.model_name}
              title="Delete model files"
            >
              🗑
            </button>
            <span className="row__expand">{expandedModel === m.model_name ? '▾' : '▸'}</span>
          </div>
        </div>

        {expandedModel === m.model_name && (() => {
          // find matching ModelInfo for detail
          const info = models.find(mi => modelName(mi) === m.model_name);
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
              <span className="row__name">{m.display_name || name}</span>
              <span className="row__sub">
                {recipeLabel((m as any).recipe || '')}
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
              </div>
            ) : isDownloaded ? (
              <>
                <span className="row__status-pill row__status-pill--ready">Ready</span>
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
                  title="Delete model files"
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

    // Filter to only GGUF files from siblings
    const ggufFiles = (r.siblings || [])
      .map(f => f.rfilename)
      .filter(f => f.toLowerCase().endsWith('.gguf'));

    return (
      <div className={`row row--hf${isExpanded ? ' row--expanded' : ''}`} key={r.id}>
        <div className="row__content" onClick={() => setExpandedHfModel(prev => prev === r.id ? null : r.id)}>
          <div className="row__main">
            <div className="row__icon row__icon--hf">🤗</div>
            <div className="row__text">
              <span className="row__name">{r.id}</span>
              <span className="row__sub">
                🦙 llama.cpp · {pipelineTag && `${pipelineTag} · `}
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
              </div>
            ) : ggufFiles.length > 0 ? (
              <button
                className="row__action row__action--download"
                onClick={(e) => { e.stopPropagation(); setExpandedHfModel(r.id); }}
                title="Expand to pick a GGUF file to download"
              >
                ↓ Download
              </button>
            ) : null}
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
                <div className="detail__field">
                  <span className="detail__label">Last Modified</span>
                  <span className="detail__value">{new Date(r.lastModified).toLocaleDateString()}</span>
                </div>
              </div>
              <div className="detail__source">
                {ggufFiles.length > 0 && (
                  <div className="detail__field">
                    <span className="detail__label">GGUF Files — pick one to download</span>
                    <div className="hf-detail__gguf-list">
                      {ggufFiles.map(f => (
                        <button
                          key={f}
                          className="hf-detail__gguf-btn"
                          disabled={isHfPulling}
                          onClick={() => handleHfPull(r.id, f)}
                        >
                          <span className="hf-detail__gguf-name">{f}</span>
                          <span className="hf-detail__gguf-action">↓ Download</span>
                        </button>
                      ))}
                    </div>
                  </div>
                )}
                {r.siblings && r.siblings.length > ggufFiles.length && (
                  <div className="detail__field">
                    <span className="detail__label">Other Files ({r.siblings.length - ggufFiles.length})</span>
                    <div className="hf-detail__files">
                      {r.siblings
                        .filter(f => !f.rfilename.toLowerCase().endsWith('.gguf'))
                        .slice(0, 5)
                        .map(f => (
                          <span key={f.rfilename} className="hf-detail__file">{f.rfilename}</span>
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
  const totalDownloaded = downloaded.length + loadedModels.length;
  const totalPulling = Object.keys(pulling).length;

  return (
    <div className="manager">
      <div className="manager__head">
        <div className="manager__title">
          <h1>Models</h1>
          <div className="manager__stats">
            <span className="manager__stat">
              <span className="manager__stat-num">{loadedModels.length}</span> running
            </span>
            <span className="manager__stat-sep">·</span>
            <span className="manager__stat">
              <span className="manager__stat-num">{totalDownloaded}</span> downloaded
            </span>
            <span className="manager__stat-sep">·</span>
            <span className="manager__stat">
              <span className="manager__stat-num">{models.length}</span> available
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

        {/* HuggingFace zone — always visible */}
        <section className="zone zone--hf">
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
          ) : searchQuery.trim().length >= 2 && filteredHfResults.length > 0 ? (
            filteredHfResults.map(r => renderHfRow(r))
          ) : (
            <div className="hf-zone__empty">
              <span>🤗</span>
              <span>{searchQuery.trim().length < 2 ? 'Type at least 2 characters to search HuggingFace' : 'No HuggingFace results for this query'}</span>
            </div>
          )}
        </section>

        {filteredRunning.length === 0 && filteredDownloaded.length === 0 && filteredAvailable.length === 0 && (
          <div className="manager__empty">
            <span className="manager__empty-icon">{api.isConnected ? '🤖' : '🔌'}</span>
            <p>{api.isConnected
              ? 'No models found matching your search.'
              : 'Connect to a Lemonade server to see models.'
            }</p>
          </div>
        )}
      </div>
    </div>
  );
};

export default ModelManager;
