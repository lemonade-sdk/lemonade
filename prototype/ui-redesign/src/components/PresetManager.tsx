import React, { useState, useEffect, useCallback, useMemo } from 'react';
import { LoadedModel } from '../api';

/* ── Data model ────────────────────────────────────────────── */

interface Preset {
  id: string;
  name: string;
  description: string;
  applies_to: string[];
  options: {
    ctx_size?: number;
    backend?: string;
    steps?: number;
    cfg_scale?: number;
  };
  sampling: {
    temperature?: number;
    top_p?: number;
    top_k?: number;
    repeat_penalty?: number;
  };
  starter: boolean;
}

/* ── Constants ─────────────────────────────────────────────── */

const STARTERS: Preset[] = [
  { id: 's-balanced', name: 'Balanced', description: 'Sensible defaults. Good first pick for everyday chat.', applies_to: ['chat'], options: { ctx_size: 4096, backend: 'llamacpp · Vulkan' }, sampling: { temperature: 0.70, top_p: 0.90, top_k: 40, repeat_penalty: 1.05 }, starter: true },
  { id: 's-quality', name: 'Quality', description: 'Larger context, slightly looser sampling for richer long-form answers.', applies_to: ['chat'], options: { ctx_size: 8192, backend: 'llamacpp · Vulkan' }, sampling: { temperature: 0.70, top_p: 0.95, top_k: 40, repeat_penalty: 1.10 }, starter: true },
  { id: 's-fast', name: 'Fast', description: 'Small context, tight sampling. Snappy responses for quick interactions.', applies_to: ['chat'], options: { ctx_size: 2048, backend: 'llamacpp · Vulkan' }, sampling: { temperature: 0.60, top_p: 0.80, top_k: 40, repeat_penalty: 1.05 }, starter: true },
  { id: 's-creative', name: 'Creative', description: 'Higher temperature for brainstorming, dialog, and divergent thinking.', applies_to: ['chat'], options: { ctx_size: 8192, backend: 'llamacpp · Vulkan' }, sampling: { temperature: 0.95, top_p: 0.95, top_k: 60, repeat_penalty: 1.00 }, starter: true },
  { id: 's-long-context', name: 'Long Context', description: 'For documents, codebases, and long conversation threads.', applies_to: ['chat'], options: { ctx_size: 32768, backend: 'llamacpp · Vulkan' }, sampling: { temperature: 0.70, top_p: 0.90, top_k: 40, repeat_penalty: 1.05 }, starter: true },
  { id: 's-code', name: 'Code', description: 'Low temperature, tight sampling for code generation and refactoring.', applies_to: ['chat'], options: { ctx_size: 8192, backend: 'llamacpp · Vulkan' }, sampling: { temperature: 0.20, top_p: 0.95, top_k: 40, repeat_penalty: 1.05 }, starter: true },
  { id: 's-sharp', name: 'Sharp', description: 'More steps and tighter guidance for crisp, deliberate image generation.', applies_to: ['image'], options: { steps: 30, cfg_scale: 8.0 }, sampling: {}, starter: true },
  { id: 's-quick', name: 'Quick', description: 'Fewer steps, looser guidance — fast drafts and iteration.', applies_to: ['image'], options: { steps: 15, cfg_scale: 7.0 }, sampling: {}, starter: true },
];

const DEFAULT_USER_PRESETS: Preset[] = [
  { id: 'u-long-code', name: 'Long Code', description: 'Custom: big context + code-style sampling for monorepo work.', applies_to: ['chat'], options: { ctx_size: 16384, backend: 'llamacpp · Vulkan' }, sampling: { temperature: 0.25, top_p: 0.95, top_k: 40, repeat_penalty: 1.04 }, starter: false },
  { id: 'u-brainstorm', name: 'Brainstorm', description: 'High-temp, wide top_p for ideation sessions and divergent thinking.', applies_to: ['chat'], options: { ctx_size: 4096, backend: 'llamacpp · Vulkan' }, sampling: { temperature: 1.05, top_p: 0.98, top_k: 80, repeat_penalty: 1.00 }, starter: false },
];

const LS_USER_PRESETS = 'lemonade_user_presets';
const LS_APPLIED_PRESETS = 'lemonade_applied_presets';

const BACKEND_OPTIONS = [
  'llamacpp · CPU',
  'llamacpp · Vulkan',
  'llamacpp · ROCm',
  'llamacpp · Metal',
];

