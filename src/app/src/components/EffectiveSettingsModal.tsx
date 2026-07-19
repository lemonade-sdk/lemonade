import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import api, { type ModelInfo, type EffectiveLoadCommand, friendlyErrorMessage } from '../api';
import {
  type Preset,
  type RecipeOptions,
  type SamplingParams,
  type TuningValueSource,
  DEFAULT_CONTEXT_SIZE,
  THINKING_MODE_LABELS,
  backendArgsFieldForRecipe,
  backendSupportsArgs,
  clearSessionArgsOverride,
  getSessionArgsOverride,
  presetMcpDisplayText,
  resolvedModelTuningForPreset,
  setSessionArgsOverride,
  systemPromptNameForPreset,
} from '../presetStore';
import { Icon } from './Icon';

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
  mmproj_enabled: 'Multimodal projector',
  merge_args: 'Merge args',
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
    case 'custom': return 'Model tuning';
    case 'built-in': return 'Built-in tuning';
    case 'optimized': return 'AutoOpt optimized';
    default: return 'Default';
  }
}

function sourceClass(source: TuningValueSource | undefined): string {
  switch (source) {
    case 'custom': return 'effective-settings__source--custom';
    case 'built-in': return 'effective-settings__source--builtin';
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

function shellQuote(token: string): string {
  if (token === '') return "''";
  if (/^[A-Za-z0-9_\-./:=@]+$/.test(token)) return token;
  return `'${token.replace(/'/g, `'\\''`)}'`;
}

function formatCommand(modelName: string, args: string[]): string {
  const parts = ['lemonade-server', 'load', shellQuote(modelName), ...args.map(shellQuote)];
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
  preset: Preset;
  recipe: string;
  fallbackCtxSize?: number;
  isModelLoaded: boolean;
  onReload: () => Promise<void>;
}

const EffectiveSettingsModal: React.FC<EffectiveSettingsModalProps> = ({
  open, onClose, modelName, modelInfo, preset, recipe, fallbackCtxSize, isModelLoaded, onReload,
}) => {
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
  const [notice, setNotice] = useState<string | null>(null);

  const hasOverride = !!getSessionArgsOverride(modelName);

  const resolved = useMemo(() => {
    if (!modelInfo || !open) return null;
    try {
      return resolvedModelTuningForPreset(modelName, modelInfo, preset, fallbackCtxSize ?? DEFAULT_CONTEXT_SIZE);
    } catch {
      return null;
    }
  }, [modelName, modelInfo, preset, fallbackCtxSize, open]);

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
  }, [open, loadEffective]);

  useEffect(() => {
    if (!open || !unlocked || !argsField) { setPreview(null); return; }
    let cancelled = false;
    const handle = setTimeout(async () => {
      setPreviewError(null);
      try {
        const result = await api.effectiveLoadCommand(modelName, { [argsField]: draft }, modelInfo);
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
    try {
      setSessionArgsOverride(modelName, recipe, draft.trim());
      if (isModelLoaded) {
        await onReload();
        setNotice('Applied and reloaded with the new arguments.');
      } else {
        setNotice('Saved for this session. It will take effect the next time this model loads.');
      }
      await loadEffective();
      setUnlocked(false);
    } catch (err) {
      setNotice(friendlyErrorMessage(err));
    } finally {
      setBusy(false);
    }
  }, [argsField, modelName, recipe, draft, isModelLoaded, onReload, loadEffective]);

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

  const sourceRows = useMemo<SourceRow[]>(() => {
    if (!resolved) return [];
    const rows: SourceRow[] = [];
    for (const [key, value] of Object.entries(resolved.tuning.recipe_options || {})) {
      rows.push({
        key: `ro-${key}`,
        label: RECIPE_OPTION_LABELS[key as keyof RecipeOptions] || key,
        value: displayValue(value),
        source: resolved.sources.recipe_options[key as keyof RecipeOptions],
      });
    }
    for (const [key, value] of Object.entries(resolved.tuning.sampling || {})) {
      rows.push({
        key: `sp-${key}`,
        label: SAMPLING_LABELS[key as keyof SamplingParams] || key,
        value: displayValue(value),
        source: resolved.sources.sampling[key as keyof SamplingParams],
      });
    }
    return rows;
  }, [resolved]);

  if (!open) return null;

  const previewArgs = preview?.args ?? effective?.args ?? [];
  const backendLabel = effective?.backend || '—';

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
          <h4><Icon name="sliders-horizontal" size={15} /> Effective settings</h4>
          <button ref={closeRef} className="close-modal-btn" onClick={onClose} aria-label="Close">
            <Icon name="x" size={16} />
          </button>
        </div>

        <div className="inspect-modal-body effective-settings__body">
          <p className="effective-settings__model">
            <strong>{modelName}</strong>
            <span className="effective-settings__meta">Preset: {preset.name} · Backend: {backendLabel}</span>
          </p>

          <section className="effective-settings__section">
            <h5 className="effective-settings__section-title">Settings by source</h5>
            <div className="effective-settings__rows">
              <div className="effective-settings__row">
                <span className="effective-settings__row-label">Preset</span>
                <span className="effective-settings__row-value">{preset.name}</span>
                <span className="effective-settings__source effective-settings__source--preset">Preset</span>
              </div>
              <div className="effective-settings__row">
                <span className="effective-settings__row-label">System prompt</span>
                <span className="effective-settings__row-value">{systemPromptNameForPreset(preset)}</span>
                <span className="effective-settings__source effective-settings__source--preset">Preset</span>
              </div>
              <div className="effective-settings__row">
                <span className="effective-settings__row-label">MCP servers</span>
                <span className="effective-settings__row-value">{presetMcpDisplayText(preset)}</span>
                <span className="effective-settings__source effective-settings__source--preset">Preset</span>
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
          </section>

          <section className="effective-settings__section">
            <h5 className="effective-settings__section-title"><Icon name="terminal-square" size={14} /> Effective load command</h5>
            {loading && <p className="effective-settings__empty">Resolving…</p>}
            {error && <p className="effective-settings__error">{error}</p>}
            {!loading && !error && (
              <>
                <pre className="effective-settings__command"><code>{formatCommand(modelName, previewArgs)}</code></pre>
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
                <span><Icon name="alert" size={13} /> I know what I am doing — let me edit the final loading arguments</span>
              </label>
              {unlocked && (
                <div className="effective-settings__editor">
                  <textarea
                    className="effective-settings__textarea"
                    value={draft}
                    spellCheck={false}
                    placeholder="--threads 8 --flash-attn on"
                    onChange={e => setDraft(e.target.value)}
                    rows={3}
                  />
                  <p className="effective-settings__hint">
                    These raw backend arguments replace the resolved ones for the next load of this model. Session-only — nothing is written to disk, and it resets when you reload the app.
                  </p>
                  {previewError && <p className="effective-settings__error">{previewError}</p>}
                  <div className="effective-settings__actions">
                    <button className="btn btn--primary" onClick={applyOverride} disabled={busy}>
                      {busy ? 'Applying…' : (isModelLoaded ? 'Apply & reload' : 'Apply for next load')}
                    </button>
                    {hasOverride && (
                      <button className="btn" onClick={resetOverride} disabled={busy}>
                        <Icon name="rotate-ccw" size={13} /> Reset override
                      </button>
                    )}
                  </div>
                </div>
              )}
              {!unlocked && hasOverride && (
                <div className="effective-settings__actions">
                  <span className="effective-settings__override-flag"><Icon name="alert" size={12} /> A session override is active.</span>
                  <button className="btn" onClick={resetOverride} disabled={busy}>
                    <Icon name="rotate-ccw" size={13} /> Reset override
                  </button>
                </div>
              )}
              {notice && <p className="effective-settings__notice">{notice}</p>}
            </section>
          )}
        </div>
      </div>
    </div>
  );

  return createPortal(body, document.body);
};

export default EffectiveSettingsModal;
