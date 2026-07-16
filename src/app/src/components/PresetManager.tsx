import React, { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import api, { LoadedModel, ModelInfo } from '../api';
import {
  CAPABILITY_LABELS,
  CUSTOM_PRESET_PROMPTS,
  DEFAULT_PRESET,
  Capability,
  NO_SYSTEM_PROMPT_ID,
  Preset,
  PresetSystemPrompt,
  TemperatureHint,
  ContextHint,
  ThinkingMode,
  TEMPERATURE_HINT_LABELS,
  CONTEXT_HINT_LABELS,
  THINKING_MODE_LABELS,
  presetSupportsChatIntent,
  STARTERS,
  isCompatible,
  labelsFor,
  loadApplied,
  loadUserPresets,
  normalizePresetCapabilities,
  PRESET_STORE_EVENT,
  presetLabelsFor,
  presetParamPreviewLines,
  sanitizePreset,
  newCustomSystemPrompt,
  systemPromptNameForPreset,
  saveApplied,
  saveUserPresets,
} from '../presetStore';
import { CapabilityIcon, Icon, PresetIcon, type IconName } from './Icon';
import { useFocusTrap } from '../hooks/useFocusTrap';
import AutoOptRail, { openAutoOptRun } from '../features/autoOpt/AutoOptRail';
import { autoOptStore, type AutoOptState } from '../features/autoOpt/autoOptStore';

const CAPABILITIES: Capability[] = ['all', 'chat', 'omni', 'vision', 'code', 'image', 'transcription', 'audio-generation', 'tts', 'model3d', 'embedding', 'reranking'];

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
  const [highlightPresetId, setHighlightPresetId] = useState<string | null>(null);
  const slideoverRef = useRef<HTMLElement>(null);
  const triggerRef = useRef<HTMLElement | null>(null);
  const startersRef = useRef<HTMLDivElement>(null);
  const userPresetsRef = useRef(userPresets);

  useEffect(() => { userPresetsRef.current = userPresets; }, [userPresets]);
  useEffect(() => { saveUserPresets(userPresets); }, [userPresets]);
  useEffect(() => { saveApplied(appliedPresets); }, [appliedPresets]);

  useEffect(() => {
    const onStoreChange = () => {
      const next = loadUserPresets();
      if (JSON.stringify(next) !== JSON.stringify(userPresetsRef.current)) {
        const previousIds = new Set(userPresetsRef.current.map(preset => preset.id));
        const added = next.find(preset => !previousIds.has(preset.id));
        setUserPresets(next);
        if (added) setHighlightPresetId(added.id);
      }
      setAppliedPresets(prev => {
        const nextApplied = loadApplied();
        return JSON.stringify(nextApplied) === JSON.stringify(prev) ? prev : nextApplied;
      });
    };
    window.addEventListener(PRESET_STORE_EVENT, onStoreChange);
    return () => window.removeEventListener(PRESET_STORE_EVENT, onStoreChange);
  }, []);

  useEffect(() => {
    if (!highlightPresetId) return;
    requestAnimationFrame(() => {
      document.querySelector(`[data-recipe-id="${highlightPresetId}"]`)
        ?.scrollIntoView({ behavior: 'smooth', block: 'center' });
    });
    const timer = window.setTimeout(() => setHighlightPresetId(null), 10000);
    return () => window.clearTimeout(timer);
  }, [highlightPresetId]);

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

  const linkedModelsByPreset = useMemo(() => {
    const map = new Map<string, string[]>();
    for (const [model, presetId] of Object.entries(appliedPresets)) {
      map.set(presetId, [...(map.get(presetId) || []), model]);
    }
    return map;
  }, [appliedPresets]);

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
      temperature_hint: 'balanced',
      context_hint: 'medium',
      thinking_mode: 'normal',
      recipe_options: {},
      sampling: {},
      engine_hint: 'auto',
      starter: false,
      auto_opt_enabled: true,
      auto_opt_run_id: null,
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

  const handleClone = useCallback((preset: Preset, openEditor = false) => {
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
    setHighlightPresetId(cloned.id);
    if (openEditor) openSlideover(cloned);
    else closeSlideover();
  }, [closeSlideover, openSlideover]);

  const handleCustomize = useCallback((preset: Preset) => {
    handleClone(preset, true);
  }, [handleClone]);

  const scrollToStarters = useCallback(() => {
    const target = startersRef.current;
    if (!target) return;
    target.scrollIntoView({ behavior: 'smooth', block: 'start' });
    window.setTimeout(() => target.focus({ preventScroll: true }), 350);
  }, []);

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


  return (
    <>
      <section className={`recipes recipes--with-rail${autoRailCollapsed ? ' context-rail-collapsed' : ''}`} data-view="presets">
        <AutoOptRail
          loadedModels={loadedModels}
          collapsed={autoRailCollapsed}
          onToggleCollapsed={() => setAutoRailCollapsed(v => !v)}
        />
        <div className="recipes__main">
        <div className="recipes__head">
          <div className="recipes__title">
            <h1>Presets</h1>
            <span className="recipes__title-sub" data-recipes-count>{STARTERS.length + 1} starters · {userPresets.length} yours</span>
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
            Presets describe how you want to use a model. Lemonade resolves the concrete runtime settings through Model Tuning for each model.
          </p>
          {importError && <p className="preset-error" role="alert">⚠ {importError}</p>}

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
                  <PresetCard key={preset.id} preset={preset} linkedModels={linkedModelsByPreset.get(preset.id)} highlight={highlightPresetId === preset.id} onClick={() => openSlideover(preset)} onApply={() => openSlideover(preset)} onClone={() => handleClone(preset)} onExport={() => handleExport(preset)} />
                ))}
              </div>
            ) : (
              <div className="empty-state--inset" data-empty="yours">
                <p className="preset-empty-title">Your presets are empty.</p>
                <p className="preset-empty-copy">Pick a starter, customize it, or create a preset from scratch.</p>
                <div className="preset-empty-actions">
                  <button className="btn btn--ghost" onClick={scrollToStarters}>Pick a starter</button>
                  <button className="btn btn--ghost" onClick={handleNewPreset}>+ New Preset</button>
                </div>
              </div>
            )}
          </div>

          <div className="zone zone--starters" ref={startersRef} tabIndex={-1} data-starter-zone>
            <div className="zone__head">
              <span className="zone__dot zone__dot--ready" />
              <span className="zone__title">Bundled starters</span>
              <span className="zone__count">{STARTERS.length + 1}</span>
              <span className="zone__rule" />
            </div>
            <div className="recipe-grid recipe-grid--starters-combined">
              <PresetCard preset={DEFAULT_PRESET} linkedModels={linkedModelsByPreset.get(DEFAULT_PRESET.id)} onClick={() => openSlideover(DEFAULT_PRESET)} onCustomize={() => handleCustomize(DEFAULT_PRESET)} onClone={() => handleClone(DEFAULT_PRESET)} />
              <div className="recipe-grid__contents" data-recipe-grid="starters">
                {STARTERS.map(preset => (
                  <PresetCard key={preset.id} preset={preset} linkedModels={linkedModelsByPreset.get(preset.id)} onClick={() => openSlideover(preset)} onCustomize={() => handleCustomize(preset)} onClone={() => handleClone(preset)} />
                ))}
              </div>
            </div>
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
            linkedModels={linkedModelsByPreset.get(selectedPreset.id) || []}
            applyTarget={applyTarget}
            onApplyTargetChange={setApplyTarget}
            onApply={handleApply}
            applySuccess={applySuccess}
            onSave={handleSave}
            onClone={preset => handleClone(preset)}
            onCustomize={handleCustomize}
            onExport={handleExport}
            onDelete={handleDelete}
            onClose={closeSlideover}
          />
        )}
      </aside>
    </>
  );
};

