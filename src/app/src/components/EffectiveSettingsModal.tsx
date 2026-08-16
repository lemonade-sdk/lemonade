import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import api, { type ModelInfo, type EffectiveLoadCommand, type LoadedModel, friendlyErrorMessage } from '../api';
import {
  type RecipeOptions,
  type SamplingParams,
  type TuningValueSource,
  DEFAULT_CONTEXT_SIZE,
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
import { useI18n } from '../i18n';

const RECIPE_OPTION_LABEL_KEYS: Partial<Record<keyof RecipeOptions, string>> = {
  ctx_size: 'effective.contextSize',
  llamacpp_backend: 'effective.fields.backend',
  llamacpp_device: 'effective.fields.device',
  llamacpp_args: 'effective.fields.backendArgs',
  vllm_backend: 'effective.fields.backend',
  vllm_args: 'effective.fields.backendArgs',
  flm_args: 'effective.fields.backendArgs',
  whispercpp_backend: 'effective.fields.backend',
  whispercpp_args: 'effective.fields.backendArgs',
  moonshine_backend: 'effective.fields.backend',
  moonshine_args: 'effective.fields.backendArgs',
  sdcpp_args: 'effective.fields.backendArgs',
  'sd-cpp_backend': 'effective.fields.backend',
  steps: 'effective.fields.steps',
  cfg_scale: 'effective.fields.cfgScale',
  width: 'effective.fields.width',
  height: 'effective.fields.height',
  sampling_method: 'effective.fields.samplingMethod',
  flow_shift: 'effective.fields.flowShift',
  voice: 'effective.fields.voice',
  speed: 'effective.fields.speed',
};

const SAMPLING_LABEL_KEYS: Partial<Record<keyof SamplingParams, string>> = {
  temperature: 'effective.sampling.temperature',
  top_p: 'effective.sampling.topP',
  top_k: 'effective.sampling.topK',
  min_p: 'effective.sampling.minP',
  repeat_penalty: 'effective.sampling.repeatPenalty',
};

function sourceLabel(source: TuningValueSource | undefined, t: (key: string) => string): string {
  switch (source) {
    case 'custom': return t('effective.source.custom');
    case 'built_in': return t('effective.source.builtIn');
    case 'optimized': return t('effective.source.optimized');
    default: return t('effective.source.default');
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

function displayValue(value: unknown, t: (key: string) => string): string {
  if (value === null || value === undefined || value === '') return '—';
  if (typeof value === 'boolean') return value ? t('effective.on') : t('effective.off');
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

function shellQuote(token: string): string {
  if (token === '') return "''";
  if (/^[A-Za-z0-9_\-./:=@]+$/.test(token)) return token;
  return `'${token.replace(/'/g, `'\\''`)}'`;
}

function formatCommand(modelName: string, args: string[]): string {
  const parts = ['lemonade', 'load', shellQuote(modelName), ...args.map(shellQuote)];
  return parts.join(' ');
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

type EffectiveNotice = { key: string } | { text: string };

const EffectiveSettingsModal: React.FC<EffectiveSettingsModalProps> = ({
  open, onClose, modelName, modelInfo, recipe, mcpEnabled, mcpServerIds, fallbackCtxSize, loadedModel, isModelLoaded, onReload, onLoad,
}) => {
  const { t, formatNumber } = useI18n('models');
  const argsField = backendArgsFieldForRecipe(recipe);
  const canEditArgs = backendSupportsArgs(recipe) && !!argsField;

  const [effective, setEffective] = useState<EffectiveLoadCommand | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const [unlocked, setUnlocked] = useState(false);
  const [draft, setDraft] = useState('');
  const [preview, setPreview] = useState<EffectiveLoadCommand | null>(null);
  const [previewError, setPreviewError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [notice, setNotice] = useState<EffectiveNotice | null>(null);
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
      const result = await api.effectiveLoadCommand(modelName, undefined, modelInfo);
      setEffective(result);
      const committed = argsField ? result.options[argsField] : undefined;
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
        const result = await api.effectiveLoadCommand(modelName, { [argsField]: draft, merge_args: false }, modelInfo);
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
        setNotice({ key: 'effective.appliedReloaded' });
      } else {
        setNotice({ key: 'effective.savedSession' });
      }
      setUnlocked(false);
    } catch (err) {
      setNotice({ text: friendlyErrorMessage(err) });
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
        setNotice({ key: 'effective.clearedReloaded' });
      } else {
        setNotice({ key: 'effective.cleared' });
      }
      await loadEffective();
      setUnlocked(false);
    } catch (err) {
      setNotice({ text: friendlyErrorMessage(err) });
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
        return { value: t('effective.autoValue', { value: formatNumber(runtimeContext) }), source: t('effective.runtime') };
      }
      if (loadedModel && resolvingContextSize) return { value: t('effective.resolving'), source: t('effective.runtime') };
      return { value: t('effective.auto'), source: t('effective.configuration') };
    }

    if (runtimeContext !== null) {
      return { value: formatNumber(runtimeContext), source: t('effective.runtime') };
    }

    const effectiveContext = positiveContextSize(effective?.options?.ctx_size);
    if (effectiveContext !== null) {
      return { value: formatNumber(effectiveContext), source: t('effective.effectiveLoad') };
    }

    const resolvedContext = positiveContextSize(resolvedContextRaw);
    if (resolvedContext !== null) {
      return {
        value: formatNumber(resolvedContext),
        source: sourceLabel(resolved?.sources.recipe_options.ctx_size, t),
      };
    }

    if (loadedModel && resolvingContextSize) return { value: t('effective.resolving'), source: t('effective.runtime') };
    return { value: t('effective.unavailable'), source: t('effective.configuration') };
  }, [autoContextSizeEnabled, effective, loadedContextSize, loadedModel, resolved, resolvedContextRaw, resolvingContextSize, t, formatNumber]);

  const sourceRows = useMemo<SourceRow[]>(() => {
    if (!resolved) return [];
    const rows: SourceRow[] = [];
    for (const [key, value] of Object.entries(resolved.tuning.recipe_options || {})) {
      if (key === 'merge_args' || key === 'mmproj_enabled' || key === 'ctx_size') continue;
      rows.push({
        key: `ro-${key}`,
        label: RECIPE_OPTION_LABEL_KEYS[key as keyof RecipeOptions] ? t(RECIPE_OPTION_LABEL_KEYS[key as keyof RecipeOptions]!) : key,
        value: displayValue(value, t),
        source: resolved.sources.recipe_options[key as keyof RecipeOptions],
      });
    }
    return rows;
  }, [resolved, t]);

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
    setNotice({ key: Object.keys(sampling).length > 0
      ? 'effective.samplingSaved'
      : 'effective.samplingDefault' });
  };

  if (!open) return null;

  const previewArgs = preview?.args ?? effective?.args ?? [];
  const backendLabel = effective?.backend || '—';

  const body = (
    <div className="inspect-modal-overlay effective-settings-overlay" onClick={onClose}>
      <div
        className="inspect-modal-content effective-settings"
        role="dialog"
        aria-modal="true"
        aria-label={t('effective.title')}
        onClick={e => e.stopPropagation()}
      >
        <div className="inspect-modal-header">
          <h4>{t('effective.title')}</h4>
          <WorkspaceActionButton
            ref={closeRef}
            appearance="quiet"
            size="toolbar"
            icon="x"
            iconOnly
            className="close-modal-btn"
            onClick={onClose}
            aria-label={t('effective.close')}
            title={t('effective.close')}
          />
        </div>

        <div className="inspect-modal-body effective-settings__body">
          <p className="effective-settings__model">
            <strong>{modelName}</strong>
            <span className="effective-settings__meta">{t('effective.backend', { backend: backendLabel })}</span>
          </p>

          <section className="effective-settings__section">
            <h5 className="effective-settings__section-title">{t('effective.settingsBySource')}</h5>
            <p className="effective-settings__note">
              <Icon name="info" size={12} />
              <span className="effective-settings__note-copy">{t('effective.sourceNotePrefix')} <strong>{t('effective.sourceNoteCommand')}</strong> {t('effective.sourceNoteSuffix')}</span>
            </p>
            <div className="effective-settings__rows">
              <div className="effective-settings__row">
                <span className="effective-settings__row-label">{t('effective.contextSize')}</span>
                <span className="effective-settings__row-value">{contextSetting.value}</span>
                <span className="effective-settings__source effective-settings__source--generic">{contextSetting.source}</span>
              </div>
              <div className="effective-settings__row">
                <span className="effective-settings__row-label">{t('effective.mcpServers')}</span>
                <span className="effective-settings__row-value">{!mcpEnabled ? t('effective.off') : (mcpServerIds.length > 0 ? mcpServerIds.join(', ') : t('effective.builtIn'))}</span>
                <span
                  className="effective-settings__source effective-settings__source--generic effective-settings__source--chat-add"
                  title={t('effective.configuredChatMenu')}
                >
                  <Icon name="plus" size={11} />
                  <span>{t('effective.menu')}</span>
                </span>
              </div>
              {resolved && (
                <div className="effective-settings__row">
                  <span className="effective-settings__row-label">{t('effective.thinking')}</span>
                  <span className="effective-settings__row-value">{t(`effective.thinkingModes.${resolved.thinking_mode === 'smart_extra' ? 'smartExtra' : resolved.thinking_mode}`)}</span>
                  <span className={`effective-settings__source ${sourceClass(resolved.sources.thinking_mode)}`}>{sourceLabel(resolved.sources.thinking_mode, t)}</span>
                </div>
              )}
              {sourceRows.map(row => (
                <div className="effective-settings__row" key={row.key}>
                  <span className="effective-settings__row-label">{row.label}</span>
                  <span className="effective-settings__row-value">{row.value}</span>
                  <span className={`effective-settings__source ${sourceClass(row.source)}`}>{sourceLabel(row.source, t)}</span>
                </div>
              ))}
              {sourceRows.length === 0 && !resolved && (
                <p className="effective-settings__empty">{t('effective.detailsUnavailable')}</p>
              )}
            </div>
            {autoContextSizeEnabled && (
              <p className="effective-settings__note">
                <Icon name="info" size={12} /> {t('effective.autoContextHelp')}
              </p>
            )}
          </section>

          <section className="effective-settings__section">
            <h5 className="effective-settings__section-title">{t('effective.chatSampling')}</h5>
            <p className="effective-settings__note"><Icon name="info" size={12} /> {t('effective.samplingHelp')}</p>
            <div className="effective-settings__sampling">
              {(Object.keys(SAMPLING_LABEL_KEYS) as Array<keyof SamplingParams>).map(key => {
                const inputId = samplingInputId(key);
                const labelKey = SAMPLING_LABEL_KEYS[key];
                const label = labelKey ? t(labelKey) : key;
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
                        placeholder={t('effective.backendDefault')}
                        onChange={event => setSamplingDraft(current => ({ ...current, [key]: event.target.value }))}
                      />
                      <span className="detail-configuration__context-stepper">
                        <button type="button" onClick={() => stepSamplingInput(key, 1)} aria-label={t('effective.sampling.increase', { label })}>
                          <Icon name="chevron-up" size={11} aria-hidden="true" />
                        </button>
                        <button type="button" onClick={() => stepSamplingInput(key, -1)} aria-label={t('effective.sampling.decrease', { label })}>
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
                {t('effective.saveSampling')}
              </WorkspaceActionButton>
            </div>
          </section>

          <section className="effective-settings__section">
            <h5 className="effective-settings__section-title"><Icon name="terminal-square" size={14} /> {t('effective.loadCommand')}</h5>
            {loading && <p className="effective-settings__empty">{t('effective.resolving')}</p>}
            {error && <p className="effective-settings__error">{error}</p>}
            {!loading && !error && (
              <>
                <pre className="effective-settings__command"><code>{formatCommand(modelName, previewArgs)}</code></pre>
                <p className="effective-settings__note">
                  <Icon name="info" size={12} /> {t('effective.loadCommandHelp')}
                </p>
              </>
            )}
          </section>

          {canEditArgs && (
            <section className="effective-settings__section effective-settings__danger">
              <label className="effective-settings__ack">
                <input type="checkbox" checked={unlocked} onChange={e => setUnlocked(e.target.checked)} />
                <span><Icon name="alert" size={13} /> {t('effective.unlock')}</span>
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
                    {t('effective.rawArgsHelp')}
                  </p>
                  {previewError && <p className="effective-settings__error">{previewError}</p>}
                  <div className="effective-settings__actions">
                    <WorkspaceActionButton appearance="primary" size="small" onClick={applyOverride} disabled={busy}>
                      {busy ? t('effective.applying') : (isModelLoaded ? t('effective.applyReload') : t('effective.applyNext'))}
                    </WorkspaceActionButton>
                    {hasOverride && (
                      <WorkspaceActionButton appearance="secondary" size="small" icon="rotate-ccw" onClick={resetOverride} disabled={busy}>
                        {t('effective.resetOverride')}
                      </WorkspaceActionButton>
                    )}
                  </div>
                </div>
              )}
              {!unlocked && hasOverride && (
                <div className="effective-settings__actions">
                  <span className="effective-settings__override-flag"><Icon name="alert" size={12} /> {t('effective.overrideActive')}</span>
                  <WorkspaceActionButton appearance="secondary" size="small" icon="rotate-ccw" onClick={resetOverride} disabled={busy}>
                    {t('effective.resetOverride')}
                  </WorkspaceActionButton>
                </div>
              )}
            </section>
          )}
          {notice && <p className="effective-settings__notice" role="status">{'key' in notice ? t(notice.key) : notice.text}</p>}
        </div>
      </div>
    </div>
  );

  return createPortal(body, document.body);
};

export default EffectiveSettingsModal;