const ALL_CAPABILITIES = ['chat', 'vision', 'code', 'embedding', 'reranking', 'image', 'edit', 'transcription', 'tts'];

/* ── Helpers ────────────────────────────────────────────────── */

function loadUserPresets(): Preset[] {
  try {
    const raw = localStorage.getItem(LS_USER_PRESETS);
    if (raw) return JSON.parse(raw);
  } catch {}
  return [...DEFAULT_USER_PRESETS];
}

function saveUserPresets(presets: Preset[]): void {
  localStorage.setItem(LS_USER_PRESETS, JSON.stringify(presets));
}

function loadApplied(): Record<string, string> {
  try {
    const raw = localStorage.getItem(LS_APPLIED_PRESETS);
    if (raw) return JSON.parse(raw);
  } catch {}
  return {};
}

function saveApplied(applied: Record<string, string>): void {
  localStorage.setItem(LS_APPLIED_PRESETS, JSON.stringify(applied));
}

function primaryCap(preset: Preset): string {
  return preset.applies_to[0] || 'chat';
}

function paramsPreview(preset: Preset): string {
  if (primaryCap(preset) === 'image') {
    const s = preset.options.steps ?? '—';
    const c = preset.options.cfg_scale != null ? preset.options.cfg_scale.toFixed(1) : '—';
    return `steps ${s} · cfg ${c}`;
  }
  const t = preset.sampling?.temperature != null ? preset.sampling.temperature.toFixed(2) : '—';
  const ctx = preset.options.ctx_size ?? '—';
  return `temp ${t} · ctx ${ctx}`;
}

/* ── Phase glyph SVG ───────────────────────────────────────── */

const PhaseGlyph: React.FC<{ size?: 'sm' | 'lg' | 'xl' }> = ({ size }) => {
  const cls = size === 'lg' ? 'phase-glyph phase-glyph--lg'
    : size === 'xl' ? 'phase-glyph phase-glyph--xl'
    : 'phase-glyph';
  const px = size === 'xl' ? 48 : size === 'lg' ? 22 : 14;
  return (
    <span className={cls} aria-hidden="true">
      <svg width={px} height={px} viewBox="0 0 12 12" fill="none">
        <circle cx="6" cy="6" r="5" stroke="currentColor" strokeWidth="1.3" />
        <path d="M6 1 A5 5 0 0 1 6 11 Z" fill="currentColor" />
      </svg>
    </span>
  );
};

/* ── Capability chip ───────────────────────────────────────── */

const CapChip: React.FC<{
  cap: string;
  isOn?: boolean;
  disabled?: boolean;
  onClick?: () => void;
}> = ({ cap, isOn, disabled, onClick }) => {
  let cls = `cap-chip cap-chip--${cap}`;
  if (isOn !== undefined) cls += isOn ? ' is-on' : ' is-off';
  return (
    <button
      className={cls}
      type="button"
      disabled={disabled}
      onClick={onClick}
      data-cap-chip={cap}
    >
      <span className="cap-chip__dot" aria-hidden="true" />
      {cap}
    </button>
  );
};

/* ── Main component ────────────────────────────────────────── */

interface PresetManagerProps {
  loadedModels: LoadedModel[];
}

