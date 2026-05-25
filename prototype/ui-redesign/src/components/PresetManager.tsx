import React, { useState, useEffect, useCallback, useMemo } from 'react';
import { LoadedModel } from '../api';

/* ── Data model ────────────────────────────────────────────── */

/** Recipe options — applied at model load time via /api/v1/load */
interface RecipeOptions {
  // LLM recipes (llamacpp, flm, ryzenai-llm, vllm)
  ctx_size?: number;
  llamacpp_backend?: string;
  llamacpp_device?: string;
  llamacpp_args?: string;
  // Image recipe (sd-cpp)
  steps?: number;
  cfg_scale?: number;
  width?: number;
  height?: number;
  sampling_method?: string;
  flow_shift?: number;
  sdcpp_args?: string;
  // Whisper recipe
  whispercpp_backend?: string;
  whispercpp_args?: string;
  // vLLM recipe
  vllm_backend?: string;
  vllm_args?: string;
  // FLM recipe
  flm_args?: string;
  // Global
  merge_args?: boolean;
}

/** Sampling params — applied per-request in chat/completions body */
interface SamplingParams {
  temperature?: number;
  top_p?: number;
  top_k?: number;
  repeat_penalty?: number;
}

type PresetRecipe = 'llamacpp' | 'sd-cpp' | 'whispercpp' | 'flm' | 'ryzenai-llm' | 'vllm' | 'kokoro' | 'any';

interface Preset {
  id: string;
  name: string;
  description: string;
  recipe: PresetRecipe;
  recipe_options: RecipeOptions;
  sampling: SamplingParams;
  starter: boolean;
}

/* ── Constants ─────────────────────────────────────────────── */

const STARTERS: Preset[] = [
  // LLM presets (apply to llamacpp-family recipes)
  { id: 's-balanced',     name: 'Balanced',     description: 'Sensible defaults. Good first pick for everyday chat.',                      recipe: 'llamacpp', recipe_options: { ctx_size: 4096 },  sampling: { temperature: 0.70, top_p: 0.90, top_k: 40, repeat_penalty: 1.05 }, starter: true },
  { id: 's-quality',      name: 'Quality',      description: 'Larger context, slightly looser sampling for richer long-form answers.',     recipe: 'llamacpp', recipe_options: { ctx_size: 8192 },  sampling: { temperature: 0.70, top_p: 0.95, top_k: 40, repeat_penalty: 1.10 }, starter: true },
  { id: 's-fast',         name: 'Fast',         description: 'Small context, tight sampling. Snappy responses for quick interactions.',    recipe: 'llamacpp', recipe_options: { ctx_size: 2048 },  sampling: { temperature: 0.60, top_p: 0.80, top_k: 40, repeat_penalty: 1.05 }, starter: true },
  { id: 's-creative',     name: 'Creative',     description: 'Higher temperature for brainstorming, dialog, and divergent thinking.',      recipe: 'llamacpp', recipe_options: { ctx_size: 8192 },  sampling: { temperature: 0.95, top_p: 0.95, top_k: 60, repeat_penalty: 1.00 }, starter: true },
  { id: 's-long-context', name: 'Long Context', description: 'For documents, codebases, and long conversation threads.',                  recipe: 'llamacpp', recipe_options: { ctx_size: 32768 }, sampling: { temperature: 0.70, top_p: 0.90, top_k: 40, repeat_penalty: 1.05 }, starter: true },
  { id: 's-code',         name: 'Code',         description: 'Low temperature, tight sampling for code generation and refactoring.',       recipe: 'llamacpp', recipe_options: { ctx_size: 8192 },  sampling: { temperature: 0.20, top_p: 0.95, top_k: 40, repeat_penalty: 1.05 }, starter: true },
  // Image presets (apply to sd-cpp recipe)
  { id: 's-sharp', name: 'Sharp', description: 'More steps and tighter guidance for crisp, deliberate image generation.', recipe: 'sd-cpp', recipe_options: { steps: 30, cfg_scale: 8.0, width: 512, height: 512 }, sampling: {}, starter: true },
  { id: 's-quick', name: 'Quick', description: 'Fewer steps, looser guidance — fast drafts and iteration.',               recipe: 'sd-cpp', recipe_options: { steps: 15, cfg_scale: 7.0, width: 512, height: 512 }, sampling: {}, starter: true },
];