function linkedModelsText(preset: Preset, linkedModels: string[]): string {
  return `${preset.auto_opt_run_id ? 'Optimized for' : 'Linked to'} ${linkedModels.join(', ')}`;
}

const PresetCard: React.FC<{
  preset: Preset;
  linkedModels?: string[];
  highlight?: boolean;
  onClick: () => void;
  onClone?: () => void;
  onCustomize?: () => void;
  onApply?: () => void;
  onExport?: () => void;
}> = ({ preset, linkedModels, highlight, onClick, onClone, onCustomize, onApply, onExport }) => {
  const descId = `preset-card-desc-${preset.id}`;
  const capLabels = presetLabelsFor(preset).map(c => CAPABILITY_LABELS[c] || c).join(', ');
  const paramLines = paramsPreviewLines(preset);
  const descParts: string[] = [];
  if (preset.starter) descParts.push('Starter');
  descParts.push(`Applies to: ${capLabels}`);
  if (linkedModels?.length) descParts.push(linkedModelsText(preset, linkedModels));
  if (paramLines.length) descParts.push(`Intent: ${paramLines.join('; ')}`);
  descParts.push(`Prompt: ${promptDisplayText(preset)}`);
  descParts.push(`Tools: ${toolsDisplayText(preset)}`);
  return (
  <article
    className={`recipe-card${highlight ? ' recipe-card--flash' : ''}`}
    data-recipe-id={preset.id}
  >
    {/* Overlay button covers the card for pointer/keyboard activation without nesting interactive roles */}
    <button
      className="recipe-card__overlay-btn"
      onClick={onClick}
      aria-label={`Open Preset: ${preset.name}`}
      aria-describedby={descId}
    />
    {/* sr-only description for #2345: exposes intent/prompt/tools metadata to AT */}
    <span id={descId} className="sr-only">{descParts.join('. ')}.</span>
    {preset.starter && <span className="starter-badge">Starter</span>}
    <div className="recipe-card__head"><PresetIcon preset={preset} /><span className="recipe-card__name">{preset.name}</span></div>
    <p className="recipe-card__desc">{preset.description}</p>
    {linkedModels && linkedModels.length > 0 && (
      <p className={`recipe-card__linked${preset.auto_opt_run_id ? ' recipe-card__linked--optimized' : ''}`} data-preset-linked-models aria-hidden="true">
        <Icon name={preset.auto_opt_run_id ? 'gauge' : 'hard-drive'} size={12} aria-hidden="true" />
        {linkedModelsText(preset, linkedModels)}
      </p>
    )}
    <div className="cap-chip-list cap-chip-list--card" title="Applies to">
      {presetLabelsFor(preset).map(cap => <CapabilityChip key={cap} cap={cap} small />)}
    </div>
    <div className="recipe-card__params" aria-hidden="true">
      <span className="recipe-card__param-key">intent</span>
      <span className="recipe-card__param-val preset-param-lines">{paramsPreviewLines(preset).map(line => <span key={line}>{line}</span>)}</span>
    </div>
    <div className="recipe-card__behavior" aria-hidden="true">
      <span>prompt</span><strong>{promptDisplayText(preset)}</strong>
      <span>tools</span><strong>{toolsDisplayText(preset)}</strong>
    </div>
    <div className="recipe-card__actions" onClick={e => e.stopPropagation()}>
      {preset.starter ? (
        <>
          {onCustomize && <button className="recipe-card__action recipe-card__action--primary" onClick={onCustomize}>Customize</button>}
          {onClone && <button className="recipe-card__action" onClick={onClone}>Clone</button>}
        </>
      ) : (
        <>
          {onApply && <button className="recipe-card__action recipe-card__action--primary" onClick={onApply}>Apply</button>}
          {onClone && <button className="recipe-card__action" onClick={onClone}>Clone</button>}
          {onExport && <button className="recipe-card__action" onClick={onExport}>Export</button>}
        </>
      )}
    </div>
  </article>
  );
};