const PresetManager: React.FC<PresetManagerProps> = ({ loadedModels }) => {
  const [userPresets, setUserPresets] = useState<Preset[]>(loadUserPresets);
  const [appliedPresets, setAppliedPresets] = useState<Record<string, string>>(loadApplied);
  const [selectedPreset, setSelectedPreset] = useState<Preset | null>(null);
  const [importOpen, setImportOpen] = useState(false);
  const [applyTarget, setApplyTarget] = useState('');

  // Persist on change
  useEffect(() => { saveUserPresets(userPresets); }, [userPresets]);
  useEffect(() => { saveApplied(appliedPresets); }, [appliedPresets]);

  const allPresets = useMemo(() => [...STARTERS, ...userPresets], [userPresets]);

  const lookupPreset = useCallback((id: string) =>
    allPresets.find(p => p.id === id) || null
  , [allPresets]);

  /* ── Actions ─────────────────────────────────────────────── */

  const handleNewPreset = useCallback(() => {
    alert('New Preset — coming soon! This will open a preset editor.');
  }, []);

  const handleImportFile = useCallback(() => {
    alert('Import from file — coming soon!');
    setImportOpen(false);
  }, []);

  const handleImportClipboard = useCallback(() => {
    alert('Import from clipboard — coming soon!');
    setImportOpen(false);
  }, []);

  const handleClone = useCallback((preset: Preset) => {
    alert(`Cloned "${preset.name}" — coming soon!`);
  }, []);

  const handleExport = useCallback((preset: Preset) => {
    alert(`Export "${preset.name}" — coming soon!`);
  }, []);

  const handleDelete = useCallback((preset: Preset) => {
    setUserPresets(prev => prev.filter(p => p.id !== preset.id));
    // Remove any applied bindings for this preset
    setAppliedPresets(prev => {
      const next = { ...prev };
      for (const [model, pid] of Object.entries(next)) {
        if (pid === preset.id) delete next[model];
      }
      return next;
    });
    setSelectedPreset(null);
  }, []);

  const handleApply = useCallback((presetId: string, modelName: string) => {
    if (!modelName) return;
    setAppliedPresets(prev => ({ ...prev, [modelName]: presetId }));
    alert(`Applied preset to ${modelName}`);
  }, []);

  const handleDetach = useCallback((modelName: string) => {
    setAppliedPresets(prev => {
      const next = { ...prev };
      delete next[modelName];
      return next;
    });
  }, []);

  const openSlideover = useCallback((preset: Preset) => {
    setSelectedPreset(preset);
    setApplyTarget('');
  }, []);

  const closeSlideover = useCallback(() => {
    setSelectedPreset(null);
  }, []);

  /* ── Model names for applied list ────────────────────────── */
  const modelNames = useMemo(() => {
    if (loadedModels.length > 0) return loadedModels.map(m => m.model_name);
    return Object.keys(appliedPresets);
  }, [loadedModels, appliedPresets]);

  /* ── Render ──────────────────────────────────────────────── */

  return (
    <>
      <section className="recipes" data-view="presets">
        {/* ── Header ──────────────────────────────────── */}
        <div className="recipes__head">
          <div className="recipes__title">
            <h1>Presets</h1>
            <span className="recipes__title-sub" data-recipes-count>
              {STARTERS.length} starters · {userPresets.length} yours
            </span>
          </div>
          <div className="recipes__actions">
            <button className="btn btn--primary" onClick={handleNewPreset}>
              + New Preset
            </button>
            <div className="dropdown">
              <button
                className="btn btn--ghost dropdown__trigger"
                onClick={() => setImportOpen(!importOpen)}
              >
                + Import <span className="dropdown__caret">▾</span>
              </button>
              <div className="dropdown__menu" hidden={!importOpen}>
                <button className="dropdown__item" onClick={handleImportFile}>
                  From file…
                </button>
                <button className="dropdown__item" onClick={handleImportClipboard}>
                  From clipboard
                </button>
              </div>
            </div>
          </div>
        </div>

        {/* ── Body ────────────────────────────────────── */}
        <div className="recipes__body">
          <p className="recipes__lede">
            Presets are portable bundles of <em>options</em> (applied when a model
            loads) and <em>sampling</em> (applied to each request), keyed by
            capability — so a chat preset fits any chat-capable model, an image
            preset fits any image model, and so on.
          </p>

          {/* Zone 1 — Bundled starters */}
          <div className="zone">
            <div className="zone__head">
              <span className="zone__dot zone__dot--ready" />
              <span className="zone__title">Bundled starters</span>
              <span className="zone__count">{STARTERS.length}</span>
              <span className="zone__rule" />
            </div>
            <div className="recipe-grid" data-recipe-grid="starters">
              {STARTERS.map(preset => (
                <PresetCard
                  key={preset.id}
                  preset={preset}
                  onClick={() => openSlideover(preset)}
                  onClone={() => handleClone(preset)}
                />
              ))}
            </div>
          </div>

          {/* Zone 2 — Your presets */}
          <div className="zone">
            <div className="zone__head">
              <span className="zone__dot zone__dot--available" />
              <span className="zone__title">Your presets</span>
              <span className="zone__count">{userPresets.length}</span>
              <span className="zone__rule" />
            </div>
            {userPresets.length > 0 ? (
              <div className="recipe-grid" data-recipe-grid="yours">
                {userPresets.map(preset => (
                  <PresetCard
                    key={preset.id}
                    preset={preset}
                    onClick={() => openSlideover(preset)}
                    onApply={() => alert(`Apply "${preset.name}" — pick a model in the detail panel.`)}
                    onExport={() => handleExport(preset)}
                  />
                ))}
              </div>
            ) : (
              <div className="empty-state--inset" data-empty="yours">
                <p style={{ color: 'var(--text-tertiary)', textAlign: 'center' }}>
                  Make a preset your own
                </p>
                <div style={{ display: 'flex', gap: 'var(--space-2)', justifyContent: 'center', marginTop: 'var(--space-3)' }}>
                  <button className="btn btn--ghost" onClick={handleNewPreset}>
                    + New Preset
                  </button>
                  <button className="btn btn--ghost" onClick={() => setImportOpen(true)}>
                    + Import
                  </button>
                </div>
              </div>
            )}
          </div>

          {/* Zone 3 — Applied to models */}
          {modelNames.length > 0 && (
            <div className="zone">
              <div className="zone__head">
                <span className="zone__dot zone__dot--running" />
                <span className="zone__title">Applied to models</span>
                <span className="zone__count">{modelNames.length}</span>
                <span className="zone__rule" />
              </div>
              <div className="applied-list" data-applied-list>
                {modelNames.map(name => {
                  const pid = appliedPresets[name];
                  const preset = pid ? lookupPreset(pid) : null;
                  const initial = name.charAt(0);
                  return (
                    <div className="applied-row" key={name} data-applied-row={name}>
                      <div className="applied-row__model">
                        <span className="applied-row__model-icon">{initial}</span>
                        <span className="applied-row__model-name">{name}</span>
                      </div>
                      {preset ? (
                        <div className="applied-row__recipe">
                          <PhaseGlyph />
                          <span className="applied-row__recipe-name">{preset.name}</span>
                          <span style={{ fontSize: '11px', color: 'var(--text-tertiary)', fontFamily: 'var(--font-mono)' }}>
                            · {preset.starter ? 'starter' : 'yours'}
                          </span>
                        </div>
                      ) : (
                        <div className="applied-row__recipe applied-row__recipe--none">
                          no preset — defaults
                        </div>
                      )}
                      <div className="applied-row__actions">
                        {preset ? (
                          <>
                            <button className="btn btn--tiny btn--ghost" onClick={() => openSlideover(preset)}>Edit</button>
                            <button className="btn btn--tiny btn--ghost" onClick={() => handleDetach(name)}>Detach</button>
                          </>
                        ) : (
                          <button className="btn btn--tiny btn--ghost" onClick={() => alert(`Apply a preset to ${name}`)}>Apply…</button>
                        )}
                      </div>
                    </div>
                  );
                })}
              </div>
            </div>
          )}
        </div>
      </section>

      {/* ── Scrim + Slide-over ────────────────────────── */}
      <div
        className={`scrim${selectedPreset ? ' is-open' : ''}`}
        onClick={closeSlideover}
      />
      <aside
        className={`slideover slideover--recipe${selectedPreset ? ' is-open' : ''}`}
        aria-hidden={!selectedPreset}
      >
        {selectedPreset && (
          <SlideoverContent
            preset={selectedPreset}
            modelNames={modelNames}
            applyTarget={applyTarget}
            onApplyTargetChange={setApplyTarget}
            onApply={handleApply}
            onClone={handleClone}
            onExport={handleExport}
            onDelete={handleDelete}
            onClose={closeSlideover}
          />
        )}
      </aside>
    </>
  );
};

