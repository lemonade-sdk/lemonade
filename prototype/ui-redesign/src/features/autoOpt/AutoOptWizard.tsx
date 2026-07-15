import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import api, { LoadedModel, ModelInfo } from '../../api';
import { labelsFor } from '../../presetStore';
import { Icon } from '../../components/Icon';
import { useFocusTrap } from '../../hooks/useFocusTrap';
import {
  AutoOptBudget,
  AutoOptKvCacheQuant,
  AutoOptParallelMode,
  AutoOptRamHeadroom,
  AutoOptStartRequest,
  isAutoOptRunActive,
} from './autoOptTypes';
import { autoOptStore, AutoOptState } from './autoOptStore';
import {
  BUDGET_LABELS,
  BUDGET_STEP,
  KV_QUANT_LABELS,
  KV_QUANT_STEP,
  MODEL_STEP,
  PARALLEL_STEP,
  RAM_HEADROOM_LABELS,
  RAM_STEP,
  REVIEW_STEP,
  RUNNING_STEP,
  WIZARD_INTRO,
  WIZARD_TITLE,
} from './wizardCopy';

type WizardStep = 'model' | 'parallel' | 'kv' | 'ram' | 'budget' | 'review' | 'running';

function modelName(model: ModelInfo): string {
  return String((model as Record<string, unknown>).model_name || model.name || model.id || '').trim();
}

function isEligibleModel(model: ModelInfo): boolean {
  if ((model as Record<string, unknown>).downloaded !== true) return false;
  const recipe = String((model as Record<string, unknown>).recipe || '').trim().toLowerCase();
  if (recipe !== 'llamacpp') return false;
  const caps = labelsFor(model);
  return caps.includes('chat') || caps.includes('omni');
}

function physicalMemoryGb(info: Record<string, unknown> | null): number | null {
  if (!info) return null;
  const raw = info['Physical Memory'] ?? (info as Record<string, unknown>).physical_memory ?? (info as Record<string, unknown>).memory_gb;
  if (typeof raw === 'number' && Number.isFinite(raw)) return raw;
  if (typeof raw === 'string') {
    const match = /([\d.]+)/.exec(raw);
    const n = match ? Number(match[1]) : NaN;
    if (Number.isFinite(n)) return raw.toLowerCase().includes('mb') ? n / 1024 : n;
  }
  return null;
}

function suggestedHeadroom(gb: number): AutoOptRamHeadroom {
  if (gb >= 64) return 'normal';
  if (gb >= 32) return 'reduced';
  if (gb >= 16) return 'minimal';
  return 'disabled';
}

