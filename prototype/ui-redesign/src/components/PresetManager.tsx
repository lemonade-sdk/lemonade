import React, { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import api, { LoadedModel, ModelInfo } from '../api';
import {
  CAPABILITY_LABELS,
  CUSTOM_PRESET_PROMPTS,
  DEFAULT_PRESET,
  KNOWN_CAPABILITIES,
  Capability,
  NO_SYSTEM_PROMPT_ID,
  Preset,
  PresetRecipe,
  PresetSystemPrompt,
  RecipeOptions,
  SamplingParams,
  STARTERS,
  isCompatible,
  labelsFor,
  loadApplied,
  loadUserPresets,
  normalizePresetCapabilities,
  presetLabelsFor,
  presetParamPreviewLines,
  sanitizePreset,
  newCustomSystemPrompt,
  systemPromptNameForPreset,
  saveApplied,
  saveUserPresets,
} from '../presetStore';
import { CapabilityIcon, PresetIcon } from './Icon';
import { useFocusTrap } from '../hooks/useFocusTrap';
import { DEFAULT_TTS_VOICE, TTS_VOICES, normalizeTtsVoice } from '../features/audio/ttsSettings';

const ENGINE_LABELS: Record<PresetRecipe, string> = {
  auto: 'Auto — server decides',
  llamacpp: 'llama.cpp',
  'sd-cpp': 'stable-diffusion.cpp',
  whispercpp: 'whisper.cpp',
  moonshine: 'Moonshine',
  flm: 'FastFlowLM',
  'ryzenai-llm': 'RyzenAI',
  vllm: 'vLLM',
  kokoro: 'Kokoro',
};

// Known selectable values for #2339 datalist suggestions
const LLAMACPP_BACKENDS = ['auto', 'cpu', 'cuda', 'vulkan', 'kompute', 'metal', 'rpc', 'opencl', 'mmap'] as const;
const LLAMACPP_DEVICES  = ['Auto', 'CPU', 'CUDA0', 'CUDA1', 'Vulkan0', 'Vulkan1', 'Metal'] as const;

const RECIPE_KEYS: Record<PresetRecipe, (keyof RecipeOptions)[]> = {
  auto: ['ctx_size', 'steps', 'cfg_scale'],
  llamacpp: ['ctx_size', 'llamacpp_backend', 'llamacpp_device', 'llamacpp_args', 'merge_args'],
  'sd-cpp': ['steps', 'cfg_scale', 'width', 'height', 'sampling_method', 'flow_shift', 'sdcpp_args', 'merge_args'],
  whispercpp: ['whispercpp_backend', 'whispercpp_args', 'merge_args'],
  moonshine: ['moonshine_backend', 'moonshine_args', 'merge_args'],
  flm: ['ctx_size', 'flm_args', 'merge_args'],
  'ryzenai-llm': ['ctx_size'],
  vllm: ['ctx_size', 'vllm_backend', 'vllm_args', 'merge_args'],
  kokoro: ['voice', 'speed', 'merge_args'],
};

const CAPABILITIES: Capability[] = ['all', 'chat', 'image', 'tts'];

interface AutoOptRun {
  id: string;
  name: string;
  date: string;
  lemonadeVersion: string;
  summary: string;
  args: string;
  backends: { name: string; version: string; device: string }[];
}

const AUTO_OPT_RUNS: AutoOptRun[] = [
  {
    id: 'autoopt-1',
    name: 'AutoOpt #1',
    date: '2026-06-11',
    lemonadeVersion: '0.6.0-prototype',
    summary: 'Balanced local baseline for llama.cpp on mixed CPU/GPU machines.',
    args: '--threads auto --batch-size 512 --ubatch-size 256 --ctx-size 4096',
    backends: [
      { name: 'llama.cpp', version: 'b5412', device: 'CPU baseline' },
      { name: 'Vulkan', version: '1.3 safe path', device: 'GPU if available' },
    ],
  },
  {
    id: 'autoopt-2',
    name: 'AutoOpt #2',
    date: '2026-06-09',
    lemonadeVersion: '0.6.0-prototype',
    summary: 'Low-memory fallback that favors predictable CPU execution.',
    args: '--threads auto --batch-size 256 --ubatch-size 128 --ctx-size 4096 --n-gpu-layers 0',
    backends: [
      { name: 'llama.cpp', version: 'b5408', device: 'CPU' },
    ],
  },
  {
    id: 'autoopt-3',
    name: 'AutoOpt #3',
    date: '2026-06-05',
    lemonadeVersion: '0.5.9',
    summary: 'Throughput-oriented llama.cpp draft for larger VRAM systems.',
    args: '--threads auto --batch-size 1024 --ubatch-size 512 --ctx-size 8192 --n-gpu-layers 99',
    backends: [
      { name: 'llama.cpp', version: 'b5389', device: 'GPU preferred' },
      { name: 'CUDA/Vulkan', version: 'runtime default', device: 'Auto by Lemonade' },
    ],
  },
];

function modelName(model: ModelInfo): string {
  return model.id || model.name || model.display_name || 'unknown';
}

function capChipClass(cap: Capability): string {
  if (cap === 'all') return 'cap-chip--all';
  if (cap === 'transcription') return 'cap-chip--audio';
  if (cap === 'embedding') return 'cap-chip--embed';
  if (cap === 'reranking') return 'cap-chip--rerank';
  return `cap-chip--${cap}`;
}


const DEFAULT_CONTEXT_SIZE = 4096;
const DEFAULT_CONTEXT_LIMIT = 998400;

function parseContextSize(value: unknown, fallback = DEFAULT_CONTEXT_SIZE): number {
  const n = Number(value);
  if (!Number.isFinite(n) || n <= 0) return fallback;
  return Math.max(1, Math.round(n));
}

const CONTEXT_SIZE_SLIDER_SEGMENTS = [
  { from: 1024, to: 16 * 1024, step: 1024 },
  { from: 20 * 1024, to: 64 * 1024, step: 4 * 1024 },
  { from: 80 * 1024, to: 256 * 1024, step: 16 * 1024 },
  { from: 320 * 1024, to: DEFAULT_CONTEXT_LIMIT, step: 64 * 1024 },
];

function contextSizeOptions(max: number): number[] {
  const limit = Math.max(1, Math.round(max));
  const values = new Set<number>();

  for (const segment of CONTEXT_SIZE_SLIDER_SEGMENTS) {
    if (segment.from > limit) continue;
    const end = Math.min(segment.to, limit);
    for (let value = segment.from; value <= end; value += segment.step) {
      values.add(value);
    }
  }

  values.add(Math.min(DEFAULT_CONTEXT_SIZE, limit));
  values.add(limit);

  return [...values]
    .filter(value => value > 0 && value <= limit)
    .sort((a, b) => a - b);
}

function nearestContextSize(value: unknown, options: number[], fallback = DEFAULT_CONTEXT_SIZE): number {
  const parsed = parseContextSize(value, fallback);
  if (!options.length) return parsed;

  return options.reduce((best, candidate) => (
    Math.abs(candidate - parsed) < Math.abs(best - parsed) ? candidate : best
  ), options[0]);
}

function contextSizeIndex(value: unknown, options: number[]): number {
  const nearest = nearestContextSize(value, options);
  return Math.max(0, options.indexOf(nearest));
}

function clampContextSize(value: unknown, max: number, fallback = DEFAULT_CONTEXT_SIZE): number {
  return Math.min(parseContextSize(value, fallback), Math.max(1, Math.round(max)));
}

function firstPositiveInt(values: unknown[]): number | null {
  for (const value of values) {
    const n = Number(value);
    if (Number.isFinite(n) && n > 0) return Math.round(n);
  }
  return null;
}

function maxPositiveInt(values: unknown[]): number | null {
  let max = 0;
  for (const value of values) {
    const n = Number(value);
    if (Number.isFinite(n) && n > max) max = n;
  }
  return max > 0 ? Math.round(max) : null;
}

function contextLimitForModel(model?: ModelInfo | null): number | null {
  if (!model) return null;
  const direct = firstPositiveInt([
    (model as any).max_context_window,
    (model as any).max_ctx_size,
    (model as any).max_ctx,
    (model as any).context_window,
    (model as any).context_length,
    (model as any).max_sequence_length,
    (model as any).n_ctx,
    (model as any).ctx_size,
  ]);
  if (direct) return direct;

  const recipes = Array.isArray(model.recipes) ? model.recipes : [];
  const recipeCandidates: unknown[] = [];
  for (const recipe of recipes) {
    if (!recipe || typeof recipe !== 'object') continue;
    recipeCandidates.push(
      (recipe as any).max_context_window,
      (recipe as any).max_ctx_size,
      (recipe as any).max_ctx,
      (recipe as any).context_window,
      (recipe as any).context_length,
      (recipe as any).max_sequence_length,
      (recipe as any).n_ctx,
      (recipe as any).ctx_size,
    );
    const options = (recipe as any).recipe_options || (recipe as any).options;
    if (options && typeof options === 'object') {
      recipeCandidates.push(
        (options as any).max_context_window,
        (options as any).max_ctx_size,
        (options as any).max_ctx,
        (options as any).context_window,
        (options as any).context_length,
        (options as any).max_sequence_length,
        (options as any).n_ctx,
        (options as any).ctx_size,
      );
    }
  }
  return maxPositiveInt(recipeCandidates);
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
    <button type="button" className={`copy-inline${copied ? ' copy-inline--copied' : ''}`} onClick={handleClick} title={copied ? 'Copied' : title} aria-label={copied ? 'Copied' : title}>
      {copied ? '✓' : '⧉'}
    </button>
  );
};

