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
  DEFAULT_PRESET, PRESET_STORE_EVENT, Preset,
  allStoredPresets, isCompatible, loadApplied, saveApplied,
  effectivePresetParamPreviewLines,
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
          className="detail-presets__linked-card"
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
            <div className="detail-presets__card-params" aria-label="Preset parameters">
              {previewLines.map(line => <span key={line} className="detail-presets__param-line">{line}</span>)}
            </div>
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

      {/* Compatible presets */}
      {compatiblePresets.length > 0 && (
        <section className="detail-presets__recommended-section" aria-label="Compatible presets">
          <h3 className="detail-presets__section-title">Compatible presets</h3>
          <div
            className="detail-presets__preset-list"
            role="listbox"
            aria-label="Compatible presets — select to attach"
          >
            {compatiblePresets.map(preset => {
              const isLinked = preset.id === linkedPresetId;
              const paramLines = effectivePresetParamPreviewLines(preset, model, undefined);
              return (
                <div
                  key={preset.id}
                  role="option"
                  aria-selected={isLinked}
                  className={`detail-presets__preset-card${isLinked ? ' detail-presets__preset-card--selected' : ''}`}
                  aria-label={`${preset.name}${isLinked ? ' (currently linked)' : ''}`}
                >
                  <div className="detail-presets__card-header">
                    <PresetIcon preset={preset} size={13} />
                    <strong className="detail-presets__card-name">{preset.name}</strong>
                    {isLinked && <span className="detail-presets__card-badge detail-presets__card-badge--linked" aria-hidden="true">Linked</span>}
                  </div>
                  {preset.description && (
                    <p className="detail-presets__card-desc">{preset.description}</p>
                  )}
                  {paramLines.length > 0 && (
                    <div className="detail-presets__card-params">
                      {paramLines.map(line => <span key={line} className="detail-presets__param-line">{line}</span>)}
                    </div>
                  )}
                  {!isLinked && (
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
              );
            })}
          </div>
        </section>
      )}

      {compatiblePresets.length === 0 && (
        <p className="detail-presets__empty">No compatible presets found. Create a preset in the Presets page and set the model type to match this model.</p>
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
  onPull: (model: ModelInfo) => void;
  onPullAndLoad: (model: ModelInfo) => void;
  onDelete: (model: ModelInfo) => void;
  onCancelPull: (name: string) => void;
  serverDefaultCtxSize: number;
  /** Called when the "Back to models" button is clicked (narrow viewports). */
  onBack?: () => void;
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
  onPull,
  onPullAndLoad,
  onDelete,
  onCancelPull,
  onBack,
}) => {
  const [activeTab, setActiveTab] = useState<DetailTab>('readme');
  const tabRefs = useRef<Array<HTMLButtonElement | null>>([]);
  const panelHeadingRef = useRef<HTMLHeadingElement>(null);

  // Move focus to heading when model changes
  useEffect(() => {
    if (model) panelHeadingRef.current?.focus();
  }, [model?.id]);

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
          <p>Select a model to view details</p>
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
            <button
              type="button"
              className="btn btn--ghost btn--sm"
              onClick={() => onUnload(loadedModel!)}
              disabled={isLoadingThis}
              aria-label={isLoadingThis ? `Working on ${name}…` : `Unload ${name}`}
            >
              {isLoadingThis ? 'Working…' : 'Unload'}
            </button>
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
