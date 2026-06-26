/**
 * ModelDetailPanel — right-side detail view for the selected model.
 * Contains: header (title, metadata, primary actions) + tablist (README / Presets / Files).
 *
 * Part of the master-detail layout introduced in #2355 Slice 1.
 */
import React, { useState, useEffect, useMemo, useRef, useCallback } from 'react';
import MarkdownIt from 'markdown-it';
import DOMPurify from 'dompurify';
import type { ModelInfo, LoadedModel } from '../api';
import { capabilityFromModelInfo, capabilityLabel } from '../modelCapabilities';
import {
  DEFAULT_PRESET, PRESET_STORE_EVENT, Preset, PresetChangeKind,
  allStoredPresets, isCompatible, loadApplied, saveApplied,
  effectivePresetParamPreviewLines, activePresetForModel,
  runningPresetIdForModel, setRunningPreset, clearRunningPreset,
  classifyPresetChange, systemPromptTextForPreset,
} from '../presetStore';
import { Icon, CapabilityIcon, PresetIcon } from './Icon';

/* ── Helpers (local copies to keep component self-contained) ──── */

function mdName(m: ModelInfo | null | undefined): string {
  if (!m) return '';
  return String((m as any).model_name ?? m.name ?? m.id ?? '').trim();
}

function fmtSize(gb: number): string {
  if (!Number.isFinite(gb) || gb <= 0) return '';
  if (gb >= 1) return `${gb.toFixed(1)} GB`;
  if (gb >= 0.01) return `${(gb * 1000).toFixed(0)} MB`;
  return '< 1 MB';
}

function recipeDisplayLabel(recipe: string): string {
  const n = String(recipe || '').toLowerCase();
  switch (n) {
    case 'llamacpp': return 'llama.cpp';
    case 'vllm': return 'vLLM';
    case 'flm': return 'FastFlowLM';
    case 'ryzenai-llm': return 'RyzenAI';
    case 'sd-cpp': return 'Stable Diffusion';
    case 'whispercpp': return 'Whisper';
    case 'moonshine': return 'Moonshine';
    case 'kokoro': return 'Kokoro TTS';
    case 'collection.omni': return 'Omni Collection';
    case 'collection': return 'Collection';
    default: return recipe || 'Unknown';
  }
}

/** Regex: only attempt HF README fetch when the derived value looks like `owner/repo`. */
const HF_REPO_RE = /^[\w.-]+\/[\w.-]+$/;

/**
 * Derive the best-effort HF repo from model.checkpoint or model.checkpoints.main
 * (falling back to the first available checkpoint value).
 * Strips the variant/file suffix after `:`.
 * Returns null if no valid `owner/repo` can be derived.
 */
function deriveHFRepo(
  checkpoint: string | null | undefined,
  checkpoints: Record<string, string> | null | undefined,
): string | null {
  const candidates: (string | undefined)[] = [
    checkpoint ?? undefined,
    checkpoints?.main,
    ...(checkpoints ? Object.values(checkpoints) : []),
  ];
  for (const c of candidates) {
    if (!c) continue;
    const repo = c.split(':')[0].trim();
    if (HF_REPO_RE.test(repo)) return repo;
  }
  return null;
}

/* ── Shared markdown-it instance for README rendering ─────────── */

// html:true is safe here because the rendered output is passed through the
// strict DOMPurify allowlist (README_PURIFY_CONFIG) below before injection.
// HF model READMEs commonly embed raw HTML (<div align="center">, <img>,
// tables, badges); with html:false markdown-it escapes those to literal text.
const readmeMd = new MarkdownIt({ html: true, linkify: true, typographer: true });

const README_PURIFY_CONFIG: DOMPurify.Config = {
  ALLOWED_TAGS: [
    'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'p', 'br', 'hr',
    'ul', 'ol', 'li', 'dl', 'dt', 'dd',
    'table', 'thead', 'tbody', 'tfoot', 'tr', 'th', 'td', 'caption', 'colgroup', 'col',
    'div', 'span', 'a', 'img', 'picture', 'source',
    'strong', 'em', 'b', 'i', 'u', 's', 'del', 'mark', 'code', 'pre',
    'sup', 'sub', 'kbd', 'samp', 'var',
    'blockquote', 'details', 'summary', 'figure', 'figcaption', 'abbr',
  ],
  ALLOWED_ATTR: ['class', 'href', 'target', 'rel', 'src', 'srcset', 'alt', 'title', 'width', 'height', 'align', 'colspan', 'rowspan'],
};