const TEMPERATURE_INTENT_OPTIONS: Array<{ value: TemperatureHint; icon: IconName; description: string }> = [
  { value: 'precise', icon: 'crosshair', description: 'Low-variation, exact responses' },
  { value: 'balanced', icon: 'scale', description: 'General-purpose balance' },
  { value: 'exploratory', icon: 'compass', description: 'Broader alternatives and ideas' },
  { value: 'creative', icon: 'lightbulb', description: 'Highest variation and imagination' },
];

const CONTEXT_INTENT_OPTIONS: Array<{ value: ContextHint; icon: IconName; description: string }> = [
  { value: 'small', icon: 'minimize-2', description: 'About 4K tokens, capped by the model' },
  { value: 'medium', icon: 'panel-top', description: 'About 40% of model context' },
  { value: 'large', icon: 'expand', description: 'About 66% of model context' },
  { value: 'max', icon: 'maximize-2', description: 'Full model-supported context' },
];

const THINKING_INTENT_OPTIONS: Array<{ value: ThinkingMode; icon: IconName; description: string; unavailable?: boolean }> = [
  { value: 'none', icon: 'brain-off', description: 'Disable explicit model thinking where supported' },
  { value: 'normal', icon: 'brain', description: 'Default model thinking' },
  { value: 'smart', icon: 'brain-cog', description: 'Not yet available', unavailable: true },
  { value: 'smart-extra', icon: 'brain-circuit', description: 'Not yet available', unavailable: true },
];