function primaryCap(preset: Pick<Preset, 'applies_to'>): Capability {
  return preset.applies_to[0] || 'chat';
}

function paramsPreviewLines(preset: Preset): string[] {
  return presetParamPreviewLines(preset);
}

function cloneSystemPrompts(prompts: PresetSystemPrompt[] | undefined): PresetSystemPrompt[] {
  return (prompts || []).map(prompt => ({ ...prompt }));
}

function promptDisplayText(preset: Preset): string {
  return systemPromptNameForPreset(preset);
}

function toolsDisplayText(preset: Preset): string {
  return preset.tools_enabled === false ? 'OFF' : 'ON';
}

function hasManualArgs(preset: Pick<Preset, 'recipe_options'>): boolean {
  const ro = preset.recipe_options || {};
  return Boolean(
    String(ro.llamacpp_args || '').trim()
    || String(ro.sdcpp_args || '').trim()
    || String(ro.vllm_args || '').trim()
    || String(ro.flm_args || '').trim()
    || String(ro.whispercpp_args || '').trim()
    || String(ro.moonshine_args || '').trim()
  );
}

const CapabilityChip: React.FC<{ cap: Capability; small?: boolean; on?: boolean; off?: boolean }> = ({ cap, small, on, off }) => (
  <span className={`cap-chip ${capChipClass(cap)}${small ? ' cap-chip--sm' : ''}${on ? ' is-on' : ''}${off ? ' is-off' : ''}`}>
    <span className="cap-chip__icon" aria-hidden="true"><CapabilityIcon capability={cap} size={12} /></span>
    {CAPABILITY_LABELS[cap] || cap}
  </span>
);