const DEFAULT_USER_PRESETS: Preset[] = [
  { id: 'u-long-code',  name: 'Long Code',  description: 'Custom: big context + code-style sampling for monorepo work.',               recipe: 'llamacpp', recipe_options: { ctx_size: 16384 }, sampling: { temperature: 0.25, top_p: 0.95, top_k: 40, repeat_penalty: 1.04 }, starter: false },
  { id: 'u-brainstorm', name: 'Brainstorm', description: 'High-temp, wide top_p for ideation sessions and divergent thinking.',        recipe: 'llamacpp', recipe_options: { ctx_size: 4096 },  sampling: { temperature: 1.05, top_p: 0.98, top_k: 80, repeat_penalty: 1.00 }, starter: false },
];

const LS_USER_PRESETS = 'lemonade_user_presets';
const LS_APPLIED_PRESETS = 'lemonade_applied_presets';

/** Real recipe names from the backend — user-facing labels */
const RECIPE_LABELS: Record<PresetRecipe, string> = {
  'llamacpp':     'llama.cpp',
  'sd-cpp':       'stable-diffusion.cpp',
  'whispercpp':   'whisper.cpp',
  'flm':          'FastFlowLM',
  'ryzenai-llm':  'RyzenAI',
  'vllm':         'vLLM',
  'kokoro':       'Kokoro',
  'any':          'Any',
};

/** Recipe → which recipe_options keys are valid */
const RECIPE_KEYS: Record<string, (keyof RecipeOptions)[]> = {
  'llamacpp':     ['ctx_size', 'llamacpp_backend', 'llamacpp_device', 'llamacpp_args', 'merge_args'],
  'sd-cpp':       ['steps', 'cfg_scale', 'width', 'height', 'sampling_method', 'flow_shift', 'sdcpp_args', 'merge_args'],
  'whispercpp':   ['whispercpp_backend', 'whispercpp_args', 'merge_args'],
  'flm':          ['ctx_size', 'flm_args', 'merge_args'],
  'ryzenai-llm':  ['ctx_size'],
  'vllm':         ['ctx_size', 'vllm_backend', 'vllm_args', 'merge_args'],
  'kokoro':       [],
  'any':          [],
};

const LLAMACPP_BACKENDS = ['vulkan', 'rocm', 'metal', 'cpu'] as const;
const SDCPP_SAMPLING_METHODS = ['euler', 'euler_a', 'heun', 'dpm2', 'dpm++2s_a', 'dpm++2m', 'dpm++2mv2', 'lcm'] as const;

/* ── Helpers ────────────────────────────────────────────────── */

function sanitizePreset(p: Partial<Preset>): Preset {
  return {
    id: p.id || `u-${Date.now()}`,
    name: p.name || 'Untitled',
    description: p.description || '',
    recipe: p.recipe || 'any',
    recipe_options: p.recipe_options || {},
    sampling: p.sampling || {},
    starter: p.starter ?? false,
  };
}