const AutoOptWizard: React.FC<{
  open: boolean;
  onClose: () => void;
  loadedModels: LoadedModel[];
}> = ({ open, onClose, loadedModels }) => {
  const [step, setStep] = useState<WizardStep>('model');
  const [models, setModels] = useState<ModelInfo[]>([]);
  const [selectedModel, setSelectedModel] = useState('');
  const [parallelMode, setParallelMode] = useState<AutoOptParallelMode>('single');
  const [slots, setSlots] = useState(2);
  const [dedicated, setDedicated] = useState(false);
  const [kvQuant, setKvQuant] = useState<AutoOptKvCacheQuant>('q8_0');
  const [ramHeadroom, setRamHeadroom] = useState<AutoOptRamHeadroom>('normal');
  const [ramGb, setRamGb] = useState<number | null>(null);
  const [budget, setBudget] = useState<AutoOptBudget>('standard');
  const [allowNetwork, setAllowNetwork] = useState(true);
  const [consent, setConsent] = useState(false);
  const [submitError, setSubmitError] = useState<string | null>(null);
  const [runId, setRunId] = useState<string | null>(null);
  const [storeState, setStoreState] = useState<AutoOptState>(() => autoOptStore.snapshot());
  const slideoverRef = useRef<HTMLElement>(null);
  const ramTouchedRef = useRef(false);

  useFocusTrap(slideoverRef, open);

  useEffect(() => {
    if (!open) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') onClose();
    };
    document.addEventListener('keydown', onKey);
    return () => document.removeEventListener('keydown', onKey);
  }, [open, onClose]);

  useEffect(() => autoOptStore.subscribe(setStoreState), []);

  useEffect(() => {
    if (!open) return;
    setStep('model');
    setSubmitError(null);
    setRunId(null);
    setConsent(false);
    ramTouchedRef.current = false;
    let alive = true;
    api.models(true).then(data => { if (alive) setModels((data.data || []).filter(isEligibleModel)); }).catch(() => {
      if (alive) setModels(api.allModels.filter(isEligibleModel));
    });
    const applySuggestion = (info: Record<string, unknown> | null) => {
      if (!alive) return;
      const gb = physicalMemoryGb(info);
      setRamGb(gb);
      if (gb !== null && !ramTouchedRef.current) setRamHeadroom(suggestedHeadroom(gb));
    };
    api.systemInfo().then(applySuggestion).catch(() => applySuggestion(api.systemInfoData));
    return () => { alive = false; };
  }, [open]);

  useEffect(() => {
    if (!open || selectedModel) return;
    const loadedChat = loadedModels.find(model => model.type === 'chat' || model.type === 'omni' || model.type === 'llm')
      || loadedModels[0];
    if (loadedChat && models.some(model => modelName(model) === loadedChat.model_name)) {
      setSelectedModel(loadedChat.model_name);
    }
  }, [open, selectedModel, loadedModels, models]);

  const steps = useMemo<WizardStep[]>(() => [
    'model', 'parallel', 'kv', 'ram', 'budget', 'review',
  ], []);

  const stepIndex = steps.indexOf(step);
  const benchmarkTier = budget !== 'quick';
  const consentRequired = benchmarkTier && loadedModels.length > 0;
  const canStart = !!selectedModel && (!consentRequired || consent);

  const goBack = useCallback(() => {
    if (stepIndex > 0) setStep(steps[stepIndex - 1]);
  }, [stepIndex, steps]);

  const goNext = useCallback(() => {
    if (stepIndex >= 0 && stepIndex < steps.length - 1) setStep(steps[stepIndex + 1]);
  }, [stepIndex, steps]);

  const buildRequest = useCallback((): AutoOptStartRequest => ({
    model: selectedModel,
    budget,
    allow_unload: consentRequired ? consent : false,
    answers: {
      parallel: parallelMode === 'parallel'
        ? { mode: 'parallel', slots, dedicated }
        : { mode: 'single' },
      kv_cache_quant: kvQuant,
      ram_headroom: ramHeadroom,
      allow_network: allowNetwork,
    },
  }), [selectedModel, budget, consentRequired, consent, parallelMode, slots, dedicated, kvQuant, ramHeadroom, allowNetwork]);

  const handleStart = useCallback(async () => {
    setSubmitError(null);
    try {
      const id = await autoOptStore.startRun(buildRequest());
      autoOptStore.setActiveRun(id);
      setRunId(id);
      setStep('running');
    } catch (err) {
      setSubmitError(err instanceof Error ? err.message : 'Could not start the optimizer.');
    }
  }, [buildRequest]);

  const activeRun = runId ? storeState.runs.find(run => run.id === runId) : undefined;

  const renderOptions = <T,>(
    options: Array<{ value: T; label: string; description: string }>,
    value: T,
    setValue: (next: T) => void,
    dataAttribute: string,
  ) => (
    <div className="autoopt-wizard__options">
      {options.map(option => (
        <button
          key={String(option.value)}
          type="button"
          className="autoopt-option"
          aria-pressed={value === option.value}
          onClick={() => setValue(option.value)}
          data-autoopt-option={`${dataAttribute}:${String(option.value)}`}
        >
          <strong>{option.label}</strong>
          <span>{option.description}</span>
        </button>
      ))}
    </div>
  );

  const renderStep = () => {
    switch (step) {
      case 'model':
        return (
          <fieldset className="preset-intent-fieldset autoopt-wizard__fieldset" data-autoopt-step="model">
            <legend>{MODEL_STEP.legend}</legend>
            <p className="preset-help">{MODEL_STEP.help}</p>
            <p className="preset-help" data-autoopt-model-note>{MODEL_STEP.note}</p>
            {models.length === 0 ? (
              <p className="preset-help" data-autoopt-no-models>{MODEL_STEP.empty}</p>
            ) : (
              <select
                className="select"
                value={selectedModel}
                onChange={event => setSelectedModel(event.target.value)}
                aria-label="Model to optimize"
                data-autoopt-model-select
              >
                <option value="">— pick a model —</option>
                {models.map(model => {
                  const name = modelName(model);
                  return <option key={name} value={name}>{name}</option>;
                })}
              </select>
            )}
          </fieldset>
        );
      case 'parallel':
        return (
          <fieldset className="preset-intent-fieldset autoopt-wizard__fieldset" data-autoopt-step="parallel">
            <legend>{PARALLEL_STEP.legend}</legend>
            {renderOptions(PARALLEL_STEP.options, parallelMode, setParallelMode, 'parallel')}
            {parallelMode === 'parallel' && (
              <div className="autoopt-wizard__subfields">
                <label className="field">
                  <span className="field__label">{PARALLEL_STEP.slotsLabel}</span>
                  <input
                    className="input"
                    type="number"
                    min={2}
                    max={16}
                    value={slots}
                    onChange={event => setSlots(Math.max(2, Math.min(16, Math.round(Number(event.target.value) || 2))))}
                    data-autoopt-slots
                  />
                </label>
                <label className="autoopt-wizard__checkbox">
                  <input type="checkbox" checked={dedicated} onChange={event => setDedicated(event.target.checked)} data-autoopt-dedicated />
                  <span>{PARALLEL_STEP.dedicatedLabel}</span>
                </label>
              </div>
            )}
            <p className="preset-help autoopt-wizard__footnote">{PARALLEL_STEP.footnote}</p>
          </fieldset>
        );
      case 'kv':
        return (
          <fieldset className="preset-intent-fieldset autoopt-wizard__fieldset" data-autoopt-step="kv">
            <legend>{KV_QUANT_STEP.legend}</legend>
            <p className="preset-help">{KV_QUANT_STEP.help}</p>
            {renderOptions(KV_QUANT_STEP.options, kvQuant, setKvQuant, 'kv')}
          </fieldset>
        );
      case 'ram':
        return (
          <fieldset className="preset-intent-fieldset autoopt-wizard__fieldset" data-autoopt-step="ram">
            <legend>{RAM_STEP.legend}</legend>
            <p className="preset-help">{RAM_STEP.help}</p>
            {ramGb !== null && (
              <span className="autoopt-suggestion-chip" data-autoopt-ram-suggestion>
                {RAM_STEP.suggestionChip(Math.round(ramGb))}
              </span>
            )}
            {renderOptions(RAM_STEP.options, ramHeadroom, value => {
              ramTouchedRef.current = true;
              setRamHeadroom(value);
            }, 'ram')}
          </fieldset>
        );
      case 'budget':
        return (
          <fieldset className="preset-intent-fieldset autoopt-wizard__fieldset" data-autoopt-step="budget">
            <legend>{BUDGET_STEP.legend}</legend>
            {renderOptions(BUDGET_STEP.options, budget, setBudget, 'budget')}
            <label className="autoopt-wizard__checkbox">
              <input type="checkbox" checked={allowNetwork} onChange={event => setAllowNetwork(event.target.checked)} data-autoopt-network />
              <span>{BUDGET_STEP.networkLabel}</span>
            </label>
            <p className="preset-help">{BUDGET_STEP.networkHelp}</p>
            {consentRequired && (
              <>
                <label className="autoopt-wizard__checkbox autoopt-wizard__checkbox--consent">
                  <input type="checkbox" checked={consent} onChange={event => setConsent(event.target.checked)} data-autoopt-consent />
                  <span>{BUDGET_STEP.consentLabel}</span>
                </label>
                <p className="preset-help">{BUDGET_STEP.consentHelp}</p>
              </>
            )}
          </fieldset>
        );
      case 'review':
        return (
          <fieldset className="preset-intent-fieldset autoopt-wizard__fieldset" data-autoopt-step="review">
            <legend>{REVIEW_STEP.legend}</legend>
            <p className="preset-help">{REVIEW_STEP.help}</p>
            <dl className="autoopt-review">
              <dt>Model</dt><dd>{selectedModel || '—'}</dd>
              <dt>Usage</dt><dd>{parallelMode === 'parallel' ? `Parallel · ${slots} slots${dedicated ? ' · dedicated server' : ''}` : 'Single user'}</dd>
              <dt>KV cache</dt><dd>{KV_QUANT_LABELS[kvQuant]}</dd>
              <dt>Prompt cache</dt><dd>{RAM_HEADROOM_LABELS[ramHeadroom]}</dd>
              <dt>Budget</dt><dd>{BUDGET_LABELS[budget]}</dd>
              <dt>Network</dt><dd>{allowNetwork ? 'Fetch model metadata from Hugging Face' : 'No network access'}</dd>
              <dt>Unload models</dt><dd>{!benchmarkTier ? 'Not needed for Fast Scan' : (loadedModels.length === 0 ? 'Nothing loaded' : (consent ? 'Allowed' : 'Not allowed'))}</dd>
            </dl>
            {consentRequired && !consent && (
              <p className="preset-error" data-autoopt-consent-gate>Confirm the unload consent on the previous step to start.</p>
            )}
            {submitError && <p className="preset-error" role="alert" data-autoopt-submit-error>⚠ {submitError}</p>}
          </fieldset>
        );
      case 'running':
        return (
          <div className="autoopt-wizard__running" data-autoopt-step="running">
            <h3>{activeRun && !isAutoOptRunActive(activeRun) ? 'Run finished' : 'Optimizing…'}</h3>
            {activeRun?.progress?.detail && (
              <p className="preset-help" data-autoopt-progress>{activeRun.progress.detail}</p>
            )}
            {activeRun && activeRun.stages.length > 0 && (
              <ul className="autoopt-stage-list" data-autoopt-stage-list>
                {activeRun.stages.map(stage => (
                  <li key={stage.name} className={`autoopt-stage autoopt-stage--${stage.status}`}>
                    <span className="autoopt-stage__marker" aria-hidden="true" />
                    <span className="autoopt-stage__name">{stage.name}</span>
                    <span className="autoopt-stage__status">{stage.status}</span>
                  </li>
                ))}
              </ul>
            )}
            {activeRun?.status === 'failed' && activeRun.error && (
              <p className="preset-error" role="alert">⚠ {activeRun.error}</p>
            )}
            <p className="preset-help" data-autoopt-close-note>{RUNNING_STEP.closeNote}</p>
          </div>
        );
    }
  };

  if (!open) return null;

  return (
    <>
      <div className="scrim scrim--autoopt-wizard is-open" onClick={onClose} />
      <aside
        ref={slideoverRef}
        className="slideover slideover--wide slideover--autoopt-wizard is-open"
        role="dialog"
        aria-modal="true"
        aria-label="AutoOpt wizard"
        data-autoopt-wizard
      >
        <>
            <div className="slideover__head">
              <div className="slideover__top">
                <div className="slideover__title-wrap">
                  <h2 className="slideover__title">{WIZARD_TITLE}</h2>
                </div>
                <button className="slideover__close" onClick={onClose} aria-label="Close">✕</button>
              </div>
              <p className="slideover__desc">{WIZARD_INTRO}</p>
              {step !== 'running' && (
                <div className="autoopt-wizard__progress" aria-hidden="true">
                  {steps.map((s, index) => (
                    <span key={s} className={`autoopt-wizard__dot${index === stepIndex ? ' is-active' : ''}${index < stepIndex ? ' is-done' : ''}`} />
                  ))}
                </div>
              )}
            </div>

            <div className="slideover__body autoopt-wizard__body">
              {renderStep()}
            </div>

            <div className="slideover__foot">
              {step === 'running' ? (
                <>
                  {activeRun && isAutoOptRunActive(activeRun) && runId && (
                    <button
                      className="btn btn--ghost"
                      onClick={() => autoOptStore.cancelRun(runId)}
                      data-autoopt-cancel-run
                    >
                      Cancel run
                    </button>
                  )}
                  <button className="btn btn--primary" onClick={onClose}>Close</button>
                </>
              ) : (
                <>
                  <button className="btn btn--ghost" onClick={goBack} disabled={stepIndex <= 0} data-autoopt-back>Back</button>
                  {step === 'review' ? (
                    <button className="btn btn--primary" onClick={() => void handleStart()} disabled={!canStart} data-autoopt-start>
                      <Icon name="play" size={14} aria-hidden="true" /> Start optimization
                    </button>
                  ) : (
                    <button className="btn btn--primary" onClick={goNext} disabled={step === 'model' && !selectedModel} data-autoopt-next>Next</button>
                  )}
                </>
              )}
            </div>
        </>
      </aside>
    </>
  );
};

export default AutoOptWizard;