const PhaseGlyph: React.FC<{ size?: 'sm' | 'lg' | 'xl' }> = ({ size }) => {
  const cls = size === 'lg' ? 'phase-glyph phase-glyph--lg' : size === 'xl' ? 'phase-glyph phase-glyph--xl' : 'phase-glyph';
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

interface PresetManagerProps {
  loadedModels: LoadedModel[];
}

const PresetManager: React.FC<PresetManagerProps> = ({ loadedModels }) => {
  const [userPresets, setUserPresets] = useState<Preset[]>(loadUserPresets);
  const [appliedPresets, setAppliedPresets] = useState<Record<string, string>>(loadApplied);
  const [selectedPreset, setSelectedPreset] = useState<Preset | null>(null);
  const [knownModels, setKnownModels] = useState<ModelInfo[]>(api.allModels);
  const [importOpen, setImportOpen] = useState(false);
  const [importError, setImportError] = useState<string | null>(null);
  const [applyTarget, setApplyTarget] = useState('');
  const [applySuccess, setApplySuccess] = useState<string | null>(null);
  const [autoRailCollapsed, setAutoRailCollapsed] = useState(false);
  const [selectedAutoRunId, setSelectedAutoRunId] = useState(AUTO_OPT_RUNS[0]?.id || '');
  const slideoverRef = useRef<HTMLElement>(null);
  const triggerRef = useRef<HTMLElement | null>(null);

  useEffect(() => { saveUserPresets(userPresets); }, [userPresets]);
  useEffect(() => { saveApplied(appliedPresets); }, [appliedPresets]);

  useEffect(() => {
    let alive = true;
    api.models(true).then(data => { if (alive) setKnownModels(data.data || []); }).catch(() => {
      if (alive) setKnownModels(api.allModels);
    });
    return () => { alive = false; };
  }, []);

  const allPresets = useMemo(() => [DEFAULT_PRESET, ...STARTERS, ...userPresets], [userPresets]);
  const lookupPreset = useCallback((id: string) => allPresets.find(p => p.id === id) || null, [allPresets]);

  const allModelOptions = useMemo(() => {
    const map = new Map<string, ModelInfo>();
    for (const m of knownModels) map.set(modelName(m), m);
    for (const m of loadedModels) map.set(m.model_name, { id: m.model_name, name: m.model_name, labels: [m.type], recipe: m.recipe } as ModelInfo);
    for (const name of Object.keys(appliedPresets)) if (!map.has(name)) map.set(name, { id: name } as ModelInfo);
    return [...map.values()].sort((a, b) => modelName(a).localeCompare(modelName(b)));
  }, [knownModels, loadedModels, appliedPresets]);

  const appliedModelNames = useMemo(() => Object.keys(appliedPresets), [appliedPresets]);

  const closeSlideover = useCallback(() => {
    setSelectedPreset(null);
    requestAnimationFrame(() => triggerRef.current?.focus());
  }, []);

  const openSlideover = useCallback((preset: Preset) => {
    triggerRef.current = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    setSelectedPreset(preset);
    setApplyTarget('');
    setApplySuccess(null);
  }, []);

  useFocusTrap(slideoverRef, !!selectedPreset);

  useEffect(() => {
    if (!selectedPreset) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') closeSlideover();
    };
    document.addEventListener('keydown', onKey);
    return () => document.removeEventListener('keydown', onKey);
  }, [selectedPreset, closeSlideover]);

  const handleNewPreset = useCallback(() => {
    const newPreset: Preset = {
      id: `u-${Date.now()}`,
      name: 'New Preset',
      description: '',
      applies_to: ['chat'],
      recipe_options: { ctx_size: 4096 },
      sampling: { temperature: 0.70, top_p: 0.90, top_k: 40, repeat_penalty: 1.05 },
      engine_hint: 'auto',
      starter: false,
      auto_opt_enabled: true,
      auto_opt_run_id: AUTO_OPT_RUNS[0]?.id || null,
      system_prompt_id: 'general',
      system_prompts: cloneSystemPrompts(CUSTOM_PRESET_PROMPTS),
      tools_enabled: true,
    };
    setUserPresets(prev => [newPreset, ...prev]);
    openSlideover(newPreset);
  }, [openSlideover]);

  const importPresets = useCallback((raw: string) => {
    const data = JSON.parse(raw);
    const items = Array.isArray(data) ? data : [data];
    if (items.some(p => p && typeof p === 'object' && 'recipe' in p && !('applies_to' in p))) {
      throw new Error('This file uses the legacy schema. Use the v1.4 export instead.');
    }
    const presets = items.map(p => sanitizePreset({
      ...p,
      id: `u-${Date.now()}-${Math.random().toString(36).slice(2, 6)}`,
      starter: false,
    })).filter((p): p is Preset => !!p);
    if (presets.length !== items.length) throw new Error('Preset import must include applies_to: Capability[].');
    setUserPresets(prev => [...presets, ...prev]);
  }, []);

  const handleImportFile = useCallback(() => {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.json';
    input.onchange = async () => {
      const file = input.files?.[0];
      if (!file) return;
      try {
        importPresets(await file.text());
        setImportError(null);
      } catch (err) {
        setImportError(err instanceof Error ? err.message : 'Could not import preset JSON.');
      }
      setImportOpen(false);
    };
    input.click();
  }, [importPresets]);

  const handleImportClipboard = useCallback(async () => {
    try {
      importPresets(await navigator.clipboard.readText());
      setImportError(null);
    } catch (err) {
      setImportError(err instanceof Error ? err.message : 'Could not import preset JSON.');
    }
    setImportOpen(false);
  }, [importPresets]);

  const handleClone = useCallback((preset: Preset) => {
    const clonedId = `u-${Date.now()}`;
    const cloned: Preset = {
      ...preset,
      id: clonedId,
      name: `${preset.name} (copy)`,
      starter: false,
      applies_to: normalizePresetCapabilities(clonedId, preset.applies_to),
      recipe_options: { ...preset.recipe_options },
      sampling: { ...preset.sampling },
      system_prompts: cloneSystemPrompts(preset.system_prompts),
      system_prompt_id: preset.system_prompt_id || NO_SYSTEM_PROMPT_ID,
      tools_enabled: preset.tools_enabled !== false,
    };
    setUserPresets(prev => [cloned, ...prev]);
    openSlideover(cloned);
  }, [openSlideover]);

  const handleExport = useCallback((preset: Preset) => {
    const { starter, ...exportable } = preset;
    navigator.clipboard.writeText(JSON.stringify(exportable, null, 2)).catch(() => {});
  }, []);

  const handleSave = useCallback((updated: Preset) => {
    setUserPresets(prev => prev.map(p => p.id === updated.id ? updated : p));
    setSelectedPreset(updated);
  }, []);

  const handleDelete = useCallback((preset: Preset) => {
    setUserPresets(prev => prev.filter(p => p.id !== preset.id));
    setAppliedPresets(prev => Object.fromEntries(Object.entries(prev).filter(([, pid]) => pid !== preset.id)));
    closeSlideover();
  }, [closeSlideover]);

  const handleApply = useCallback((presetId: string, model: ModelInfo) => {
    const preset = allPresets.find(p => p.id === presetId);
    if (!preset || !isCompatible(preset, model)) return;
    const name = modelName(model);
    setAppliedPresets(prev => ({ ...prev, [name]: presetId }));
    setApplySuccess(`Staged "${preset.name}" for ${name}. Will apply on next load.`);
    setTimeout(() => setApplySuccess(null), 3000);
  }, [allPresets]);

  const handleDetach = useCallback((name: string) => {
    setAppliedPresets(prev => {
      const next = { ...prev };
      delete next[name];
      return next;
    });
  }, []);

  const selectedAutoRun = AUTO_OPT_RUNS.find(run => run.id === selectedAutoRunId) || AUTO_OPT_RUNS[0];

  return (
    <>
      <section className={`recipes recipes--with-rail${autoRailCollapsed ? ' context-rail-collapsed' : ''}`} data-view="presets">
        <aside className={`context-rail context-rail--autoopt${autoRailCollapsed ? ' is-collapsed' : ''}`} aria-label="AutoOpt runs">
          <div className="context-rail__head">
            <button type="button" className="context-rail__toggle" onClick={() => setAutoRailCollapsed(v => !v)} aria-label="Toggle AutoOpt rail">☰</button>
            <div className="context-rail__title-wrap">
              <span className="context-rail__eyebrow">Auto Optimizer</span>
              <strong className="context-rail__title">Runs</strong>
            </div>
          </div>
          <div className="context-rail__body">
            <p className="context-rail__hint">Select a safe local optimization result and attach it to editable presets. Manual args override AutoOpt.</p>
            <div className="auto-run-list">
              {AUTO_OPT_RUNS.map(run => (
                <article key={run.id} className={`auto-run-card${selectedAutoRunId === run.id ? ' is-active' : ''}`}>
                  <button type="button" className="auto-run-card__main" onClick={() => setSelectedAutoRunId(run.id)} aria-pressed={selectedAutoRunId === run.id}>
                    <span className="auto-run-card__icon">⚙️</span>
                    <span className="auto-run-card__text">
                      <strong>{run.name}</strong>
                      <span>{run.date} · Lemonade {run.lemonadeVersion}</span>
                    </span>
                  </button>
                  <details className="auto-run-card__details" onClick={e => e.stopPropagation()}>
                    <summary>Backend details</summary>
                    <p>{run.summary}</p>
                    <code>{run.args}</code>
                    <ul>
                      {run.backends.map(backend => <li key={`${run.id}-${backend.name}-${backend.version}`}>{backend.name} {backend.version} · {backend.device}</li>)}
                    </ul>
                  </details>
                </article>
              ))}
            </div>
          </div>
        </aside>
        <div className="recipes__main">
        <div className="recipes__head">
          <div className="recipes__title">
            <h1>Presets</h1>
            <span className="recipes__title-sub" data-recipes-count>Default · {STARTERS.length} starters · {userPresets.length} yours</span>
          </div>
          <div className="recipes__actions">
            <button className="btn btn--primary" onClick={handleNewPreset}>+ New Preset</button>
            <div className="dropdown">
              <button className="btn btn--ghost dropdown__trigger" onClick={() => setImportOpen(!importOpen)}>
                + Import <span className="dropdown__caret">▾</span>
              </button>
              <div className="dropdown__menu" hidden={!importOpen}>
                <button className="dropdown__item" onClick={handleImportFile}>From file…</button>
                <button className="dropdown__item" onClick={handleImportClipboard}>From clipboard</button>
              </div>
            </div>
          </div>
        </div>

        <div className="recipes__body">
          <p className="recipes__lede">
            Presets are saved ways to use a model. They apply to capabilities like <strong>Chat</strong> or <strong>Image</strong>,
            can stage recipe options for the next explicit model load, pass chat sampling defaults per request, and optionally follow an AutoOpt run.
          </p>
          {importError && <p className="preset-error" role="alert">⚠ {importError}</p>}


          {selectedAutoRun && (
            <div className="autoopt-summary">
              <div>
                <span className="autoopt-summary__kicker">Selected AutoOpt result</span>
                <strong>{selectedAutoRun.name}</strong>
                <span>{selectedAutoRun.summary}</span>
              </div>
              <code>{selectedAutoRun.args}</code>
            </div>
          )}

          <div className="zone">
            <div className="zone__head">
              <span className="zone__dot zone__dot--ready" />
              <span className="zone__title">Bundled starters</span>
              <span className="zone__count">{STARTERS.length + 1}</span>
              <span className="zone__rule" />
            </div>
            <div className="recipe-grid recipe-grid--starters-combined">
              <PresetCard preset={DEFAULT_PRESET} onClick={() => openSlideover(DEFAULT_PRESET)} onClone={() => handleClone(DEFAULT_PRESET)} />
              <div className="recipe-grid__contents" data-recipe-grid="starters">
                {STARTERS.map(preset => (
                  <PresetCard key={preset.id} preset={preset} onClick={() => openSlideover(preset)} onClone={() => handleClone(preset)} />
                ))}
              </div>
            </div>
          </div>

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
                  <PresetCard key={preset.id} preset={preset} onClick={() => openSlideover(preset)} onApply={() => openSlideover(preset)} onExport={() => handleExport(preset)} />
                ))}
              </div>
            ) : (
              <div className="empty-state--inset" data-empty="yours">
                <p className="preset-empty-title">Your presets are empty.</p>
                <p className="preset-empty-copy">Pick a starter, clone it, or save from a model to create one.</p>
                <div className="preset-empty-actions">
                  <button className="btn btn--ghost" onClick={() => openSlideover(STARTERS[0])}>Pick a starter</button>
                  <button className="btn btn--ghost" onClick={handleNewPreset}>+ New Preset</button>
                </div>
              </div>
            )}
          </div>

          {appliedModelNames.length > 0 && (
            <div className="zone">
              <div className="zone__head">
                <span className="zone__dot zone__dot--running" />
                <span className="zone__title">Applied to models</span>
                <span className="zone__count">{appliedModelNames.length}</span>
                <span className="zone__rule" />
              </div>
              <div className="applied-list" data-applied-list>
                {appliedModelNames.map(name => {
                  const preset = lookupPreset(appliedPresets[name]);
                  return (
                    <div className="applied-row" key={name} data-applied-row={name}>
                      <div className="applied-row__model">
                        <span className="applied-row__model-icon">{name.charAt(0)}</span>
                        <span className="applied-row__model-name-wrap"><span className="applied-row__model-name">{name}</span><CopyInlineButton text={name} /></span>
                      </div>
                      <div className="applied-row__recipe">
                        <PhaseGlyph />
                        <span className="applied-row__recipe-name">{preset?.name || 'Missing preset'}</span>
                        <span className="preset-status-chip">Will apply on next load</span>
                      </div>
                      <div className="applied-row__actions">
                        {preset && <button className="btn btn--tiny btn--ghost" onClick={() => openSlideover(preset)}>Edit</button>}
                        <button className="btn btn--tiny btn--ghost" onClick={() => handleDetach(name)}>Detach</button>
                      </div>
                    </div>
                  );
                })}
              </div>
            </div>
          )}
        </div>
        </div>
      </section>

      <div className={`scrim${selectedPreset ? ' is-open' : ''}`} onClick={closeSlideover} />
      <aside
        ref={slideoverRef}
        className={`slideover slideover--recipe${selectedPreset ? ' is-open' : ''}`}
        aria-hidden={!selectedPreset}
        role="dialog"
        aria-modal="true"
        aria-label="Preset details"
      >
        {selectedPreset && (
          <SlideoverContent
            preset={selectedPreset}
            models={allModelOptions}
            applyTarget={applyTarget}
            onApplyTargetChange={setApplyTarget}
            onApply={handleApply}
            applySuccess={applySuccess}
            onSave={handleSave}
            onClone={handleClone}
            onExport={handleExport}
            onDelete={handleDelete}
            onClose={closeSlideover}
            autoRuns={AUTO_OPT_RUNS}
          />
        )}
      </aside>
    </>
  );
};