function loadUserPresets(): Preset[] {
  try {
    const raw = localStorage.getItem(LS_USER_PRESETS);
    if (raw) return (JSON.parse(raw) as Partial<Preset>[]).map(sanitizePreset);
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

function primaryCap(preset: Preset): 'llm' | 'image' | 'audio' | 'tts' | 'other' {
  const r = preset.recipe;
  if (r === 'llamacpp' || r === 'flm' || r === 'ryzenai-llm' || r === 'vllm') return 'llm';
  if (r === 'sd-cpp') return 'image';
  if (r === 'whispercpp') return 'audio';
  if (r === 'kokoro') return 'tts';
  return 'other';
}

/** Chip color per recipe */
function recipeChipClass(recipe: PresetRecipe): string {
  switch (recipe) {
    case 'llamacpp':    return 'cap-chip--chat';
    case 'sd-cpp':      return 'cap-chip--image';
    case 'whispercpp':  return 'cap-chip--audio';
    case 'flm':         return 'cap-chip--embed';
    case 'ryzenai-llm': return 'cap-chip--vision';
    case 'vllm':        return 'cap-chip--rerank';
    case 'kokoro':      return 'cap-chip--tts';
    default:            return 'cap-chip--chat';
  }
}

function paramsPreview(preset: Preset): string {
  const cap = primaryCap(preset);
  const ro = preset.recipe_options || {};
  const sp = preset.sampling || {};
  if (cap === 'image') {
    const s = ro.steps ?? '—';
    const c = ro.cfg_scale != null ? ro.cfg_scale.toFixed(1) : '—';
    const w = ro.width ?? 512;
    const h = ro.height ?? 512;
    return `${s} steps · cfg ${c} · ${w}×${h}`;
  }
  const t = sp.temperature != null ? sp.temperature.toFixed(2) : '—';
  const ctx = ro.ctx_size ?? '—';
  return `temp ${t} · ctx ${ctx}`;
}

/** Whether a preset has any sampling params */
function hasSampling(preset: Preset): boolean {
  const sp = preset.sampling || {};
  return sp.temperature != null || sp.top_p != null
    || sp.top_k != null || sp.repeat_penalty != null;
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

/* ── Recipe chip ────────────────────────────────────────────── */

const RecipeChip: React.FC<{
  recipe: PresetRecipe;
  small?: boolean;
}> = ({ recipe, small }) => (
  <span className={`cap-chip ${recipeChipClass(recipe)}${small ? ' cap-chip--sm' : ''}`}>
    <span className="cap-chip__dot" aria-hidden="true" />
    {RECIPE_LABELS[recipe]}
  </span>
);

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
            Presets bundle <strong>recipe options</strong> (applied when a model
            loads — context size, backend, image dimensions) and <strong>sampling
            params</strong> (applied per request — temperature, top_p), scoped to
            a recipe type like <em>llama.cpp</em> or <em>stable-diffusion.cpp</em>.
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
      <div className="cap-chip-list cap-chip-list--card" title="Recipe target">
        <RecipeChip recipe={preset.recipe} small />
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
  const validKeys = RECIPE_KEYS[preset.recipe] || [];

  // Local state — recipe options
  const [ctxSize, setCtxSize] = useState(preset.recipe_options.ctx_size ?? 4096);
  const [llamacppBackend, setLlamacppBackend] = useState(preset.recipe_options.llamacpp_backend ?? '');
  const [llamacppDevice, setLlamacppDevice] = useState(preset.recipe_options.llamacpp_device ?? '');
  const [llamacppArgs, setLlamacppArgs] = useState(preset.recipe_options.llamacpp_args ?? '');
  const [steps, setSteps] = useState(preset.recipe_options.steps ?? 20);
  const [cfgScale, setCfgScale] = useState(preset.recipe_options.cfg_scale ?? 7.0);
  const [imgWidth, setImgWidth] = useState(preset.recipe_options.width ?? 512);
  const [imgHeight, setImgHeight] = useState(preset.recipe_options.height ?? 512);
  const [samplingMethod, setSamplingMethod] = useState(preset.recipe_options.sampling_method ?? '');
  const [sdcppArgs, setSdcppArgs] = useState(preset.recipe_options.sdcpp_args ?? '');

  // Local state — sampling params
  const [temperature, setTemperature] = useState(preset.sampling.temperature ?? 0.7);
  const [topP, setTopP] = useState(preset.sampling.top_p ?? 0.9);
  const [topK, setTopK] = useState(preset.sampling.top_k ?? 40);
  const [repeatPenalty, setRepeatPenalty] = useState(preset.sampling.repeat_penalty ?? 1.05);

  useEffect(() => {
    setCtxSize(preset.recipe_options.ctx_size ?? 4096);
    setLlamacppBackend(preset.recipe_options.llamacpp_backend ?? '');
    setLlamacppDevice(preset.recipe_options.llamacpp_device ?? '');
    setLlamacppArgs(preset.recipe_options.llamacpp_args ?? '');
    setSteps(preset.recipe_options.steps ?? 20);
    setCfgScale(preset.recipe_options.cfg_scale ?? 7.0);
    setImgWidth(preset.recipe_options.width ?? 512);
    setImgHeight(preset.recipe_options.height ?? 512);
    setSamplingMethod(preset.recipe_options.sampling_method ?? '');
    setSdcppArgs(preset.recipe_options.sdcpp_args ?? '');
    setTemperature(preset.sampling.temperature ?? 0.7);
    setTopP(preset.sampling.top_p ?? 0.9);
    setTopK(preset.sampling.top_k ?? 40);
    setRepeatPenalty(preset.sampling.repeat_penalty ?? 1.05);
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
          <RecipeChip recipe={preset.recipe} />
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
        {/* Recipe info */}
        <div className="slideover__section">
          <h3>Recipe target</h3>
          <p style={{ fontSize: 'var(--text-sm)', color: 'var(--text-tertiary)', margin: '0 0 var(--space-2)' }}>
            This preset is scoped to <strong>{RECIPE_LABELS[preset.recipe]}</strong> models.
            Valid recipe_options: {validKeys.length > 0 ? validKeys.join(', ') : 'none'}.
          </p>
        </div>

        {/* ── Recipe options (load-time) ── */}

        {/* llamacpp / flm / ryzenai-llm / vllm — ctx_size + backend */}
        {cap === 'llm' && (
          <div data-preset-fields="llm">
            <div className="slideover__section">
              <h3>Recipe options <span className="slideover__hint">applied at model load</span></h3>
              {validKeys.includes('ctx_size') && (
                <div className="field">
                  <label className="field__label">ctx_size</label>
                  <div className="field__row">
                    <input type="range" className="slider" min={1024} max={131072} step={1024}
                      value={ctxSize} disabled={isReadOnly}
                      onChange={e => setCtxSize(Number(e.target.value))} data-recipe-ctx />
                    <span className="field__value" data-recipe-ctx-val>{ctxSize.toLocaleString()}</span>
                  </div>
                </div>
              )}
              {validKeys.includes('llamacpp_backend') && (
                <div className="field">
                  <label className="field__label">llamacpp_backend</label>
                  <div className="field__row">
                    <select className="select" disabled={isReadOnly}
                      value={llamacppBackend}
                      onChange={e => setLlamacppBackend(e.target.value)}
                      data-recipe-backend>
                      <option value="">(auto — server decides)</option>
                      {LLAMACPP_BACKENDS.map(b => <option key={b} value={b}>{b}</option>)}
                    </select>
                  </div>
                </div>
              )}
              {validKeys.includes('llamacpp_device') && (
                <div className="field">
                  <label className="field__label">llamacpp_device</label>
                  <div className="field__row">
                    <input type="text" className="input" placeholder="e.g. Vulkan0"
                      value={llamacppDevice} disabled={isReadOnly}
                      onChange={e => setLlamacppDevice(e.target.value)} />
                  </div>
                </div>
              )}
              {validKeys.includes('llamacpp_args') && (
                <div className="field">
                  <label className="field__label">llamacpp_args</label>
                  <div className="field__row">
                    <input type="text" className="input" placeholder="e.g. --n-gpu-layers 99"
                      value={llamacppArgs} disabled={isReadOnly}
                      onChange={e => setLlamacppArgs(e.target.value)} />
                  </div>
                </div>
              )}
            </div>
          </div>
        )}

        {/* sd-cpp — image generation options */}
        {cap === 'image' && (
          <div data-preset-fields="image">
            <div className="slideover__section">
              <h3>Recipe options <span className="slideover__hint">applied at generation time</span></h3>
              <div className="field">
                <label className="field__label">steps</label>
                <div className="field__row">
                  <input type="range" className="slider" min={1} max={100} step={1}
                    value={steps} disabled={isReadOnly}
                    onChange={e => setSteps(Number(e.target.value))} data-recipe-steps />
                  <span className="field__value" data-recipe-steps-val>{steps}</span>
                </div>
              </div>
              <div className="field">
                <label className="field__label">cfg_scale</label>
                <div className="field__row">
                  <input type="range" className="slider" min={1} max={30} step={0.5}
                    value={cfgScale} disabled={isReadOnly}
                    onChange={e => setCfgScale(Number(e.target.value))} data-recipe-cfg />
                  <span className="field__value" data-recipe-cfg-val>{cfgScale.toFixed(1)}</span>
                </div>
              </div>
              <div className="field">
                <label className="field__label">width × height</label>
                <div className="field__row" style={{ gap: 'var(--space-2)' }}>
                  <input type="number" className="input input--narrow" min={256} max={2048} step={64}
                    value={imgWidth} disabled={isReadOnly}
                    onChange={e => setImgWidth(Number(e.target.value))} />
                  <span style={{ color: 'var(--text-tertiary)' }}>×</span>
                  <input type="number" className="input input--narrow" min={256} max={2048} step={64}
                    value={imgHeight} disabled={isReadOnly}
                    onChange={e => setImgHeight(Number(e.target.value))} />
                </div>
              </div>
              <div className="field">
                <label className="field__label">sampling_method</label>
                <div className="field__row">
                  <select className="select" disabled={isReadOnly}
                    value={samplingMethod}
                    onChange={e => setSamplingMethod(e.target.value)}>
                    <option value="">(default)</option>
                    {SDCPP_SAMPLING_METHODS.map(m => <option key={m} value={m}>{m}</option>)}
                  </select>
                </div>
              </div>
              {validKeys.includes('sdcpp_args') && (
                <div className="field">
                  <label className="field__label">sdcpp_args</label>
                  <div className="field__row">
                    <input type="text" className="input" placeholder="e.g. --diffusion-fa"
                      value={sdcppArgs} disabled={isReadOnly}
                      onChange={e => setSdcppArgs(e.target.value)} />
                  </div>
                </div>
              )}
            </div>
          </div>
        )}

        {/* ── Sampling params (per-request) — only for LLM recipes ── */}
        {hasSampling(preset) && (
          <div data-preset-fields="sampling">
            <div className="slideover__section">
              <h3>Sampling params <span className="slideover__hint">applied per request</span></h3>
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