/**
 * Strip a leading YAML frontmatter block from an HF README.
 * HF READMEs begin with metadata delimited by `---` ... `---`. With html:true
 * this would otherwise render as a stray <hr> plus dumped key/value text.
 * Defensive: only strips a well-formed leading block; never throws.
 */
function stripFrontmatter(source: string): string {
  if (typeof source !== 'string') return '';
  const leading = source.replace(/^\s+/, '');
  if (!leading.startsWith('---\n') && !leading.startsWith('---\r\n')) return source;
  const lines = leading.split('\n');
  // lines[0] is the opening '---'; find the next line that is exactly '---'.
  for (let i = 1; i < lines.length; i++) {
    if (lines[i].replace(/\r$/, '').trim() === '---') {
      return lines.slice(i + 1).join('\n');
    }
  }
  // No closing delimiter found: not a well-formed block, leave untouched.
  return source;
}

const readmeCache = new Map<string, string>();

/* ── README tab ──────────────────────────────────────────────── */

const ModelReadmeTab: React.FC<{ model: ModelInfo | null | undefined; isActive: boolean }> = ({ model, isActive }) => {
  const checkpoint = model ? String((model as any).checkpoint || '') : '';
  const checkpoints = model ? ((model as any).checkpoints as Record<string, string> | null ?? null) : null;
  const hfRepo = deriveHFRepo(checkpoint || null, checkpoints);
  const readmeUrl = hfRepo ? `https://huggingface.co/${hfRepo}/raw/main/README.md` : null;

  const [readme, setReadme] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);

  useEffect(() => {
    if (!isActive) return;
    if (!readmeUrl) { setReadme(''); return; }

    const cached = readmeCache.get(readmeUrl);
    if (cached !== undefined) { setReadme(cached); return; }

    let cancelled = false;
    setLoading(true);
    fetch(readmeUrl)
      .then(r => r.ok ? r.text() : null)
      .then(text => {
        if (cancelled) return;
        const content = text || '';
        readmeCache.set(readmeUrl, content);
        setReadme(content);
      })
      .catch(() => {
        if (!cancelled) { readmeCache.set(readmeUrl, ''); setReadme(''); }
      })
      .finally(() => { if (!cancelled) setLoading(false); });

    return () => { cancelled = true; };
  }, [readmeUrl, isActive]);

  if (loading) {
    return (
      <div className="detail-tab-content detail-readme detail-readme--loading" aria-live="polite" aria-busy="true">
        <span>Loading README…</span>
      </div>
    );
  }

  if (!readme) {
    return (
      <div className="detail-tab-content detail-readme detail-readme--empty">
        <Icon name="book-open" size={32} aria-hidden="true" />
        <p>README unavailable for this model.</p>
      </div>
    );
  }

  const html = DOMPurify.sanitize(readmeMd.render(stripFrontmatter(readme)), README_PURIFY_CONFIG);

  return (
    <div
      className="detail-tab-content detail-readme"
      dangerouslySetInnerHTML={{ __html: html }}
    />
  );
};

/* ── Presets tab ─────────────────────────────────────────────── */