/* ── Preset card ───────────────────────────────────────────── */

const PresetCard: React.FC<{
  preset: Preset;
  onClick: () => void;
  onClone?: () => void;
  onApply?: () => void;
  onExport?: () => void;
}> = ({ preset, onClick, onClone, onApply, onExport }) => {
  const params = paramsPreview(preset);
  return (
    <article
      className="recipe-card"
      data-recipe-id={preset.id}
      tabIndex={0}
      role="button"
      aria-label={`Preset: ${preset.name}`}
      onClick={onClick}
      onKeyDown={e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); onClick(); } }}
    >
      {preset.starter && <span className="starter-badge">Starter</span>}
      <div className="recipe-card__head">
        <PhaseGlyph />
        <span className="recipe-card__name">{preset.name}</span>
      </div>
      <p className="recipe-card__desc">{preset.description}</p>
      <div className="cap-chip-list cap-chip-list--card" title="Applies to capabilities">
        {preset.applies_to.map(c => (
          <span className={`cap-chip cap-chip--${c}`} key={c}>
            <span className="cap-chip__dot" aria-hidden="true" />
            {c}
          </span>
        ))}
      </div>
      <div className="recipe-card__params" aria-hidden="true">
        <span className="recipe-card__param-key">params</span>
        <span className="recipe-card__param-val">{params}</span>
      </div>
      <div className="recipe-card__actions" onClick={e => e.stopPropagation()}>
        {preset.starter ? (
          <button
            className="recipe-card__action recipe-card__action--primary"
            onClick={onClone}
          >
            Clone
          </button>
        ) : (
          <>
            {onApply && (
              <button className="recipe-card__action" onClick={onApply}>Apply</button>
            )}
            {onExport && (
              <button className="recipe-card__action" onClick={onExport}>Export</button>
            )}
          </>
        )}
      </div>
    </article>
  );
};

