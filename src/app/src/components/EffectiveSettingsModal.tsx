import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import api, { type ModelInfo, type ModelOptions, type LoadedModel, friendlyErrorMessage } from '../api';
import {
  type RecipeOptions,
  type SamplingParams,
  type TuningValueSource,
  DEFAULT_CONTEXT_SIZE,
  THINKING_MODE_LABELS,
  backendArgsFieldForRecipe,
  backendSupportsArgs,
  clearSessionArgsOverride,
  effectiveModelTuningForModel,
  getSessionArgsOverride,
  loadModelTuning,
  saveModelTuning,
  setSessionArgsOverride,
} from '../modelConfiguration';
import { Icon } from './Icon';
import { WorkspaceActionButton } from './WorkspacePanels';

const RECIPE_OPTION_LABELS: Partial<Record<keyof RecipeOptions, string>> = {
  ctx_size: 'Context size',
  llamacpp_backend: 'Backend',
  llamacpp_device: 'Device',
  llamacpp_args: 'Backend args',
  vllm_backend: 'Backend',
  vllm_args: 'Backend args',
  flm_args: 'Backend args',
  whispercpp_backend: 'Backend',
  whispercpp_args: 'Backend args',
  moonshine_backend: 'Backend',
  moonshine_args: 'Backend args',
  sdcpp_args: 'Backend args',
  'sd-cpp_backend': 'Backend',
  steps: 'Steps',
  cfg_scale: 'CFG scale',
  width: 'Width',
  height: 'Height',
  sampling_method: 'Sampling method',
  flow_shift: 'Flow shift',
  voice: 'Voice',
  speed: 'Speed',
};

const SAMPLING_LABELS: Partial<Record<keyof SamplingParams, string>> = {
  temperature: 'Temperature',
  top_p: 'Top-p',
  top_k: 'Top-k',
  min_p: 'Min-p',
  repeat_penalty: 'Repeat penalty',
};

function sourceLabel(source: TuningValueSource | undefined): string {
  switch (source) {
    case 'custom': return 'Direct configuration';
    case 'built_in': return 'Recipe default';
    case 'optimized': return 'Optimized';
    default: return 'Default';
  }
}

function sourceClass(source: TuningValueSource | undefined): string {
  switch (source) {
    case 'custom': return 'effective-settings__source--custom';
    case 'built_in': return 'effective-settings__source--builtin';
    case 'optimized': return 'effective-settings__source--optimized';
    default: return 'effective-settings__source--generic';
  }
}

function displayValue(value: unknown): string {
  if (value === null || value === undefined || value === '') return '—';
  if (typeof value === 'boolean') return value ? 'On' : 'Off';
  if (typeof value === 'number') return String(value);
  return String(value);
}

function positiveContextSize(value: unknown): number | null {
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed > 0 ? Math.round(parsed) : null;
}

function isAutoContextSize(value: unknown): boolean {
  return value === 'auto' || Number(value) === -1;
}

interface SourceRow {
  key: string;
  label: string;
  value: string;
  source: TuningValueSource | undefined;
}

interface EffectiveSettingsModalProps {
  open: boolean;
  onClose: () => void;
  modelName: string;
  modelInfo: ModelInfo | null;
  recipe: string;
  mcpEnabled: boolean;
  mcpServerIds: string[];
  fallbackCtxSize?: number;
  loadedModel?: LoadedModel | null;
  isModelLoaded: boolean;
  onReload: () => Promise<void>;
  onLoad: () => Promise<void>;
}