const ModelPresetsTab: React.FC<{
  model: ModelInfo;
  isActive: boolean;
}> = ({ model, isActive }) => {
  const name = mdName(model);
  const [allPresets, setAllPresets] = useState<Preset[]>(() => allStoredPresets());
  const [appliedPresets, setAppliedPresets] = useState<Record<string, string>>(() => loadApplied());
  const [notice, setNotice] = useState<string | null>(null);
  const [showChooser, setShowChooser] = useState(false);
  const liveRef = useRef<HTMLDivElement>(null);
  const changeBtnRef = useRef<HTMLButtonElement>(null);
  const chooserRef = useRef<HTMLDivElement>(null);

  // Reload when preset store changes
  useEffect(() => {
    const handler = () => {
      setAllPresets(allStoredPresets());
      setAppliedPresets(loadApplied());
    };
    window.addEventListener(PRESET_STORE_EVENT, handler);
    return () => window.removeEventListener(PRESET_STORE_EVENT, handler);
  }, []);

  // Close chooser when model changes
  useEffect(() => { setShowChooser(false); }, [name]);

  // Focus first focusable element in chooser when it opens
  useEffect(() => {
    if (showChooser) {
      requestAnimationFrame(() => {
        const first = chooserRef.current?.querySelector<HTMLElement>('button:not(.detail-presets__chooser-close), [tabindex="0"]');
        first?.focus();
      });
    }
  }, [showChooser]);

  const linkedPresetId = appliedPresets[name] || DEFAULT_PRESET.id;
  const linkedPreset = allPresets.find(p => p.id === linkedPresetId) || DEFAULT_PRESET;

  const compatiblePresets = useMemo(
    () => allPresets.filter(p => p.id !== DEFAULT_PRESET.id && isCompatible(p, model)),
    [allPresets, model],
  );

  const handleAttach = useCallback((preset: Preset) => {
    if (!name) return;
    setAppliedPresets(prev => {
      const next = { ...prev };
      if (preset.id === DEFAULT_PRESET.id) delete next[name];
      else next[name] = preset.id;
      saveApplied(next);
      return next;
    });
    const msg = preset.id === DEFAULT_PRESET.id
      ? `Reset to default preset for ${name}`
      : `Attached "${preset.name}" to ${name}`;
    setNotice(msg);
    setTimeout(() => setNotice(null), 2500);
  }, [name]);

  const handleAttachFromChooser = useCallback((preset: Preset) => {
    handleAttach(preset);
    setShowChooser(false);
    requestAnimationFrame(() => changeBtnRef.current?.focus());
  }, [handleAttach]);

  const handleCloseChooser = useCallback(() => {
    setShowChooser(false);
    requestAnimationFrame(() => changeBtnRef.current?.focus());
  }, []);

  const navigateToPresets = useCallback(() => {
    // Client-local deep-link to the global Presets page (no lemond involvement).
    window.dispatchEvent(new CustomEvent('lemonade:navigate', { detail: { view: 'presets' } }));
  }, []);

  if (!isActive) return null;

  const previewLines = effectivePresetParamPreviewLines(linkedPreset, model, undefined);

  return (
    <div className="detail-tab-content detail-presets">
      {/* Always-present live region for attachment announcements */}
      <div ref={liveRef} role="status" aria-live="polite" aria-atomic="true" className="sr-only">
        {notice || ''}
      </div>

      {/* Linked preset */}
      <section className="detail-presets__linked-section" aria-label="Linked preset">
        <h3 className="detail-presets__section-title">Linked preset</h3>
        <div
          className="detail-presets__card detail-presets__linked-card"
          aria-current="true"
          aria-label={`Active preset: ${linkedPreset.name}`}
        >
          <div className="detail-presets__card-header">
            <PresetIcon preset={linkedPreset} size={14} />
            <strong className="detail-presets__card-name">{linkedPreset.name}</strong>
            <span className="detail-presets__card-badge detail-presets__card-badge--linked">Active</span>
          </div>
          {linkedPreset.description && (
            <p className="detail-presets__card-desc">{linkedPreset.description}</p>
          )}
          {previewLines.length > 0 && (
            <p className="detail-presets__card-meta" aria-label="Preset parameters">
              {previewLines.join(' · ')}
            </p>
          )}
          {linkedPreset.id !== DEFAULT_PRESET.id && (
            <div className="detail-presets__linked-actions">
              <button
                ref={changeBtnRef}
                type="button"
                className="btn btn--primary btn--tiny detail-presets__change-btn"
                onClick={() => setShowChooser(v => !v)}
                aria-label={`Change linked preset for ${name}`}
                aria-expanded={showChooser}
                aria-haspopup="dialog"
              >
                Change
              </button>
              <button
                type="button"
                className="btn btn--ghost btn--tiny detail-presets__detach-btn"
                onClick={() => handleAttach(DEFAULT_PRESET)}
                aria-label={`Detach preset "${linkedPreset.name}" from ${name}, reset to default`}
              >
                Reset to default
              </button>
            </div>
          )}
        </div>

        {/* Inline change-preset chooser */}
        {showChooser && (
          <div
            ref={chooserRef}
            className="detail-presets__change-chooser"
            role="dialog"
            aria-label="Switch linked preset"
            aria-modal="true"
          >
            <div className="detail-presets__chooser-head">
              <span className="detail-presets__chooser-title">Switch to a different preset</span>
              <button
                type="button"
                className="detail-presets__chooser-close btn btn--ghost btn--tiny"
                onClick={handleCloseChooser}
                aria-label="Close preset chooser"
              >
                <Icon name="x" size={12} />
              </button>
            </div>
            {compatiblePresets.filter(p => p.id !== linkedPresetId).length === 0 ? (
              <p className="detail-presets__chooser-empty">
                {compatiblePresets.length === 0
                  ? 'No compatible presets available. Create one in the Presets page.'
                  : 'No other compatible presets to switch to.'}
              </p>
            ) : (
              <ul className="detail-presets__chooser-list" role="listbox" aria-label="Select a preset to switch to">
                {compatiblePresets
                  .filter(p => p.id !== linkedPresetId)
                  .map(preset => (
                    <li key={preset.id} role="option" aria-selected={false}>
                      <button
                        type="button"
                        className="detail-presets__chooser-option"
                        onClick={() => handleAttachFromChooser(preset)}
                        aria-label={`Switch to preset "${preset.name}"`}
                      >
                        <PresetIcon preset={preset} size={12} />
                        <span className="detail-presets__chooser-option-name">{preset.name}</span>
                        {preset.description && (
                          <span className="detail-presets__chooser-option-desc">{preset.description}</span>
                        )}
                      </button>
                    </li>
                  ))}
              </ul>
            )}
          </div>
        )}
      </section>

      {/* Recommended / compatible presets — a neat grid of compact cards */}
      {compatiblePresets.length > 0 && (
        <section className="detail-presets__recommended-section" aria-label="Recommended presets">
          <div className="detail-presets__section-head">
            <h3 className="detail-presets__section-title">Recommended presets</h3>
            <button
              type="button"
              className="btn btn--ghost btn--tiny detail-presets__browse-btn"
              onClick={navigateToPresets}
              aria-label="Browse all presets in the Presets page"
            >
              Browse presets
            </button>
          </div>
          <ul
            className="detail-presets__preset-grid"
            role="list"
            aria-label="Recommended presets — select to attach"
          >
            {compatiblePresets.map(preset => {
              const isLinked = preset.id === linkedPresetId;
              const paramLines = effectivePresetParamPreviewLines(preset, model, undefined);
              return (
                <li
                  key={preset.id}
                  aria-current={isLinked ? 'true' : undefined}
                  className={`detail-presets__card detail-presets__preset-card detail-presets__preset-card--sm${isLinked ? ' detail-presets__preset-card--selected' : ''}`}
                  aria-label={`${preset.name}${isLinked ? ' (currently linked)' : ''}`}
                >
                  <div className="detail-presets__card-header">
                    <PresetIcon preset={preset} size={13} />
                    <strong className="detail-presets__card-name">{preset.name}</strong>
                    {isLinked && <span className="detail-presets__card-badge detail-presets__card-badge--linked">Linked</span>}
                  </div>
                  {preset.description && (
                    <p className="detail-presets__card-desc">{preset.description}</p>
                  )}
                  {paramLines.length > 0 && (
                    <p className="detail-presets__card-meta">{paramLines.join(' · ')}</p>
                  )}
                  <div className="detail-presets__card-footer">
                    {isLinked ? (
                      <span className="detail-presets__card-linked-note">Currently linked</span>
                    ) : (
                      <button
                        type="button"
                        className="btn btn--primary btn--tiny detail-presets__attach-btn"
                        onClick={() => handleAttach(preset)}
                        aria-label={`${linkedPreset.id !== DEFAULT_PRESET.id ? 'Switch to' : 'Attach'} preset "${preset.name}" for ${name}`}
                      >
                        {linkedPreset.id !== DEFAULT_PRESET.id ? 'Switch' : 'Attach'}
                      </button>
                    )}
                  </div>
                </li>
              );
            })}
          </ul>
        </section>
      )}

      {compatiblePresets.length === 0 && (
        <div className="detail-presets__empty-block">
          <p className="detail-presets__empty">No compatible presets found. Create a preset in the Presets page and set the model type to match this model.</p>
          <button
            type="button"
            className="btn btn--ghost btn--tiny detail-presets__browse-btn"
            onClick={navigateToPresets}
            aria-label="Manage presets in the Presets page"
          >
            Manage presets
          </button>
        </div>
      )}
    </div>
  );
};