const SlideoverContent: React.FC<{
  preset: Preset;
  models: ModelInfo[];
  linkedModels: string[];
  applyTarget: string;
  onApplyTargetChange: (v: string) => void;
  onApply: (presetId: string, model: ModelInfo) => void;
  applySuccess: string | null;
  onSave: (updated: Preset) => void;
  onClone: (preset: Preset) => void;
  onCustomize: (preset: Preset) => void;
  onExport: (preset: Preset) => void;
  onDelete: (preset: Preset) => void;
  onClose: () => void;
}> = ({ preset, models, linkedModels, applyTarget, onApplyTargetChange, onApply, applySuccess, onSave, onClone, onCustomize, onExport, onDelete, onClose }) => {
  const isReadOnly = preset.starter;
  const [name, setName] = useState(preset.name);
  const [description, setDescription] = useState(preset.description);
  const [appliesTo, setAppliesTo] = useState<Capability[]>(normalizePresetCapabilities(preset.id, preset.applies_to));
  const [temperatureHint, setTemperatureHint] = useState<TemperatureHint>(preset.temperature_hint || 'balanced');
  const [contextHint, setContextHint] = useState<ContextHint>(preset.context_hint || 'medium');
  const [thinkingMode, setThinkingMode] = useState<ThinkingMode>(preset.thinking_mode || 'normal');
  const [systemPromptId, setSystemPromptId] = useState(preset.system_prompt_id || NO_SYSTEM_PROMPT_ID);
  const [systemPrompts, setSystemPrompts] = useState<PresetSystemPrompt[]>(cloneSystemPrompts(preset.system_prompts));
  const [toolsEnabled, setToolsEnabled] = useState(preset.tools_enabled !== false);
  const [saved, setSaved] = useState(false);
  const [autoOptState, setAutoOptState] = useState<AutoOptState>(() => autoOptStore.snapshot());
  const [missingRunNote, setMissingRunNote] = useState(false);

  useEffect(() => autoOptStore.subscribe(setAutoOptState), []);

  useEffect(() => {
    setMissingRunNote(false);
    setName(preset.name);
    setDescription(preset.description);
    setAppliesTo(normalizePresetCapabilities(preset.id, preset.applies_to));
    setTemperatureHint(preset.temperature_hint || 'balanced');
    setContextHint(preset.context_hint || 'medium');
    setThinkingMode(preset.thinking_mode || 'normal');
    setSystemPromptId(preset.system_prompt_id || NO_SYSTEM_PROMPT_ID);
    setSystemPrompts(cloneSystemPrompts(preset.system_prompts));
    setToolsEnabled(preset.tools_enabled !== false);
    setSaved(false);
  }, [preset]);

  const normalizedAppliesTo = normalizePresetCapabilities(preset.id, appliesTo);
  const hasChat = presetSupportsChatIntent({ id: preset.id, applies_to: normalizedAppliesTo });
  const currentPreset = useMemo<Preset>(() => {
    if (isReadOnly) return { ...preset, applies_to: normalizePresetCapabilities(preset.id, preset.applies_to) };
    return {
      ...preset,
      name,
      description,
      applies_to: normalizedAppliesTo,
      temperature_hint: hasChat ? temperatureHint : undefined,
      context_hint: hasChat ? contextHint : undefined,
      thinking_mode: hasChat ? thinkingMode : undefined,
      // Preserve legacy payloads invisibly for import/backend compatibility.
      // New presets start empty and the editor never creates concrete values.
      recipe_options: preset.recipe_options || {},
      sampling: preset.sampling || {},
      engine_hint: preset.engine_hint || 'auto',
      starter: false,
      system_prompt_id: hasChat ? systemPromptId : NO_SYSTEM_PROMPT_ID,
      system_prompts: hasChat ? cloneSystemPrompts(systemPrompts) : [],
      tools_enabled: hasChat && toolsEnabled,
    };
  }, [isReadOnly, preset, name, description, normalizedAppliesTo, hasChat, temperatureHint, contextHint, thinkingMode, systemPromptId, systemPrompts, toolsEnabled]);

  const selectedModel = models.find(model => modelName(model) === applyTarget);
  const canApply = !!selectedModel && isCompatible(currentPreset, selectedModel);
  const selectedSystemPrompt = systemPromptId === NO_SYSTEM_PROMPT_ID ? null : (systemPrompts.find(prompt => prompt.id === systemPromptId) || null);
  const selectedPromptIsCustom = selectedSystemPrompt?.built_in === false;

  const toggleCap = (cap: Capability) => {
    if (!isReadOnly) setAppliesTo([cap]);
  };

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

  const handleSave = () => {
    onSave(currentPreset);
    setSaved(true);
    setTimeout(() => setSaved(false), 1500);
  };

  const renderIntentOptions = <T extends string,>(
    label: string,
    icon: IconName,
    options: Array<{ value: T; icon: IconName; description: string; unavailable?: boolean }>,
    value: T,
    setValue: (next: T) => void,
    dataAttribute: string,
  ) => (
    <fieldset className="preset-intent-fieldset" data-preset-intent={dataAttribute}>
      <legend><Icon name={icon} size={15} aria-hidden="true" /> {label}</legend>
      <div className="preset-intent-options">
        {options.map(option => {
          const disabled = isReadOnly || !!option.unavailable;
          return (
            <button
              key={option.value}
              type="button"
              className="preset-intent-option"
              aria-pressed={value === option.value}
              disabled={disabled}
              title={label === 'Thinking' && option.value === 'normal'
                ? option.description
                : `${label === 'Temperature' ? TEMPERATURE_HINT_LABELS[option.value as TemperatureHint] : label === 'Context' ? CONTEXT_HINT_LABELS[option.value as ContextHint] : THINKING_MODE_LABELS[option.value as ThinkingMode]} — ${option.description}`}
              onClick={() => setValue(option.value)}
              data-intent-value={option.value}
            >
              <Icon name={option.icon} size={16} aria-hidden="true" />
              <span>{label === 'Temperature' ? TEMPERATURE_HINT_LABELS[option.value as TemperatureHint] : label === 'Context' ? CONTEXT_HINT_LABELS[option.value as ContextHint] : THINKING_MODE_LABELS[option.value as ThinkingMode]}</span>
              {option.unavailable && <small>Later</small>}
            </button>
          );
        })}
      </div>
    </fieldset>
  );

  return (
    <>
      <div className="slideover__head">
        <div className="slideover__top">
          <div className="slideover__title-wrap">
            <div className="slideover__title-with-icon">
              <PresetIcon preset={preset} className="preset-icon preset-icon--lg" />
              {isReadOnly ? <h2 className="slideover__title" data-recipe-name>{preset.name}</h2> : (
                <input className="slideover__title-input" value={name} onChange={event => setName(event.target.value)} placeholder="Preset name" data-recipe-name aria-label="Preset name" />
              )}
            </div>
          </div>
          <button className="slideover__close" onClick={onClose} aria-label="Close">✕</button>
        </div>
        <div className="slideover__meta-row">
          {normalizedAppliesTo.map(cap => <CapabilityChip key={cap} cap={cap} />)}
          {preset.starter && <span className="recipe-badge recipe-badge--starter" data-recipe-starter-badge>Starter</span>}
          {preset.auto_opt_run_id && (
            <button
              type="button"
              className="autoopt-preset-chip"
              onClick={() => {
                const runId = preset.auto_opt_run_id!;
                if (autoOptState.runs.some(run => run.id === runId)) {
                  setMissingRunNote(false);
                  openAutoOptRun(runId);
                } else {
                  setMissingRunNote(true);
                }
              }}
              title="Open the AutoOpt run that produced this preset"
              data-preset-autoopt-chip
            >
              ⚙ Optimized by AutoOpt
            </button>
          )}
        </div>
        {linkedModels.length > 0 && (
          <p className={`preset-linked-note${preset.auto_opt_run_id ? ' preset-linked-note--optimized' : ''}`} data-preset-editor-linked>
            <Icon name={preset.auto_opt_run_id ? 'gauge' : 'hard-drive'} size={13} aria-hidden="true" />
            {linkedModelsText(preset, linkedModels)}
          </p>
        )}
        {missingRunNote && <p className="preset-help" data-preset-autoopt-missing>Run no longer exists on this machine.</p>}
        {isReadOnly ? <p className="slideover__desc" data-recipe-desc>{preset.description}</p> : (
          <textarea className="slideover__desc-input" value={description} onChange={event => setDescription(event.target.value)} placeholder="Description (optional)" rows={2} data-recipe-desc aria-label="Description" />
        )}
      </div>

      <div className="slideover__body">
        <p className="preset-intent-explainer">Presets describe how you want to use a model. Lemonade resolves concrete runtime settings through Model Tuning for each model.</p>

        <div className="slideover__section">
          <h3>Applies to</h3>
          <div className="cap-chip-list" data-preset-capabilities role="group" aria-label="Applies to capabilities">
            {CAPABILITIES.map(cap => (
              <button key={cap} type="button" className="preset-cap-button" disabled={isReadOnly} onClick={() => toggleCap(cap)} aria-pressed={normalizedAppliesTo.includes(cap)}>
                <CapabilityChip cap={cap} on={normalizedAppliesTo.includes(cap)} off={!normalizedAppliesTo.includes(cap)} />
              </button>
            ))}
          </div>
        </div>

        {hasChat && (
          <div className="slideover__section preset-intent-controls" data-preset-fields="intent">
            {renderIntentOptions('Temperature', 'thermometer', TEMPERATURE_INTENT_OPTIONS, temperatureHint, setTemperatureHint, 'temperature')}
            {renderIntentOptions('Context', 'scan-text', CONTEXT_INTENT_OPTIONS, contextHint, setContextHint, 'context')}
            {renderIntentOptions('Thinking', 'brain', THINKING_INTENT_OPTIONS, thinkingMode, setThinkingMode, 'thinking')}

            <fieldset className="preset-intent-fieldset" data-preset-intent="tools">
              <legend><Icon name="wrench" size={15} aria-hidden="true" /> Tools</legend>
              <button
                type="button"
                className="preset-tools-toggle"
                aria-pressed={toolsEnabled}
                disabled={isReadOnly}
                onClick={() => setToolsEnabled(enabled => !enabled)}
                title={toolsEnabled ? 'Tools enabled' : 'Tools disabled'}
              >
                <Icon name={toolsEnabled ? 'wrench' : 'wrench-off'} size={17} aria-hidden="true" />
                <span>{toolsEnabled ? 'Enabled' : 'Off'}</span>
              </button>
            </fieldset>
          </div>
        )}

        {hasChat && (
          <div className="slideover__section preset-system-prompt">
            <h3>System Prompt</h3>
            <div className="field">
              <label className="field__label" htmlFor="preset-system-prompt-type">Prompt</label>
              <div className="field__row">
                <select id="preset-system-prompt-type" className="select" value={systemPromptId} disabled={isReadOnly} onChange={event => setSystemPromptId(event.target.value)}>
                  <option value={NO_SYSTEM_PROMPT_ID}>No system prompt</option>
                  {systemPrompts.map(prompt => <option key={prompt.id} value={prompt.id}>{prompt.name}</option>)}
                </select>
              </div>
            </div>
            <details className="preset-prompt-details">
              <summary>{selectedSystemPrompt ? `Prompt text: ${selectedSystemPrompt.name}` : 'Prompt text'}</summary>
              {!selectedSystemPrompt ? (
                <p className="preset-help">No system prompt is sent for this preset.</p>
              ) : selectedPromptIsCustom && !isReadOnly ? (
                <div className="preset-prompt-editor">
                  <div className="field">
                    <label className="field__label">Displayed name</label>
                    <input className="input" value={selectedSystemPrompt.name} onChange={event => updateSelectedSystemPrompt({ name: event.target.value })} />
                  </div>
                  <div className="field">
                    <label className="field__label">System prompt text</label>
                    <textarea className="input preset-prompt-textarea" rows={7} value={selectedSystemPrompt.prompt} onChange={event => updateSelectedSystemPrompt({ prompt: event.target.value })} />
                  </div>
                  <div className="preset-prompt-actions">
                    <button type="button" className="btn btn--ghost btn--tiny" onClick={deleteSelectedCustomPrompt}>Delete custom prompt</button>
                  </div>
                </div>
              ) : (
                <p className="preset-prompt-preview">{selectedSystemPrompt.prompt}</p>
              )}
            </details>
            {!isReadOnly && <button type="button" className="btn btn--ghost btn--tiny" onClick={addCustomSystemPrompt}>+ Custom prompt</button>}
          </div>
        )}

        <div className="slideover__section">
          <h3>Apply to a model</h3>
          <p className="preset-help">The intent is linked now. Concrete values resolve through Model Tuning for this model × preset.</p>
          <div className="field__row">
            <select className="select" value={applyTarget} onChange={event => onApplyTargetChange(event.target.value)} data-recipe-apply-target>
              <option value="">— pick a model —</option>
              {models.map(model => {
                const nameForModel = modelName(model);
                const caps = labelsFor(model);
                const compatible = isCompatible(currentPreset, model);
                const reason = compatible ? `${caps.map(cap => CAPABILITY_LABELS[cap]).join(', ')}` : `Incompatible: needs ${currentPreset.applies_to.map(cap => CAPABILITY_LABELS[cap]).join(' or ')}; this model exposes ${caps.map(cap => CAPABILITY_LABELS[cap]).join(', ')}`;
                return <option key={nameForModel} value={nameForModel} disabled={!compatible} title={reason}>{nameForModel} · {caps.map(cap => CAPABILITY_LABELS[cap]).join(', ')}</option>;
              })}
            </select>
            <button className="btn btn--primary" disabled={!canApply} onClick={() => selectedModel && onApply(currentPreset.id, selectedModel)}>Apply</button>
          </div>
          {selectedModel && !canApply && <p className="preset-error" role="tooltip">Incompatible preset for this model.</p>}
          {applySuccess && <p className="preset-success">✓ {applySuccess}</p>}
        </div>

      </div>

      <div className="slideover__foot">
        <button className="btn btn--ghost" onClick={() => onExport(currentPreset)}>Export</button>
        {preset.starter ? (
          <>
            <button className="btn btn--ghost" onClick={() => onClone(preset)} data-recipe-clone>Clone</button>
            <button className="btn btn--primary" onClick={() => onCustomize(preset)} data-recipe-customize>Customize</button>
          </>
        ) : (
          <>
            <button className="btn btn--ghost" onClick={() => onClone(currentPreset)} data-recipe-clone>Clone</button>
            <button className="btn btn--ghost" style={{ color: 'var(--danger)' }} onClick={() => onDelete(preset)} data-recipe-delete>Delete</button>
            <button className={`btn btn--primary${saved ? ' btn--saved' : ''}`} onClick={handleSave}>{saved ? '✓ Saved' : 'Save'}</button>
          </>
        )}
      </div>
    </>
  );
};

export default PresetManager;