const PresetCard: React.FC<{
  preset: Preset;
  onClick: () => void;
  onClone?: () => void;
  onApply?: () => void;
  onExport?: () => void;
}> = ({ preset, onClick, onClone, onApply, onExport }) => {
  const descId = `preset-card-desc-${preset.id}`;
  const capLabels = presetLabelsFor(preset).map(c => CAPABILITY_LABELS[c] || c).join(', ');
  const paramLines = paramsPreviewLines(preset);
  const descParts: string[] = [];
  if (preset.starter) descParts.push('Starter');
  if (hasManualArgs(preset)) descParts.push('Manual args active');
  descParts.push(`Applies to: ${capLabels}`);
  if (paramLines.length) descParts.push(`Parameters: ${paramLines.join('; ')}`);
  descParts.push(`Prompt: ${promptDisplayText(preset)}`);
  descParts.push(`Tools: ${toolsDisplayText(preset)}`);
  return (
  <article
    className={`recipe-card${hasManualArgs(preset) ? ' recipe-card--manual' : ''}`}
    data-recipe-id={preset.id}
  >
    {/* Overlay button covers the card for pointer/keyboard activation without nesting interactive roles */}
    <button
      className="recipe-card__overlay-btn"
      onClick={onClick}
      aria-label={`Open Preset: ${preset.name}`}
      aria-describedby={descId}
    />
    {/* sr-only description for #2345: exposes parameter/prompt/tools metadata to AT */}
    <span id={descId} className="sr-only">{descParts.join('. ')}.</span>
    {preset.starter && <span className="starter-badge">Starter</span>}
    <div className="recipe-card__head"><PresetIcon preset={preset} /><span className="recipe-card__name">{preset.name}</span></div>
    <p className="recipe-card__desc">{preset.description}</p>
    <div className="cap-chip-list cap-chip-list--card" title="Applies to">
      {presetLabelsFor(preset).map(cap => <CapabilityChip key={cap} cap={cap} small />)}
    </div>
    <div className="recipe-card__params" aria-hidden="true">
      <span className="recipe-card__param-key">params</span>
      <span className="recipe-card__param-val preset-param-lines">{paramsPreviewLines(preset).map(line => <span key={line}>{line}</span>)}</span>
    </div>
    <div className="recipe-card__behavior" aria-hidden="true">
      <span>prompt</span><strong>{promptDisplayText(preset)}</strong>
      <span>tools</span><strong>{toolsDisplayText(preset)}</strong>
    </div>
    <div className="recipe-card__actions" onClick={e => e.stopPropagation()}>
      {preset.starter ? (
        <button className="recipe-card__action recipe-card__action--primary" onClick={onClone}>Clone</button>
      ) : (
        <>
          {onApply && <button className="recipe-card__action" onClick={onApply}>Apply</button>}
          {onExport && <button className="recipe-card__action" onClick={onExport}>Export</button>}
        </>
      )}
    </div>
  </article>
  );
};