/* ── Files tab stub ──────────────────────────────────────────── */

const ModelFilesTab: React.FC = () => (
  <div className="detail-tab-content detail-files detail-files--stub">
    <Icon name="hard-drive" size={28} aria-hidden="true" />
    <p>Files tab — coming in a future update.</p>
    <small>Requires a <code>GET /api/v1/models/&#123;id&#125;/files</code> endpoint in lemond.</small>
  </div>
);

/* ── ModelDetailPanel ────────────────────────────────────────── */

export interface ModelDetailPanelProps {
  model: ModelInfo | null;
  loadedModel: LoadedModel | null;
  loadingModel: string | null;
  pulling: Record<string, number>;
  loadError: { modelName: string; message: string } | null;
  onLoad: (model: ModelInfo) => void;
  onUnload: (model: LoadedModel) => void;
  /**
   * Apply a newly-linked preset to an already-loaded model (#2356).
   * `mode` is the UI's live-vs-reload classification; `payload` carries the
   * relevant fields (sampling/system_prompt for live, recipe_options for reload).
   * Resolves once lemond reports the update applied (or the reload completed).
   */
  onUpdatePreset?: (
    model: LoadedModel,
    presetId: string,
    mode: 'live' | 'reload',
    payload: Record<string, unknown>,
  ) => Promise<void>;
  onPull: (model: ModelInfo) => void;
  onPullAndLoad: (model: ModelInfo) => void;
  onDelete: (model: ModelInfo) => void;
  onCancelPull: (name: string) => void;
  serverDefaultCtxSize: number;
  /** Whether this model is currently marked a favorite (client-local pin store). */
  isFavorite?: boolean;
  /** Toggle this model's favorite/pin state. Receives the model name. */
  onToggleFavorite?: (name: string) => void;
  /** Called when the "Back to models" button is clicked (narrow viewports). */
  onBack?: () => void;
  /** True when the registry has no models at all (empty state guidance differs
      from the normal "nothing selected yet" copy). */
  noModelsAvailable?: boolean;
}