const EffectiveSettingsModal: React.FC<EffectiveSettingsModalProps> = ({
  open, onClose, modelName, modelInfo, recipe, mcpEnabled, mcpServerIds, fallbackCtxSize, loadedModel, isModelLoaded, onReload, onLoad,
}) => {
  const argsField = backendArgsFieldForRecipe(recipe);
  const canEditArgs = backendSupportsArgs(recipe) && !!argsField;

  const [effective, setEffective] = useState<ModelOptions | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const [unlocked, setUnlocked] = useState(false);
  const [draft, setDraft] = useState('');
  const [preview, setPreview] = useState<ModelOptions | null>(null);
  const [previewError, setPreviewError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [notice, setNotice] = useState<string | null>(null);
  const [samplingDraft, setSamplingDraft] = useState<Record<keyof SamplingParams, string>>({
    temperature: '',
    top_p: '',
    top_k: '',
    min_p: '',
    repeat_penalty: '',
  });
  const [loadedContextSize, setLoadedContextSize] = useState<number | null>(null);
  const [resolvingContextSize, setResolvingContextSize] = useState(false);

  const hasOverride = !!getSessionArgsOverride(modelName);

  const resolved = useMemo(() => {
    if (!modelInfo || !open) return null;
    try {
      return effectiveModelTuningForModel(modelName, modelInfo, fallbackCtxSize ?? DEFAULT_CONTEXT_SIZE);
    } catch {
      return null;
    }
  }, [modelName, modelInfo, fallbackCtxSize, open]);

  const loadEffective = useCallback(async () => {
    if (!modelName) return;
    setLoading(true);
    setError(null);
    try {
      const result = await api.effectiveModelOptions(modelName, undefined, modelInfo);
      setEffective(result);
      const committed = argsField ? result.effective[argsField] : undefined;
      setDraft(typeof committed === 'string' ? committed : '');
    } catch (err) {
      setError(friendlyErrorMessage(err));
      setEffective(null);
    } finally {
      setLoading(false);
    }
  }, [modelName, modelInfo, argsField]);

  useEffect(() => {
    if (!open) return;
    setUnlocked(false);
    setNotice(null);
    setPreview(null);
    setPreviewError(null);
    loadEffective();
    const savedSampling = loadModelTuning(modelName)?.sampling || {};
    setSamplingDraft({
      temperature: savedSampling.temperature === undefined ? '' : String(savedSampling.temperature),
      top_p: savedSampling.top_p === undefined ? '' : String(savedSampling.top_p),
      top_k: savedSampling.top_k === undefined ? '' : String(savedSampling.top_k),
      min_p: savedSampling.min_p === undefined ? '' : String(savedSampling.min_p),
      repeat_penalty: savedSampling.repeat_penalty === undefined ? '' : String(savedSampling.repeat_penalty),
    });
  }, [open, loadEffective]);

  useEffect(() => {
    if (!open || !loadedModel) {
      setLoadedContextSize(null);
      setResolvingContextSize(false);
      return;
    }
    let cancelled = false;
    let settled = false;
    setLoadedContextSize(null);
    setResolvingContextSize(true);
    const timeout = window.setTimeout(() => {
      if (cancelled || settled) return;
      settled = true;
      setResolvingContextSize(false);
    }, 5000);
    void api.loadedModelContextSize(loadedModel).then(contextSize => {
      if (cancelled || settled) return;
      settled = true;
      window.clearTimeout(timeout);
      setLoadedContextSize(contextSize);
      setResolvingContextSize(false);
    });
    return () => {
      cancelled = true;
      window.clearTimeout(timeout);
    };
  }, [open, loadedModel]);

  useEffect(() => {
    if (!open || !unlocked || !argsField) { setPreview(null); return; }
    let cancelled = false;
    const handle = setTimeout(async () => {
      setPreviewError(null);
      try {
        const result = await api.effectiveModelOptions(modelName, { [argsField]: draft, merge_args: false }, modelInfo);
        if (!cancelled) setPreview(result);
      } catch (err) {
        if (!cancelled) { setPreview(null); setPreviewError(friendlyErrorMessage(err)); }
      }
    }, 350);
    return () => { cancelled = true; clearTimeout(handle); };
  }, [open, unlocked, draft, argsField, modelName, modelInfo]);

  const closeRef = useRef<HTMLButtonElement>(null);
  useEffect(() => {
    if (open) closeRef.current?.focus();
  }, [open]);

  useEffect(() => {
    if (!open) return;
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') onClose(); };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [open, onClose]);

  const applyOverride = useCallback(async () => {
    if (!argsField) return;
    setBusy(true);
    setNotice(null);
    const previousOverride = getSessionArgsOverride(modelName);
    const restorePreviousOverride = () => {
      if (previousOverride) setSessionArgsOverride(modelName, previousOverride.recipe, previousOverride.args);
      else clearSessionArgsOverride(modelName);
    };
    try {
      setSessionArgsOverride(modelName, recipe, draft.trim());
      if (isModelLoaded) {
        try {
          await onReload();
        } catch (reloadErr) {
          restorePreviousOverride();
          try { await onLoad(); } catch { /* keep the original failure */ }
          throw reloadErr;
        }
        setNotice('Applied and reloaded with the new arguments.');
      } else {
        setNotice('Saved for this session. It will take effect the next time this model loads.');
      }
      setUnlocked(false);
    } catch (err) {
      setNotice(friendlyErrorMessage(err));
    } finally {
      await loadEffective();
      setBusy(false);
    }
  }, [argsField, modelName, recipe, draft, isModelLoaded, onReload, onLoad, loadEffective]);

  const resetOverride = useCallback(async () => {
    setBusy(true);
    setNotice(null);
    try {
      clearSessionArgsOverride(modelName);
      if (isModelLoaded) {
        await onReload();
        setNotice('Cleared the session override and reloaded with resolved settings.');
      } else {
        setNotice('Cleared the session override.');
      }
      await loadEffective();
      setUnlocked(false);
    } catch (err) {
      setNotice(friendlyErrorMessage(err));
    } finally {
      setBusy(false);
    }
  }, [modelName, isModelLoaded, onReload, loadEffective]);

  const resolvedContextRaw = resolved?.tuning.recipe_options?.ctx_size;
  const autoContextSizeEnabled = isAutoContextSize(loadModelTuning(modelName)?.recipe_options?.ctx_size);

  const contextSetting = useMemo(() => {
    const runtimeContext = positiveContextSize(loadedContextSize);
    if (autoContextSizeEnabled) {
      if (runtimeContext !== null) {
        return { value: `${runtimeContext.toLocaleString()} (auto)`, source: 'Runtime' };
      }
      if (loadedModel && resolvingContextSize) return { value: 'Resolving…', source: 'Runtime' };
      return { value: 'Auto', source: 'Configuration' };
    }

    if (runtimeContext !== null) {
      return { value: runtimeContext.toLocaleString(), source: 'Runtime' };
    }

    const effectiveContext = positiveContextSize(effective?.resolved_ctx_size);
    if (effectiveContext !== null) {
      return { value: effectiveContext.toLocaleString(), source: 'Effective load' };
    }

    const resolvedContext = positiveContextSize(resolvedContextRaw);
    if (resolvedContext !== null) {
      return {
        value: resolvedContext.toLocaleString(),
        source: sourceLabel(resolved?.sources.recipe_options.ctx_size),
      };
    }

    if (loadedModel && resolvingContextSize) return { value: 'Resolving…', source: 'Runtime' };
    return { value: 'Unavailable', source: 'Configuration' };
  }, [autoContextSizeEnabled, effective, loadedContextSize, loadedModel, resolved, resolvedContextRaw, resolvingContextSize]);

  const sourceRows = useMemo<SourceRow[]>(() => {
    if (!resolved) return [];
    const rows: SourceRow[] = [];
    for (const [key, value] of Object.entries(resolved.tuning.recipe_options || {})) {
      if (key === 'merge_args' || key === 'mmproj_enabled' || key === 'ctx_size') continue;
      rows.push({
        key: `ro-${key}`,
        label: RECIPE_OPTION_LABELS[key as keyof RecipeOptions] || key,
        value: displayValue(value),
        source: resolved.sources.recipe_options[key as keyof RecipeOptions],
      });
    }
    return rows;
  }, [resolved]);

  const samplingInputId = (key: keyof SamplingParams) => `effective-sampling-${key}`;
  const stepSamplingInput = (key: keyof SamplingParams, direction: -1 | 1) => {
    const input = document.getElementById(samplingInputId(key));
    if (!(input instanceof HTMLInputElement)) return;
    if (direction > 0) input.stepUp();
    else input.stepDown();
    setSamplingDraft(current => ({ ...current, [key]: input.value }));
  };

  const saveSampling = () => {
    const sampling: SamplingParams = {};
    for (const key of Object.keys(samplingDraft) as Array<keyof SamplingParams>) {
      const value = Number(samplingDraft[key]);
      if (samplingDraft[key].trim() && Number.isFinite(value)) sampling[key] = value;
    }
    const existing = loadModelTuning(modelName);
    saveModelTuning(modelName, {
      ...(existing || {}),
      recipe_options: existing?.recipe_options || {},
      sampling,
    });
    setNotice(Object.keys(sampling).length > 0
      ? 'Sampling overrides will be sent with future chat requests.'
      : 'Chat requests will use the backend sampling defaults.');
  };

  if (!open) return null;

  const shown = preview ?? effective;
  const loadCommand = shown?.load_command ?? '';
  const backend = shown ? shown.effective[`${shown.recipe}_backend`] : undefined;
  const backendLabel = typeof backend === 'string' && backend ? backend : '—';

  const body = (
    <div className="inspect-modal-overlay effective-settings-overlay" onClick={onClose}>
      <div
        className="inspect-modal-content effective-settings"
        role="dialog"
        aria-modal="true"
        aria-label="Effective settings"
        onClick={e => e.stopPropagation()}
      >
        <div className="inspect-modal-header">
          <h4>Effective settings</h4>
          <WorkspaceActionButton
            ref={closeRef}
            appearance="quiet"
            size="toolbar"
            icon="x"
            iconOnly
            className="close-modal-btn"
            onClick={onClose}
            aria-label="Close"
            title="Close"
          />
        </div>

        <div className="inspect-modal-body effective-settings__body">
          <p className="effective-settings__model">
            <strong>{modelName}</strong>
            <span className="effective-settings__meta">Backend: {backendLabel}</span>
          </p>

          <section className="effective-settings__section">
            <h5 className="effective-settings__section-title">Settings by source</h5>
            <p className="effective-settings__note">
              <Icon name="info" size={12} />
              <span className="effective-settings__note-copy">These rows show known sources for individual settings. The <strong>Effective load command</strong> below is authoritative. It includes architecture and global defaults applied by the server that may not appear here.</span>
            </p>
            <div className="effective-settings__rows">
              <div className="effective-settings__row">
                <span className="effective-settings__row-label">Context size</span>
                <span className="effective-settings__row-value">{contextSetting.value}</span>
                <span className="effective-settings__source effective-settings__source--generic">{contextSetting.source}</span>
              </div>
              <div className="effective-settings__row">
                <span className="effective-settings__row-label">MCP servers</span>
                <span className="effective-settings__row-value">{!mcpEnabled ? 'Off' : (mcpServerIds.length > 0 ? mcpServerIds.join(', ') : 'Built-in Lemonade')}</span>
                <span
                  className="effective-settings__source effective-settings__source--generic effective-settings__source--chat-add"
                  title="Configured from the chat + menu"
                >
                  <Icon name="plus" size={11} />
                  <span>Menu</span>
                </span>
              </div>
              {resolved && (
                <div className="effective-settings__row">
                  <span className="effective-settings__row-label">Thinking</span>
                  <span className="effective-settings__row-value">{THINKING_MODE_LABELS[resolved.thinking_mode] || resolved.thinking_mode}</span>
                  <span className={`effective-settings__source ${sourceClass(resolved.sources.thinking_mode)}`}>{sourceLabel(resolved.sources.thinking_mode)}</span>
                </div>
              )}
              {sourceRows.map(row => (
                <div className="effective-settings__row" key={row.key}>
                  <span className="effective-settings__row-label">{row.label}</span>
                  <span className="effective-settings__row-value">{row.value}</span>
                  <span className={`effective-settings__source ${sourceClass(row.source)}`}>{sourceLabel(row.source)}</span>
                </div>
              ))}
              {sourceRows.length === 0 && !resolved && (
                <p className="effective-settings__empty">Model tuning details are unavailable for this model.</p>
              )}
            </div>
            {autoContextSizeEnabled && (
              <p className="effective-settings__note">
                <Icon name="info" size={12} /> Context size is auto-resolved from available memory. This is an estimate - the final value is computed at load time after any model eviction.
              </p>
            )}
          </section>

          <section className="effective-settings__section">
            <h5 className="effective-settings__section-title">Chat sampling</h5>
            <p className="effective-settings__note"><Icon name="info" size={12} /> Leave fields empty to use the backend defaults. Saved values are sent with future chat requests for this model.</p>
            <div className="effective-settings__sampling">
              {(Object.keys(SAMPLING_LABELS) as Array<keyof SamplingParams>).map(key => {
                const inputId = samplingInputId(key);
                const label = SAMPLING_LABELS[key] || key;
                return (
                  <div key={key} className="effective-settings__sampling-field">
                    <label htmlFor={inputId}>{label}</label>
                    <div className="detail-configuration__number-control">
                      <input
                        id={inputId}
                        className="input detail-configuration__number-input"
                        type="number"
                        step={key === 'top_k' ? 1 : 0.01}
                        min={0}
                        value={samplingDraft[key]}
                        placeholder="Backend default"
                        onChange={event => setSamplingDraft(current => ({ ...current, [key]: event.target.value }))}
                      />
                      <span className="detail-configuration__context-stepper">
                        <button type="button" onClick={() => stepSamplingInput(key, 1)} aria-label={`Increase ${label}`}>
                          <Icon name="chevron-up" size={11} aria-hidden="true" />
                        </button>
                        <button type="button" onClick={() => stepSamplingInput(key, -1)} aria-label={`Decrease ${label}`}>
                          <Icon name="chevron-down" size={11} aria-hidden="true" />
                        </button>
                      </span>
                    </div>
                  </div>
                );
              })}
            </div>
            <div className="effective-settings__actions">
              <WorkspaceActionButton appearance="secondary" size="small" onClick={saveSampling}>
                Save sampling
              </WorkspaceActionButton>
            </div>
          </section>

          <section className="effective-settings__section">
            <h5 className="effective-settings__section-title"><Icon name="terminal-square" size={14} /> Effective load command</h5>
            {loading && <p className="effective-settings__empty">Resolving…</p>}
            {error && <p className="effective-settings__error">{error}</p>}
            {!loading && !error && (
              <>
                <pre className="effective-settings__command"><code>{loadCommand}</code></pre>
                <p className="effective-settings__note">
                  <Icon name="info" size={12} /> Fixed launch flags (model path, port, chat template, metrics) are added by the server at load time and are not shown here.
                </p>
              </>
            )}
          </section>

          {canEditArgs && (
            <section className="effective-settings__section effective-settings__danger">
              <label className="effective-settings__ack">
                <input type="checkbox" checked={unlocked} onChange={e => setUnlocked(e.target.checked)} />
                <span><Icon name="alert" size={13} /> I know what I am doing. Let me edit the final loading arguments</span>
              </label>
              {unlocked && (
                <div className="effective-settings__editor">
                  <textarea
                    className="textarea effective-settings__textarea"
                    value={draft}
                    spellCheck={false}
                    placeholder="--threads 8 --flash-attn on"
                    onChange={e => setDraft(e.target.value)}
                    rows={3}
                  />
                  <p className="effective-settings__hint">
                    These raw backend arguments replace the resolved ones for the next load of this model. Session-only - nothing is written to disk, and it resets when you reload the app.
                  </p>
                  {previewError && <p className="effective-settings__error">{previewError}</p>}
                  <div className="effective-settings__actions">
                    <WorkspaceActionButton appearance="primary" size="small" onClick={applyOverride} disabled={busy}>
                      {busy ? 'Applying…' : (isModelLoaded ? 'Apply & reload' : 'Apply for next load')}
                    </WorkspaceActionButton>
                    {hasOverride && (
                      <WorkspaceActionButton appearance="secondary" size="small" icon="rotate-ccw" onClick={resetOverride} disabled={busy}>
                        Reset override
                      </WorkspaceActionButton>
                    )}
                  </div>
                </div>
              )}
              {!unlocked && hasOverride && (
                <div className="effective-settings__actions">
                  <span className="effective-settings__override-flag"><Icon name="alert" size={12} /> A session override is active.</span>
                  <WorkspaceActionButton appearance="secondary" size="small" icon="rotate-ccw" onClick={resetOverride} disabled={busy}>
                    Reset override
                  </WorkspaceActionButton>
                </div>
              )}
            </section>
          )}
          {notice && <p className="effective-settings__notice" role="status">{notice}</p>}
        </div>
      </div>
    </div>
  );

  return createPortal(body, document.body);
};

export default EffectiveSettingsModal;