const SlideoverContent: React.FC<{
  preset: Preset;
  models: ModelInfo[];
  applyTarget: string;
  onApplyTargetChange: (v: string) => void;
  onApply: (presetId: string, model: ModelInfo) => void;
  applySuccess: string | null;
  onSave: (updated: Preset) => void;
  onClone: (preset: Preset) => void;
  onExport: (preset: Preset) => void;
  onDelete: (preset: Preset) => void;
  onClose: () => void;
  autoRuns: AutoOptRun[];
}> = ({ preset, models, applyTarget, onApplyTargetChange, onApply, applySuccess, onSave, onClone, onExport, onDelete, onClose, autoRuns }) => {
  const isReadOnly = preset.starter;
  const ro = preset.recipe_options || {};
  const sp = preset.sampling || {};

  const [name, setName] = useState(preset.name);
  const [description, setDescription] = useState(preset.description);
  const [appliesTo, setAppliesTo] = useState<Capability[]>(preset.applies_to);
  const [engineHint, setEngineHint] = useState<PresetRecipe>(preset.engine_hint || 'auto');
  const [autoOptRunId, setAutoOptRunId] = useState(preset.auto_opt_run_id || autoRuns[0]?.id || '');
  const [ctxSize, setCtxSize] = useState(parseContextSize(ro.ctx_size));
  const [steps, setSteps] = useState(ro.steps ?? 20);
  const [cfgScale, setCfgScale] = useState(ro.cfg_scale ?? 7.0);
  const [imgWidth, setImgWidth] = useState(ro.width ?? 512);
  const [imgHeight, setImgHeight] = useState(ro.height ?? 512);
  const [ttsVoice, setTtsVoice] = useState(normalizeTtsVoice(ro.voice ?? DEFAULT_TTS_VOICE));
  const [llamacppBackend, setLlamacppBackend] = useState(ro.llamacpp_backend ?? '');
  const [llamacppDevice, setLlamacppDevice] = useState(ro.llamacpp_device ?? '');
  const [llamacppArgs, setLlamacppArgs] = useState(ro.llamacpp_args ?? '');
  const [sdcppArgs, setSdcppArgs] = useState(ro.sdcpp_args ?? '');
  const [temperature, setTemperature] = useState(sp.temperature ?? 0.7);
  const [topP, setTopP] = useState(sp.top_p ?? 0.9);
  const [topK, setTopK] = useState(sp.top_k ?? 40);
  const [repeatPenalty, setRepeatPenalty] = useState(sp.repeat_penalty ?? 1.05);
  const [systemPromptId, setSystemPromptId] = useState(preset.system_prompt_id || NO_SYSTEM_PROMPT_ID);
  const [systemPrompts, setSystemPrompts] = useState<PresetSystemPrompt[]>(cloneSystemPrompts(preset.system_prompts));
  const [toolsEnabled, setToolsEnabled] = useState(preset.tools_enabled !== false);
  const [saved, setSaved] = useState(false);

  useEffect(() => {
    const nextRo = preset.recipe_options || {};
    const nextSp = preset.sampling || {};
    setName(preset.name);
    setDescription(preset.description);
    setAppliesTo(normalizePresetCapabilities(preset.id, preset.applies_to));
    setEngineHint(preset.engine_hint || 'auto');
    setAutoOptRunId(preset.auto_opt_run_id || autoRuns[0]?.id || '');
    setCtxSize(parseContextSize(nextRo.ctx_size));
    setSteps(nextRo.steps ?? 20);
    setCfgScale(nextRo.cfg_scale ?? 7.0);
    setImgWidth(nextRo.width ?? 512);
    setImgHeight(nextRo.height ?? 512);
    setTtsVoice(normalizeTtsVoice(nextRo.voice ?? DEFAULT_TTS_VOICE));
    setLlamacppBackend(nextRo.llamacpp_backend ?? '');
    setLlamacppDevice(nextRo.llamacpp_device ?? '');
    setLlamacppArgs(nextRo.llamacpp_args ?? '');
    setSdcppArgs(nextRo.sdcpp_args ?? '');
    setTemperature(nextSp.temperature ?? 0.7);
    setTopP(nextSp.top_p ?? 0.9);
    setTopK(nextSp.top_k ?? 40);
    setRepeatPenalty(nextSp.repeat_penalty ?? 1.05);
    setSystemPromptId(preset.system_prompt_id || NO_SYSTEM_PROMPT_ID);
    setSystemPrompts(cloneSystemPrompts(preset.system_prompts));
    setToolsEnabled(preset.tools_enabled !== false);
    setSaved(false);
  }, [preset, autoRuns]);

  const hasAllCapability = appliesTo.includes('all');
  const manualArgsActive = Boolean(
    ((hasAllCapability || appliesTo.some(cap => cap === 'chat' || cap === 'omni' || cap === 'code' || cap === 'vision')) && llamacppArgs.trim())
    || ((hasAllCapability || appliesTo.includes('image')) && sdcppArgs.trim())
  );
  const selectedAutoRun = autoRuns.find(run => run.id === autoOptRunId) || autoRuns[0];

  const currentPreset = useMemo<Preset>(() => {
    if (isReadOnly) {
      return {
        ...preset,
        applies_to: normalizePresetCapabilities(preset.id, preset.applies_to),
      };
    }
    const normalizedAppliesTo = normalizePresetCapabilities(preset.id, appliesTo);
    const supportsTools = normalizedAppliesTo.includes('all') || normalizedAppliesTo.some(cap => cap === 'chat' || cap === 'omni' || cap === 'code' || cap === 'vision');
    return {
      ...preset,
      name,
      description,
      applies_to: normalizedAppliesTo,
      engine_hint: engineHint,
      recipe_options: buildRecipeOptions(appliesTo, ctxSize, steps, cfgScale, imgWidth, imgHeight, ttsVoice, llamacppBackend, llamacppDevice, llamacppArgs, sdcppArgs),
      sampling: buildSampling(appliesTo, temperature, topP, topK, repeatPenalty),
      starter: false,
      auto_opt_enabled: !manualArgsActive,
      auto_opt_run_id: manualArgsActive ? null : (autoOptRunId || autoRuns[0]?.id || null),
      system_prompt_id: systemPromptId,
      system_prompts: cloneSystemPrompts(systemPrompts),
      tools_enabled: supportsTools && toolsEnabled,
    };
  }, [isReadOnly, preset, name, description, appliesTo, engineHint, ctxSize, steps, cfgScale, imgWidth, imgHeight, ttsVoice, llamacppBackend, llamacppDevice, llamacppArgs, sdcppArgs, temperature, topP, topK, repeatPenalty, systemPromptId, systemPrompts, toolsEnabled, manualArgsActive, autoOptRunId, autoRuns]);

const selectedModel = models.find(m => modelName(m) === applyTarget);
const selectedModelContextLimit = contextLimitForModel(selectedModel);
const ctxSliderMax = selectedModelContextLimit || DEFAULT_CONTEXT_LIMIT;
const ctxOptions = useMemo(() => contextSizeOptions(ctxSliderMax), [ctxSliderMax]);
const ctxSliderIndex = useMemo(() => contextSizeIndex(ctxSize, ctxOptions), [ctxSize, ctxOptions]);
const canApply = !!selectedModel && isCompatible(currentPreset, selectedModel);

  useEffect(() => {
    const nextCtxSize = nearestContextSize(
      clampContextSize(ctxSize, ctxSliderMax, DEFAULT_CONTEXT_SIZE),
      ctxOptions,
    );

    if (ctxSize !== nextCtxSize) setCtxSize(nextCtxSize);
  }, [ctxSize, ctxSliderMax, ctxOptions]);
  const validKeys = RECIPE_KEYS[engineHint] || [];
  const hasAll = appliesTo.includes('all');
  const hasChat = hasAll || appliesTo.some(cap => cap === 'chat' || cap === 'omni' || cap === 'code' || cap === 'vision');
  const hasImage = hasAll || appliesTo.includes('image');
  const hasTts = hasAll || appliesTo.includes('tts');
  const isDefaultEmptyPreset = preset.id === DEFAULT_PRESET.id;
  const selectedSystemPrompt = systemPromptId === NO_SYSTEM_PROMPT_ID ? null : (systemPrompts.find(prompt => prompt.id === systemPromptId) || null);
  const selectedPromptIsCustom = selectedSystemPrompt?.built_in === false;

  const updateSelectedSystemPrompt = (patch: Partial<PresetSystemPrompt>) => {
    if (!selectedSystemPrompt || isReadOnly || !selectedPromptIsCustom) return;
    setSystemPrompts(prev => prev.map(prompt => prompt.id === selectedSystemPrompt.id ? { ...prompt, ...patch } : prompt));
  };

  const addCustomSystemPrompt = () => {
    if (isReadOnly) return;
    const customPrompt = newCustomSystemPrompt(systemPrompts);
    setSystemPrompts(prev => [...prev, customPrompt]);
    setSystemPromptId(customPrompt.id);
  };

  const deleteSelectedCustomPrompt = () => {
    if (isReadOnly || !selectedSystemPrompt || !selectedPromptIsCustom) return;
    const remaining = systemPrompts.filter(prompt => prompt.id !== selectedSystemPrompt.id);
    setSystemPrompts(remaining);
    setSystemPromptId(remaining[0]?.id || NO_SYSTEM_PROMPT_ID);
  };

  const toggleCap = (cap: Capability) => {
    if (isReadOnly) return;
    setAppliesTo([cap]);
  };

  const handleSave = () => {
    onSave(currentPreset);
    setSaved(true);
    setTimeout(() => setSaved(false), 1500);
  };

  return (
    <>
      <div className="slideover__head">
        <div className="slideover__top">
          <div className="slideover__title-wrap">
            <div className="slideover__title-with-icon">
              <PresetIcon preset={preset} className="preset-icon preset-icon--lg" />
              {isReadOnly ? <h2 className="slideover__title" data-recipe-name>{preset.name}</h2> : (
                <input className="slideover__title-input" value={name} onChange={e => setName(e.target.value)} placeholder="Preset name" data-recipe-name aria-label="Preset name" />
              )}
            </div>
          </div>
          <button className="slideover__close" onClick={onClose} aria-label="Close">✕</button>
        </div>
        <div className="slideover__meta-row">
          {appliesTo.map(cap => <CapabilityChip key={cap} cap={cap} />)}
          {preset.starter && <span className="recipe-badge recipe-badge--starter" data-recipe-starter-badge>Starter</span>}
        </div>
        {isReadOnly ? <p className="slideover__desc" data-recipe-desc>{preset.description}</p> : (
          <textarea className="slideover__desc-input" value={description} onChange={e => setDescription(e.target.value)} placeholder="Description (optional)" rows={2} data-recipe-desc aria-label="Description" />
        )}
      </div>

      <div className="slideover__body">
        <div className="slideover__section">
          <h3>Applies to capabilities</h3>
          <div className="cap-chip-list" data-preset-capabilities role="radiogroup" aria-label="Applies to capabilities">
            {CAPABILITIES.map(cap => (
              <button key={cap} type="button" className="preset-cap-button" disabled={isReadOnly} onClick={() => toggleCap(cap)} role="radio" aria-checked={appliesTo.includes(cap)}>
                <CapabilityChip cap={cap} on={appliesTo.includes(cap)} off={!appliesTo.includes(cap)} />
              </button>
            ))}
          </div>
        </div>

        <div className="slideover__section preset-system-prompt">
          <h3>System prompt</h3>
          <p className="preset-help">Only the selected prompt is sent with each compatible request. The full prompt list stays in the preset and does not inflate conversation context.</p>
          <div className="field">
            <label className="field__label">Prompt type</label>
            <div className="field__row">
              <select className="select" value={systemPromptId} disabled={isReadOnly} onChange={e => setSystemPromptId(e.target.value)}>
                <option value={NO_SYSTEM_PROMPT_ID}>No system prompt</option>
                {systemPrompts.map(prompt => <option key={prompt.id} value={prompt.id}>{prompt.name}</option>)}
              </select>
            </div>
          </div>
          <details className="preset-prompt-details">
            <summary>{selectedSystemPrompt ? `Prompt text: ${selectedSystemPrompt.name}` : 'Prompt text'}</summary>
            {!selectedSystemPrompt ? (
              <p className="preset-help">No behavior system prompt will be sent for this preset.</p>
            ) : selectedPromptIsCustom && !isReadOnly ? (
              <div className="preset-prompt-editor">
                <div className="field">
                  <label className="field__label">Displayed name</label>
                  <input className="input" value={selectedSystemPrompt.name} onChange={e => updateSelectedSystemPrompt({ name: e.target.value })} />
                </div>
                <div className="field">
                  <label className="field__label">System prompt text</label>
                  <textarea className="input preset-prompt-textarea" rows={7} value={selectedSystemPrompt.prompt} onChange={e => updateSelectedSystemPrompt({ prompt: e.target.value })} />
                </div>
              </div>
            ) : (
              <pre className="preset-prompt-preview">{selectedSystemPrompt.prompt}</pre>
            )}
          </details>
          {!isReadOnly && (
            <div className="preset-prompt-actions">
              <button type="button" className="btn btn--ghost btn--tiny" onClick={addCustomSystemPrompt}>+ Custom prompt</button>
              {selectedPromptIsCustom && <button type="button" className="btn btn--ghost btn--tiny" style={{ color: 'var(--danger)' }} onClick={deleteSelectedCustomPrompt}>Delete custom prompt</button>}
            </div>
          )}
        </div>

        <div className="slideover__section preset-tools-default">
          <h3>Tools start value</h3>
          <label className="preset-toggle">
            <input type="checkbox" checked={hasChat && toolsEnabled} disabled={isReadOnly || !hasChat} onChange={e => setToolsEnabled(e.target.checked)} />
            <span>{hasChat ? `Start chats with Lemonade tools ${toolsEnabled ? 'enabled' : 'disabled'} for this preset.` : 'Image-only presets do not start Lemonade chat tools.'}</span>
          </label>
          {preset.id === 's-quick-chat' && <p className="preset-help">Quick Chat starts with tools off to minimize request context.</p>}
          {!hasChat && <p className="preset-help">Direct image generation/edit endpoints use the prompt and image options directly; chat tools are not useful there.</p>}
        </div>

        <div className="slideover__section">
          <h3>Behavior</h3>
          {isDefaultEmptyPreset && (
            <div className="preset-empty-overrides">
              <strong>No preset overrides</strong>
              <span>Lemonade uses the selected model's current defaults for sampling, context and image generation.</span>
              <span className="preset-param-lines">{presetParamPreviewLines(preset).map(line => <span key={line}>{line}</span>)}</span>
            </div>
          )}
          {!isDefaultEmptyPreset && hasChat && (
            <div data-preset-fields="chat">
              <div className="field"><label className="field__label" htmlFor="preset-field-temperature">Creativity</label><div className="field__row"><input id="preset-field-temperature" type="range" className="slider" min={0} max={2} step={0.05} value={temperature} disabled={isReadOnly} onChange={e => setTemperature(Number(e.target.value))} data-recipe-temp /><span className="field__value">{temperature.toFixed(2)}</span></div></div>
              <div className="field"><label className="field__label" htmlFor="preset-field-top-p">Precision (top_p)</label><div className="field__row"><input id="preset-field-top-p" type="range" className="slider" min={0} max={1} step={0.01} value={topP} disabled={isReadOnly} onChange={e => setTopP(Number(e.target.value))} data-recipe-top-p /><span className="field__value">{topP.toFixed(2)}</span></div></div>
              <div className="field">
                <label className="field__label" htmlFor="preset-field-ctx-size">Context size</label>
                <div className="field__row">
                  <input
                    id="preset-field-ctx-size"
                    type="range"
                    className="slider"
                    min={0}
                    max={Math.max(0, ctxOptions.length - 1)}
                    step={1}
                    value={ctxSliderIndex}
                    disabled={isReadOnly}
                    onChange={e => setCtxSize(ctxOptions[Number(e.target.value)] ?? ctxSize)}
                    data-recipe-ctx
                  />
                  <span className="field__value">{ctxSize.toLocaleString()}</span>
                </div>
              </div>
              <div className="field"><label className="field__label" htmlFor="preset-field-top-k">top_k</label><div className="field__row"><input id="preset-field-top-k" type="number" className="input input--narrow" min={1} max={200} value={topK} disabled={isReadOnly} onChange={e => setTopK(Number(e.target.value))} data-recipe-top-k /></div></div>
              <div className="field"><label className="field__label" htmlFor="preset-field-repeat-penalty">Repeat penalty</label><div className="field__row"><input id="preset-field-repeat-penalty" type="range" className="slider" min={0.9} max={1.5} step={0.01} value={repeatPenalty} disabled={isReadOnly} onChange={e => setRepeatPenalty(Number(e.target.value))} data-recipe-rp /><span className="field__value">{repeatPenalty.toFixed(2)}</span></div></div>
            </div>
          )}
          {!isDefaultEmptyPreset && hasImage && (
            <div data-preset-fields="image">
              <div className="field"><label className="field__label" htmlFor="preset-field-steps">Steps</label><div className="field__row"><input id="preset-field-steps" type="range" className="slider" min={1} max={100} step={1} value={steps} disabled={isReadOnly} onChange={e => setSteps(Number(e.target.value))} data-recipe-steps /><span className="field__value">{steps}</span></div></div>
              <div className="field"><label className="field__label" htmlFor="preset-field-cfg-scale">CFG scale</label><div className="field__row"><input id="preset-field-cfg-scale" type="range" className="slider" min={1} max={30} step={0.5} value={cfgScale} disabled={isReadOnly} onChange={e => setCfgScale(Number(e.target.value))} data-recipe-cfg /><span className="field__value">{cfgScale.toFixed(1)}</span></div></div>
            </div>
          )}
          {!isDefaultEmptyPreset && hasTts && (
            <div data-preset-fields="audio" className="preset-audio-settings">
              <h4>Audio</h4>
              <div className="field">
                <label className="field__label" htmlFor="preset-tts-voice">Voice</label>
                <div className="field__row">
                  <select
                    id="preset-tts-voice"
                    className="select"
                    value={ttsVoice}
                    disabled={isReadOnly}
                    onChange={e => setTtsVoice(normalizeTtsVoice(e.target.value))}
                    data-recipe-tts-voice
                  >
                    {TTS_VOICES.map(voice => <option key={voice.id} value={voice.id}>{voice.label}</option>)}
                  </select>
                </div>
              </div>
            </div>
          )}
        </div>

        {!isDefaultEmptyPreset && <div className="slideover__section preset-autoopt">
          <h3>AutoOpt</h3>
          <p className="slideover__hint">Default is AutoOpt. Entering manual raw args below disables AutoOpt for this preset until those args are cleared.</p>
          <div className="field"><label className="field__label" htmlFor="preset-field-autoopt-result">AutoOpt result</label><div className="field__row"><select id="preset-field-autoopt-result" className="select" value={autoOptRunId} disabled={isReadOnly || manualArgsActive} onChange={e => setAutoOptRunId(e.target.value)}>{autoRuns.map(run => <option key={run.id} value={run.id}>{run.name} · {run.date} · Lemonade {run.lemonadeVersion}</option>)}</select></div></div>
          {selectedAutoRun && <p className="preset-autoopt__args"><span>{manualArgsActive ? 'Manual override active' : 'AutoOpt active'}</span><code>{manualArgsActive ? 'Clear manual args to re-enable AutoOpt.' : selectedAutoRun.args}</code></p>}
        </div>}

        {!isDefaultEmptyPreset && <details className="slideover__section preset-advanced">
          <summary>Advanced engine options</summary>
          <p className="slideover__hint">Optional backend hints and raw recipe_options keys. Closed by default.</p>
          <div className="field"><label className="field__label" htmlFor="preset-field-engine-hint">Engine hint</label><div className="field__row"><select id="preset-field-engine-hint" className="select" value={engineHint} disabled={isReadOnly} onChange={e => setEngineHint(e.target.value as PresetRecipe)}>{(Object.keys(ENGINE_LABELS) as PresetRecipe[]).map(r => <option key={r} value={r}>{ENGINE_LABELS[r]}</option>)}</select></div></div>
          <p className="preset-valid-keys">Valid recipe_options keys: {validKeys.length ? validKeys.join(', ') : 'none'}</p>
          {hasChat && (
            <>
              <div className="field"><label className="field__label" htmlFor="preset-field-llamacpp-backend">llamacpp_backend</label><div className="field__row"><input id="preset-field-llamacpp-backend" className="input" list="preset-llamacpp-backends" value={llamacppBackend} disabled={isReadOnly} placeholder="auto" onChange={e => setLlamacppBackend(e.target.value)} /><datalist id="preset-llamacpp-backends">{LLAMACPP_BACKENDS.map(b => <option key={b} value={b} />)}</datalist></div></div>
              <div className="field"><label className="field__label" htmlFor="preset-field-llamacpp-device">llamacpp_device</label><div className="field__row"><input id="preset-field-llamacpp-device" className="input" list="preset-llamacpp-devices" value={llamacppDevice} disabled={isReadOnly} placeholder="e.g. Vulkan0" onChange={e => setLlamacppDevice(e.target.value)} /><datalist id="preset-llamacpp-devices">{LLAMACPP_DEVICES.map(d => <option key={d} value={d} />)}</datalist></div></div>
              <div className="field"><label className="field__label" htmlFor="preset-field-llamacpp-args">llamacpp_args</label><div className="field__row"><input id="preset-field-llamacpp-args" className="input" value={llamacppArgs} disabled={isReadOnly} placeholder="e.g. --n-gpu-layers 99" onChange={e => setLlamacppArgs(e.target.value)} /></div></div>
            </>
          )}
          {hasImage && (
            <>
              <div className="field"><label className="field__label">Image width × height</label><div className="field__row"><input type="number" className="input input--narrow" aria-label="Image width" value={imgWidth} disabled={isReadOnly} onChange={e => setImgWidth(Number(e.target.value))} /><span style={{ color: 'var(--text-tertiary)' }} aria-hidden="true">×</span><input type="number" className="input input--narrow" aria-label="Image height" value={imgHeight} disabled={isReadOnly} onChange={e => setImgHeight(Number(e.target.value))} /></div></div>
              <div className="field"><label className="field__label" htmlFor="preset-field-sdcpp-args">sdcpp_args</label><div className="field__row"><input id="preset-field-sdcpp-args" className="input" value={sdcppArgs} disabled={isReadOnly} placeholder="e.g. --diffusion-fa" onChange={e => setSdcppArgs(e.target.value)} /></div></div>
            </>
          )}
        </details>}

        <div className="slideover__section">
          <h3>Apply to a model</h3>
          <p className="preset-help">Stores a local binding only. Recipe options apply the next time you explicitly load that model.</p>
          <div className="field__row">
            <select className="select" value={applyTarget} onChange={e => onApplyTargetChange(e.target.value)} data-recipe-apply-target>
              <option value="">— pick a model —</option>
              {models.map(m => {
                const nameForModel = modelName(m);
                const caps = labelsFor(m);
                const compatible = isCompatible(currentPreset, m);
                const reason = compatible ? `${caps.map(c => CAPABILITY_LABELS[c]).join(', ')}` : `Incompatible: needs ${currentPreset.applies_to.map(c => CAPABILITY_LABELS[c]).join(' or ')}; this model exposes ${caps.map(c => CAPABILITY_LABELS[c]).join(', ')}`;
                return <option key={nameForModel} value={nameForModel} disabled={!compatible} title={reason}>{nameForModel} · {caps.map(c => CAPABILITY_LABELS[c]).join(', ')}</option>;
              })}
            </select>
            <button className="btn btn--primary" disabled={!canApply} onClick={() => selectedModel && onApply(preset.id, selectedModel)}>Apply</button>
          </div>
          {selectedModel && !canApply && <p className="preset-error" role="tooltip">Incompatible preset for this model.</p>}
          {applySuccess && <p className="preset-success">✓ {applySuccess}</p>}
        </div>
      </div>

      <div className="slideover__foot">
        <button className="btn btn--ghost" onClick={() => onExport(currentPreset)}>Export</button>
        {preset.starter ? <button className="btn btn--primary" onClick={() => onClone(preset)} data-recipe-clone>Clone</button> : (
          <>
            <button className="btn btn--ghost" style={{ color: 'var(--danger)' }} onClick={() => onDelete(preset)} data-recipe-delete>Delete</button>
            <button className={`btn btn--primary${saved ? ' btn--saved' : ''}`} onClick={handleSave}>{saved ? '✓ Saved' : 'Save'}</button>
          </>
        )}
      </div>
    </>
  );
};