type DetailTab = 'readme' | 'presets' | 'files';

const TABS: Array<{ id: DetailTab; label: string }> = [
  { id: 'readme', label: 'README' },
  { id: 'presets', label: 'Presets' },
  { id: 'files', label: 'Files' },
];

export const ModelDetailPanel: React.FC<ModelDetailPanelProps> = ({
  model,
  loadedModel,
  loadingModel,
  pulling,
  loadError,
  onLoad,
  onUnload,
  onUpdatePreset,
  onPull,
  onPullAndLoad,
  onDelete,
  onCancelPull,
  isFavorite = false,
  onToggleFavorite,
  onBack,
  noModelsAvailable = false,
}) => {
  const [activeTab, setActiveTab] = useState<DetailTab>('readme');
  const tabRefs = useRef<Array<HTMLButtonElement | null>>([]);
  const panelHeadingRef = useRef<HTMLHeadingElement>(null);
  const updateBtnRef = useRef<HTMLButtonElement>(null);
  const unloadBtnRef = useRef<HTMLButtonElement>(null);

  // ── Update-preset-while-loaded state (#2356) ──────────────────────────────
  // storeTick forces a re-read of the applied/running preset stores whenever
  // they change (e.g. the user re-links a preset in the Presets tab).
  const [storeTick, setStoreTick] = useState(0);
  type UpdatePhase = 'idle' | 'live' | 'reload' | 'done-live' | 'done-reload' | 'error';
  const [updateStatus, setUpdateStatus] = useState<{ phase: UpdatePhase; msg: string }>({ phase: 'idle', msg: '' });

  const detailName = model ? mdName(model) : '';
  const detailLoaded = !!loadedModel;

  // Move focus to heading when model changes
  useEffect(() => {
    if (model) panelHeadingRef.current?.focus();
  }, [model?.id]);

  // Re-render when the preset store changes (applied/running/user presets).
  useEffect(() => {
    const handler = () => setStoreTick(t => t + 1);
    window.addEventListener(PRESET_STORE_EVENT, handler);
    return () => window.removeEventListener(PRESET_STORE_EVENT, handler);
  }, []);

  // Snapshot the running preset when a model becomes loaded; clear it when it
  // unloads. The snapshot baseline = the preset linked at the moment we first
  // observe the model loaded, so later re-links diverge and surface "Update preset".
  useEffect(() => {
    if (!detailName) return;
    if (detailLoaded) {
      if (runningPresetIdForModel(detailName) === undefined) {
        setRunningPreset(detailName, activePresetForModel(detailName).id);
      }
    } else if (runningPresetIdForModel(detailName) !== undefined) {
      clearRunningPreset(detailName);
    }
  }, [detailName, detailLoaded, storeTick]);

  // Reset transient update feedback when the selected model changes.
  useEffect(() => { setUpdateStatus({ phase: 'idle', msg: '' }); }, [model?.id]);

  // Auto-dismiss terminal update messages so the live region settles.
  useEffect(() => {
    if (['done-live', 'done-reload', 'error'].includes(updateStatus.phase)) {
      const t = window.setTimeout(() => setUpdateStatus({ phase: 'idle', msg: '' }), 6000);
      return () => window.clearTimeout(t);
    }
  }, [updateStatus]);

  const handleUpdatePreset = useCallback(async () => {
    if (!model || !loadedModel || !onUpdatePreset) return;
    const targetName = mdName(model);
    const linked = activePresetForModel(targetName);
    const runId = runningPresetIdForModel(targetName);
    const running = runId ? (allStoredPresets().find(p => p.id === runId) ?? null) : null;
    const kind = classifyPresetChange(running, linked);
    if (kind === 'none') return;

    if (kind === 'live') {
      setUpdateStatus({ phase: 'live', msg: `Applying preset “${linked.name}” to ${targetName}…` });
      try {
        await onUpdatePreset(loadedModel, linked.id, 'live', {
          sampling: linked.sampling,
          system_prompt: systemPromptTextForPreset(linked),
        });
        setRunningPreset(targetName, linked.id);
        setUpdateStatus({ phase: 'done-live', msg: `Preset updated to “${linked.name}” — applied live, no reload needed.` });
        requestAnimationFrame(() => unloadBtnRef.current?.focus());
      } catch {
        setUpdateStatus({ phase: 'error', msg: `Couldn’t update the preset for ${targetName}. Please try again.` });
        requestAnimationFrame(() => updateBtnRef.current?.focus());
      }
    } else {
      setUpdateStatus({ phase: 'reload', msg: `Reloading ${targetName} with preset “${linked.name}”…` });
      try {
        await onUpdatePreset(loadedModel, linked.id, 'reload', {
          recipe_options: linked.recipe_options,
        });
        setRunningPreset(targetName, linked.id);
        setUpdateStatus({ phase: 'done-reload', msg: `Preset updated to “${linked.name}” — model reloaded.` });
        requestAnimationFrame(() => unloadBtnRef.current?.focus());
      } catch {
        setUpdateStatus({ phase: 'error', msg: `Couldn’t reload ${targetName} with the new preset. Please try again.` });
        requestAnimationFrame(() => updateBtnRef.current?.focus());
      }
    }
  }, [model, loadedModel, onUpdatePreset]);

  // Roving tabindex: keyboard navigation across tabs
  const handleTabKeyDown = (e: React.KeyboardEvent<HTMLButtonElement>, index: number) => {
    const count = TABS.length;
    let next = -1;
    if (e.key === 'ArrowRight') { e.preventDefault(); next = (index + 1) % count; }
    else if (e.key === 'ArrowLeft') { e.preventDefault(); next = (index - 1 + count) % count; }
    else if (e.key === 'Home') { e.preventDefault(); next = 0; }
    else if (e.key === 'End') { e.preventDefault(); next = count - 1; }
    if (next >= 0) {
      setActiveTab(TABS[next].id);
      tabRefs.current[next]?.focus();
    }
  };

  if (!model) {
    return (
      <div className="model-detail-panel model-detail-panel--empty" aria-label="Model detail">
        {onBack && (
          <button
            type="button"
            className="model-detail-panel__back-btn"
            onClick={onBack}
            aria-label="Back to models list"
          >
            ← Back to models
          </button>
        )}
        <div className="model-detail-panel__placeholder">
          <Icon name="model" size={40} aria-hidden="true" />
          <p>{noModelsAvailable ? 'No models found' : 'No model selected'}</p>
          <p className="model-detail-panel__placeholder-sub">
            {noModelsAvailable
              ? 'No models are available in the registry yet. Pull a model or adjust your filters to get started.'
              : 'Select a model from the list to view its details.'}
          </p>
        </div>
      </div>
    );
  }

  const name = mdName(model);
  const recipe = String((model as any).recipe || '');
  const checkpoint = String((model as any).checkpoint || '');
  const checkpoints = (model as any).checkpoints as Record<string, string> | null ?? null;
  const hfRepo = deriveHFRepo(checkpoint || null, checkpoints);
  const isLoaded = !!loadedModel;
  const isLoadingThis = loadingModel === name;
  const isPulling = pulling[name] !== undefined;
  const pullPct = pulling[name] ?? 0;
  const isDownloaded = Boolean((model as any).downloaded);
  const cap = capabilityFromModelInfo(model);

  // ── Update-preset-while-loaded derivation (#2356) ─────────────────────────
  // Reference storeTick so this recomputes when the preset store changes.
  void storeTick;
  const linkedPreset = activePresetForModel(name);
  const runningPresetId = isLoaded ? runningPresetIdForModel(name) : undefined;
  const runningPreset = runningPresetId
    ? (allStoredPresets().find(p => p.id === runningPresetId) ?? null)
    : null;
  const presetChangeKind: PresetChangeKind = isLoaded && runningPreset
    ? classifyPresetChange(runningPreset, linkedPreset)
    : 'none';
  const isUpdatingPreset = updateStatus.phase === 'live' || updateStatus.phase === 'reload';
  const canUpdatePreset = isLoaded && !!onUpdatePreset && presetChangeKind !== 'none' && !isUpdatingPreset && !isLoadingThis;

  return (
    <div className="model-detail-panel" role="region" aria-label={`Model details: ${name}`}>

      {/* Back button for narrow viewports */}
      {onBack && (
        <button
          type="button"
          className="model-detail-panel__back-btn"
          onClick={onBack}
          aria-label="Back to models list"
        >
          ← Back to models
        </button>
      )}

      {/* Header */}
      <div className="model-detail-panel__head">
        <h2
          className="model-detail-panel__name"
          ref={panelHeadingRef}
          tabIndex={-1}
          id="detail-panel-heading"
        >
          {model.display_name || name}
        </h2>

        {/* Metadata row */}
        <div className="model-detail-panel__meta">
          {recipe && (
            <span className="model-detail-panel__badge model-detail-panel__badge--recipe">
              {recipeDisplayLabel(recipe)}
            </span>
          )}
          {model.size != null && model.size > 0 && (
            <span className="model-detail-panel__badge">{fmtSize(model.size)}</span>
          )}
          {cap && (
            <span className="model-detail-panel__badge model-detail-panel__badge--cap">
              <CapabilityIcon capability={cap} size={11} />
              {capabilityLabel(cap)}
            </span>
          )}
          {isLoaded && (
            <span className="model-detail-panel__status model-detail-panel__status--running">
              <span className="row__pulse" aria-hidden="true" /> Running
            </span>
          )}
          {isDownloaded && !isLoaded && (
            <span className="model-detail-panel__status model-detail-panel__status--ready">Ready</span>
          )}
        </div>

        {/* Primary actions */}
        <div className="model-detail-panel__actions" aria-label={`Actions for ${name}`}>
          {onToggleFavorite && (
            <button
              type="button"
              className={`model-detail-panel__fav-btn${isFavorite ? ' model-detail-panel__fav-btn--on' : ''}`}
              onClick={() => onToggleFavorite(name)}
              aria-pressed={isFavorite}
              aria-label={isFavorite ? `Remove ${name} from favorites` : `Add ${name} to favorites`}
              title={isFavorite ? 'Remove from favorites' : 'Add to favorites'}
            >
              <span aria-hidden="true" className="model-detail-panel__fav-icon">{isFavorite ? '★' : '☆'}</span>
            </button>
          )}
          {isPulling ? (
            <>
              <div className="row__progress">
                <div className="row__progress-bar">
                  <div className="row__progress-fill" style={{ width: `${pullPct}%` }} />
                </div>
                <span className="row__progress-text">{pullPct.toFixed(0)}%</span>
              </div>
              <button
                type="button"
                className="btn btn--ghost btn--sm"
                onClick={() => onCancelPull(name)}
                aria-label={`Cancel download of ${name}`}
              >
                Cancel
              </button>
            </>
          ) : isLoaded ? (
            <>
              {/* Update preset (#2356): appears next to Unload when a different
                  preset has been linked to this loaded model. */}
              {(canUpdatePreset || isUpdatingPreset) && (
                <button
                  ref={updateBtnRef}
                  type="button"
                  className="btn btn--primary btn--sm model-detail-panel__update-preset-btn"
                  onClick={handleUpdatePreset}
                  disabled={isUpdatingPreset || !canUpdatePreset}
                  aria-busy={isUpdatingPreset}
                  aria-label={
                    isUpdatingPreset
                      ? (updateStatus.phase === 'reload' ? `Reloading ${name} with new preset…` : `Updating preset for ${name}…`)
                      : (presetChangeKind === 'reload'
                        ? `Update preset for ${name} (reloads the model)`
                        : `Update preset for ${name}`)
                  }
                >
                  <Icon name="rotate-ccw" size={13} aria-hidden="true" />{' '}
                  {isUpdatingPreset
                    ? (updateStatus.phase === 'reload' ? 'Reloading…' : 'Updating…')
                    : 'Update preset'}
                </button>
              )}
              <button
                ref={unloadBtnRef}
                type="button"
                className="btn btn--ghost btn--sm"
                onClick={() => onUnload(loadedModel!)}
                disabled={isLoadingThis || isUpdatingPreset}
                aria-label={isLoadingThis ? `Working on ${name}…` : `Unload ${name}`}
              >
                {isLoadingThis ? 'Working…' : 'Unload'}
              </button>
            </>
          ) : isDownloaded ? (
            <button
              type="button"
              className="btn btn--primary btn--sm"
              onClick={() => onLoad(model)}
              disabled={isLoadingThis}
              aria-label={isLoadingThis ? `Loading ${name}…` : `Load ${name}`}
            >
              {isLoadingThis ? 'Loading…' : <><Icon name="play" size={13} /> Load</>}
            </button>
          ) : (
            <>
              <button
                type="button"
                className="btn btn--ghost btn--sm"
                onClick={() => onPull(model)}
                aria-label={`Download ${name}`}
              >
                <Icon name="download" size={13} /> Download
              </button>
              <button
                type="button"
                className="btn btn--primary btn--sm"
                onClick={() => onPullAndLoad(model)}
                aria-label={`Get and load ${name}`}
              >
                <Icon name="download" size={13} /> Get & Load
              </button>
            </>
          )}
          {(isDownloaded || isLoaded) && (
            <button
              type="button"
              className="btn btn--ghost btn--sm btn--danger"
              onClick={() => onDelete(model)}
              disabled={isLoadingThis}
              aria-label={(model as any).custom ? `Delete custom model definition for ${name}` : `Delete downloaded files for ${name}`}
            >
              <Icon name="trash" size={13} />
            </button>
          )}
        </div>

        {/* Load error */}
        {loadError?.modelName === name && (
          <div className="model-detail-panel__error" role="alert">
            <Icon name="alert" size={13} /> {loadError.message}
          </div>
        )}

        {/* Update-preset feedback + live region (#2356).
            Always-present polite live region so screen readers announce the
            live-apply / reload outcome; a visible pill mirrors it sighted. */}
        <div
          className={`model-detail-panel__preset-update${updateStatus.phase !== 'idle' ? ' model-detail-panel__preset-update--active' : ''}`}
          role="status"
          aria-live="polite"
          aria-atomic="true"
          data-preset-update-phase={updateStatus.phase}
        >
          {updateStatus.phase === 'reload' && (
            <span className="model-detail-panel__preset-update-spinner" aria-hidden="true" />
          )}
          {updateStatus.msg}
        </div>

        {/* Sighted hint explaining why Update preset appeared (not announced —
            the button's accessible name already conveys the reload semantics). */}
        {canUpdatePreset && updateStatus.phase === 'idle' && (
          <p className="model-detail-panel__preset-update-hint" aria-hidden="true">
            {presetChangeKind === 'reload'
              ? 'A different preset is linked. Updating will reload the model to apply it.'
              : 'A different preset is linked. Updating applies it live — no reload needed.'}
          </p>
        )}

        {/* HF link */}
        {hfRepo && (
          <a
            className="model-detail-panel__hf-link"
            href={`https://huggingface.co/${hfRepo}`}
            target="_blank"
            rel="noopener noreferrer"
            aria-label={`View ${name} on Hugging Face (opens in new tab)`}
          >
            <Icon name="globe" size={12} /> Hugging Face
          </a>
        )}
      </div>

      {/* Tablist */}
      <div
        className="detail-tabs__tablist"
        role="tablist"
        aria-label="Model details sections"
        aria-labelledby="detail-panel-heading"
      >
        {TABS.map((tab, i) => (
          <button
            key={tab.id}
            ref={el => { tabRefs.current[i] = el; }}
            role="tab"
            id={`detail-tab-${tab.id}`}
            aria-selected={activeTab === tab.id}
            aria-controls={`detail-panel-${tab.id}`}
            tabIndex={activeTab === tab.id ? 0 : -1}
            className={`detail-tabs__tab${activeTab === tab.id ? ' detail-tabs__tab--active' : ''}`}
            onClick={() => setActiveTab(tab.id)}
            onKeyDown={e => handleTabKeyDown(e, i)}
          >
            {tab.label}
          </button>
        ))}
      </div>

      {/* Tab panels */}
      {TABS.map(tab => (
        <div
          key={tab.id}
          id={`detail-panel-${tab.id}`}
          role="tabpanel"
          aria-labelledby={`detail-tab-${tab.id}`}
          className={`detail-tabs__panel${activeTab === tab.id ? ' detail-tabs__panel--active' : ''}`}
          hidden={activeTab !== tab.id}
        >
          {tab.id === 'readme' && (
            <ModelReadmeTab model={model} isActive={activeTab === 'readme'} />
          )}
          {tab.id === 'presets' && (
            <ModelPresetsTab model={model} isActive={activeTab === 'presets'} />
          )}
          {tab.id === 'files' && <ModelFilesTab />}
        </div>
      ))}
    </div>
  );
};

export default ModelDetailPanel;