/* ── Slideover content ─────────────────────────────────────── */

const SlideoverContent: React.FC<{
  preset: Preset;
  modelNames: string[];
  applyTarget: string;
  onApplyTargetChange: (v: string) => void;
  onApply: (presetId: string, modelName: string) => void;
  onClone: (preset: Preset) => void;
  onExport: (preset: Preset) => void;
  onDelete: (preset: Preset) => void;
  onClose: () => void;
}> = ({ preset, modelNames, applyTarget, onApplyTargetChange, onApply, onClone, onExport, onDelete, onClose }) => {
  const cap = primaryCap(preset);
  const isReadOnly = preset.starter;

  // Local state for slider values (mirrors)
  const [ctxSize, setCtxSize] = useState(preset.options.ctx_size ?? 4096);
  const [temperature, setTemperature] = useState(preset.sampling.temperature ?? 0.7);
  const [topP, setTopP] = useState(preset.sampling.top_p ?? 0.9);
  const [topK, setTopK] = useState(preset.sampling.top_k ?? 40);
  const [repeatPenalty, setRepeatPenalty] = useState(preset.sampling.repeat_penalty ?? 1.05);
  const [steps, setSteps] = useState(preset.options.steps ?? 30);
  const [cfgScale, setCfgScale] = useState(preset.options.cfg_scale ?? 7.0);

  // Reset local state when preset changes
  useEffect(() => {
    setCtxSize(preset.options.ctx_size ?? 4096);
    setTemperature(preset.sampling.temperature ?? 0.7);
    setTopP(preset.sampling.top_p ?? 0.9);
    setTopK(preset.sampling.top_k ?? 40);
    setRepeatPenalty(preset.sampling.repeat_penalty ?? 1.05);
    setSteps(preset.options.steps ?? 30);
    setCfgScale(preset.options.cfg_scale ?? 7.0);
  }, [preset]);

  return (
    <>
      {/* Head */}
      <div className="slideover__head">
        <div className="slideover__top">
          <div className="slideover__title-wrap">
            <PhaseGlyph size="lg" />
            <h2 className="slideover__title" data-recipe-name>{preset.name}</h2>
          </div>
          <button className="slideover__close" onClick={onClose} aria-label="Close">✕</button>
        </div>
        <div className="slideover__meta-row">
          {preset.starter && (
            <span className="recipe-badge recipe-badge--starter" data-recipe-starter-badge>
              Starter
            </span>
          )}
        </div>
        <p className="slideover__desc" data-recipe-desc>{preset.description}</p>
      </div>

      {/* Body */}
      <div className="slideover__body">
        {/* Applies to capabilities */}
        <div className="slideover__section">
          <h3>Applies to capabilities</h3>
          <div className="cap-chip-list" data-recipe-engines>
            {ALL_CAPABILITIES.map(c => (
              <CapChip
                key={c}
                cap={c}
                isOn={preset.applies_to.includes(c)}
                disabled={isReadOnly}
              />
            ))}
          </div>
        </div>

        {/* Chat fields */}
        {cap === 'chat' && (
          <div data-preset-fields="chat">
            <div className="slideover__section">
              <h3>Options · per model load</h3>
              <div className="field">
                <label className="field__label">Context size</label>
                <div className="field__row">
                  <input
                    type="range"
                    className="slider"
                    min={1024}
                    max={65536}
                    step={1024}
                    value={ctxSize}
                    disabled={isReadOnly}
                    onChange={e => setCtxSize(Number(e.target.value))}
                    data-recipe-ctx
                  />
                  <span className="field__value" data-recipe-ctx-val>{ctxSize}</span>
                </div>
              </div>
              <div className="field">
                <label className="field__label">Backend hint</label>
                <div className="field__row">
                  <select
                    className="select"
                    disabled={isReadOnly}
                    defaultValue={preset.options.backend || BACKEND_OPTIONS[1]}
                    data-recipe-backend
                  >
                    {BACKEND_OPTIONS.map(b => (
                      <option key={b} value={b}>{b}</option>
                    ))}
                  </select>
                </div>
              </div>
            </div>
            <div className="slideover__section">
              <h3>Sampling · per request</h3>
              <div className="field">
                <label className="field__label">Temperature</label>
                <div className="field__row">
                  <input
                    type="range"
                    className="slider"
                    min={0}
                    max={2}
                    step={0.05}
                    value={temperature}
                    disabled={isReadOnly}
                    onChange={e => setTemperature(Number(e.target.value))}
                    data-recipe-temp
                  />
                  <span className="field__value" data-recipe-temp-val>{temperature.toFixed(2)}</span>
                </div>
              </div>
              <div className="field">
                <label className="field__label">top_p</label>
                <div className="field__row">
                  <input
                    type="range"
                    className="slider"
                    min={0}
                    max={1}
                    step={0.01}
                    value={topP}
                    disabled={isReadOnly}
                    onChange={e => setTopP(Number(e.target.value))}
                    data-recipe-top-p
                  />
                  <span className="field__value" data-recipe-top-p-val>{topP.toFixed(2)}</span>
                </div>
              </div>
              <div className="field">
                <label className="field__label">top_k</label>
                <div className="field__row">
                  <input
                    type="number"
                    className="input input--narrow"
                    min={1}
                    max={200}
                    value={topK}
                    disabled={isReadOnly}
                    onChange={e => setTopK(Number(e.target.value))}
                    data-recipe-top-k
                  />
                </div>
              </div>
              <div className="field">
                <label className="field__label">Repeat penalty</label>
                <div className="field__row">
                  <input
                    type="range"
                    className="slider"
                    min={0.9}
                    max={1.5}
                    step={0.01}
                    value={repeatPenalty}
                    disabled={isReadOnly}
                    onChange={e => setRepeatPenalty(Number(e.target.value))}
                    data-recipe-rp
                  />
                  <span className="field__value" data-recipe-rp-val>{repeatPenalty.toFixed(2)}</span>
                </div>
              </div>
            </div>
          </div>
        )}

        {/* Image fields */}
        {cap === 'image' && (
          <div data-preset-fields="image">
            <div className="slideover__section">
              <h3>Options · per generation</h3>
              <div className="field">
                <label className="field__label">Steps</label>
                <div className="field__row">
                  <input
                    type="range"
                    className="slider"
                    min={5}
                    max={60}
                    step={1}
                    value={steps}
                    disabled={isReadOnly}
                    onChange={e => setSteps(Number(e.target.value))}
                    data-recipe-steps
                  />
                  <span className="field__value" data-recipe-steps-val>{steps}</span>
                </div>
              </div>
              <div className="field">
                <label className="field__label">CFG scale</label>
                <div className="field__row">
                  <input
                    type="range"
                    className="slider"
                    min={1}
                    max={20}
                    step={0.5}
                    value={cfgScale}
                    disabled={isReadOnly}
                    onChange={e => setCfgScale(Number(e.target.value))}
                    data-recipe-cfg
                  />
                  <span className="field__value" data-recipe-cfg-val>{cfgScale.toFixed(1)}</span>
                </div>
              </div>
            </div>
          </div>
        )}

        {/* Apply to a model */}
        <div className="slideover__section">
          <h3>Apply to a model</h3>
          <div className="field__row">
            <select
              className="select"
              value={applyTarget}
              onChange={e => onApplyTargetChange(e.target.value)}
              data-recipe-apply-target
            >
              <option value="">— pick a model —</option>
              {modelNames.map(name => (
                <option key={name} value={name}>{name}</option>
              ))}
            </select>
            <button
              className="btn btn--primary"
              disabled={!applyTarget}
              onClick={() => onApply(preset.id, applyTarget)}
            >
              Apply
            </button>
          </div>
        </div>
      </div>

      {/* Footer */}
      <div className="slideover__foot">
        <button className="btn btn--ghost" onClick={() => onExport(preset)}>
          Export
        </button>
        {preset.starter ? (
          <button
            className="btn btn--primary"
            onClick={() => onClone(preset)}
            data-recipe-clone
          >
            Clone
          </button>
        ) : (
          <button
            className="btn btn--ghost"
            style={{ color: 'var(--danger)' }}
            onClick={() => onDelete(preset)}
            data-recipe-delete
          >
            Delete
          </button>
        )}
      </div>
    </>
  );
};

export default PresetManager;