function buildRecipeOptions(
  appliesTo: Capability[],
  ctxSize: number,
  steps: number,
  cfgScale: number,
  imgWidth: number,
  imgHeight: number,
  ttsVoice: string,
  llamacppBackend: string,
  llamacppDevice: string,
  llamacppArgs: string,
  sdcppArgs: string,
): RecipeOptions {
  const opts: RecipeOptions = {};
  const hasAll = appliesTo.includes('all');
  const hasChat = hasAll || appliesTo.some(cap => cap === 'chat' || cap === 'omni' || cap === 'code' || cap === 'vision');
  const hasImage = hasAll || appliesTo.includes('image');
  const hasTts = hasAll || appliesTo.includes('tts');
  if (hasChat) {
    opts.ctx_size = ctxSize;
    if (llamacppBackend) opts.llamacpp_backend = llamacppBackend;
    if (llamacppDevice) opts.llamacpp_device = llamacppDevice;
    if (llamacppArgs) opts.llamacpp_args = llamacppArgs;
  }
  if (hasImage) {
    opts.steps = steps;
    opts.cfg_scale = cfgScale;
    opts.width = imgWidth;
    opts.height = imgHeight;
    if (sdcppArgs) opts.sdcpp_args = sdcppArgs;
  }
  if (hasTts) {
    opts.voice = normalizeTtsVoice(ttsVoice);
  }
  return opts;
}

function buildSampling(appliesTo: Capability[], temperature: number, topP: number, topK: number, repeatPenalty: number): SamplingParams {
  if (!appliesTo.includes('all') && !appliesTo.some(cap => cap === 'chat' || cap === 'omni' || cap === 'code' || cap === 'vision')) return {};
  return { temperature, top_p: topP, top_k: topK, repeat_penalty: repeatPenalty };
}

export default PresetManager;
