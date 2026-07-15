import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import api from '../../api';
import { useFocusTrap } from '../../hooks/useFocusTrap';
import { autoOptStore, AutoOptState } from './autoOptStore';
import { createPresetFromRun, applyRunNow } from './presetFromRun';
import {
  AutoOptRecommendation,
  AutoOptRunRecord,
  AutoOptStage,
  BenchPoint,
  isAutoOptRunActive,
} from './autoOptTypes';

function formatDuration(ms: number | undefined): string {
  if (ms === undefined || !Number.isFinite(ms)) return '';
  if (ms < 1000) return `${Math.round(ms)} ms`;
  if (ms < 60000) return `${(ms / 1000).toFixed(1)} s`;
  return `${Math.floor(ms / 60000)}m ${Math.round((ms % 60000) / 1000)}s`;
}

function formatNumber(value: unknown, digits = 1): string {
  const n = Number(value);
  return Number.isFinite(n) ? n.toFixed(digits) : '—';
}

function stageGlyph(stage: AutoOptStage): string {
  switch (stage.status) {
    case 'completed': return '✓';
    case 'failed': return '✕';
    case 'running': return '…';
    case 'skipped': return '·';
    default: return '○';
  }
}

function expectedValue(rec: AutoOptRecommendation, field: 'ttft_ms' | 'tps' | 'vram_gb'): string {
  const value = rec.expected?.[field];
  if (value === undefined) return '—';
  return field === 'vram_gb' ? formatNumber(value, 2) : formatNumber(value);
}

function benchLadderRows(run: AutoOptRunRecord | undefined): BenchPoint[] {
  const bench = run?.measurements?.bench || [];
  return bench.filter(row => row.params?.ladder === true);
}

function benchDuelRows(run: AutoOptRunRecord | undefined): BenchPoint[] {
  const bench = run?.measurements?.bench || [];
  return bench.filter(row => row.params?.ladder !== true && row.params?.spec_n === undefined);
}

const CopyArgsButton: React.FC<{ text: string }> = ({ text }) => {
  const [copied, setCopied] = useState(false);
  const handleClick = async () => {
    try {
      await navigator.clipboard.writeText(text);
      setCopied(true);
      window.setTimeout(() => setCopied(false), 1200);
    } catch {
      setCopied(false);
    }
  };
  return (
    <button
      type="button"
      className={`copy-inline${copied ? ' copy-inline--copied' : ''}`}
      onClick={() => void handleClick()}
      title={copied ? 'Copied' : 'Copy arguments'}
      aria-label={copied ? 'Copied' : 'Copy arguments'}
    >
      {copied ? '✓' : '⧉'}
    </button>
  );
};

const AutoOptRunDetail: React.FC<{
  runId: string | null;
  onClose: () => void;
}> = ({ runId, onClose }) => {
  const [storeState, setStoreState] = useState<AutoOptState>(() => autoOptStore.snapshot());
  const [selectedRecIndex, setSelectedRecIndex] = useState(0);
  const [notice, setNotice] = useState<string | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const slideoverRef = useRef<HTMLElement>(null);
  const open = !!runId;

  useFocusTrap(slideoverRef, open);

  useEffect(() => autoOptStore.subscribe(setStoreState), []);

  useEffect(() => {
    if (!open) return;
    setSelectedRecIndex(0);
    setNotice(null);
    setActionError(null);
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') onClose();
    };
    document.addEventListener('keydown', onKey);
    return () => document.removeEventListener('keydown', onKey);
  }, [open, runId, onClose]);

  const run = runId ? storeState.runs.find(candidate => candidate.id === runId) : undefined;
  const result = run?.result;

  const recommendations = useMemo<AutoOptRecommendation[]>(() => {
    if (!result) return [];
    return [result.primary, ...(result.alternatives || [])];
  }, [result]);
  const selectedRec = recommendations[selectedRecIndex] || recommendations[0];
  const ladder = benchLadderRows(run);
  const duel = benchDuelRows(run);

  const modelInfo = useMemo(() => {
    if (!run) return null;
    const target = run.model.trim().toLowerCase();
    return api.allModels.find(model => String((model as Record<string, unknown>).model_name || model.name || model.id || '').trim().toLowerCase() === target) || null;
  }, [run]);

  const runAction = useCallback(async (action: 'create' | 'create-apply' | 'try') => {
    if (!run || !selectedRec) return;
    setActionError(null);
    setNotice(null);
    try {
      if (action === 'create') {
        const preset = createPresetFromRun(run, selectedRec, modelInfo);
        setNotice(`Created preset "${preset.name}" and selected it for future loads of ${run.model}.`);
      } else if (action === 'create-apply') {
        const preset = createPresetFromRun(run, selectedRec, modelInfo);
        try {
          await applyRunNow(run, selectedRec, { save: true });
        } catch (err) {
          setActionError(`Preset "${preset.name}" was created and selected, but loading ${run.model} failed: ${err instanceof Error ? err.message : String(err)}`);
          return;
        }
        setNotice(`Created preset "${preset.name}" and applied it to ${run.model}.`);
      } else {
        await applyRunNow(run, selectedRec, { save: false });
        setNotice(`Loaded ${run.model} with the recommended settings (nothing saved).`);
      }
    } catch (err) {
      setActionError(err instanceof Error ? err.message : 'Action failed.');
    }
  }, [run, selectedRec, modelInfo]);

  if (!open) return null;

  return (
    <>
      <div className="scrim scrim--autoopt-detail is-open" onClick={onClose} />
      <aside
        ref={slideoverRef}
        className="slideover slideover--wide slideover--autoopt-detail is-open"
        role="dialog"
        aria-modal="true"
        aria-label="AutoOpt run details"
        data-autoopt-detail
      >
        {run && (
          <>
            <div className="slideover__head">
              <div className="slideover__top">
                <div className="slideover__title-wrap">
                  <h2 className="slideover__title">AutoOpt · {run.model}</h2>
                </div>
                <button className="slideover__close" onClick={onClose} aria-label="Close">✕</button>
              </div>
              <p className="slideover__desc">
                <span className={`autoopt-status-chip autoopt-status-chip--${run.status}`} data-autoopt-detail-status>{run.status}</span>
                {run.lemonade_version ? ` · Lemonade ${run.lemonade_version}` : ''}
                {run.summary ? ` · ${run.summary}` : ''}
              </p>
            </div>

            <div className="slideover__body autoopt-detail__body">
              {run.status === 'failed' && run.error && (
                <p className="preset-error autoopt-detail__error" role="alert" data-autoopt-detail-error>⚠ {run.error}</p>
              )}

              {run.stages.length > 0 && (
                <div className="slideover__section">
                  <h3>Stages</h3>
                  <ul className="autoopt-stage-list" data-autoopt-detail-stages>
                    {run.stages.map(stage => (
                      <li key={stage.name} className={`autoopt-stage autoopt-stage--${stage.status}`}>
                        <span className="autoopt-stage__marker" aria-hidden="true">{stageGlyph(stage)}</span>
                        <span className="autoopt-stage__name">{stage.name}</span>
                        {stage.duration_ms !== undefined && <span className="autoopt-stage__duration">{formatDuration(stage.duration_ms)}</span>}
                        <span className="autoopt-stage__status">{stage.status}</span>
                        {stage.error && <span className="autoopt-stage__error">⚠ {stage.error}</span>}
                        {typeof stage.data?.note === 'string' && <span className="autoopt-stage__note">{stage.data.note}</span>}
                      </li>
                    ))}
                  </ul>
                </div>
              )}

              {run.measurements && (run.measurements.fit.length > 0 || run.measurements.bench.length > 0) && (
                <div className="slideover__section">
                  <h3>Measurements</h3>
                  <p className="preset-help">
                    {run.measurements.fit.length} heuristic fit estimates · {run.measurements.bench.length} measured configurations
                  </p>
                  {duel.length > 0 && (
                    <div className="autoopt-alt-table-wrap">
                      <table className="autoopt-alt-table" data-autoopt-bench-duel>
                        <thead>
                          <tr><th>Backend</th><th>Depth</th><th>TTFT ms</th><th>tok/s</th><th>VRAM GB</th><th>Status</th></tr>
                        </thead>
                        <tbody>
                          {duel.map((row, index) => (
                            <tr key={index}>
                              <td>{row.backend}</td>
                              <td>{row.params?.d ?? '—'}</td>
                              <td>{formatNumber(row.ttft_ms)}</td>
                              <td>{formatNumber(row.tps)}</td>
                              <td>{row.vram_gb > 0 ? formatNumber(row.vram_gb, 2) : '—'}</td>
                              <td>{row.ok === false ? (row.error || 'failed') : 'ok'}</td>
                            </tr>
                          ))}
                        </tbody>
                      </table>
                    </div>
                  )}
                  {ladder.length > 0 && (
                    <div className="autoopt-alt-table-wrap">
                      <table className="autoopt-alt-table" data-autoopt-bench-ladder>
                        <thead>
                          <tr><th>Backend</th><th>batch</th><th>ubatch</th><th>TTFT ms</th></tr>
                        </thead>
                        <tbody>
                          {ladder.map((row, index) => (
                            <tr key={index}>
                              <td>{row.backend}</td>
                              <td>{row.params?.b ?? '—'}</td>
                              <td>{row.params?.ub ?? '—'}</td>
                              <td>{formatNumber(row.ttft_ms)}</td>
                            </tr>
                          ))}
                        </tbody>
                      </table>
                    </div>
                  )}
                </div>
              )}

              {selectedRec && (
                <div className="slideover__section">
                  <h3>Recommendation</h3>
                  <div className="autoopt-rec-card" data-autoopt-recommendation>
                    <div className="autoopt-rec-card__head">
                      <strong>{selectedRec.label}</strong>
                      <div className="autoopt-rec-card__chips">
                        {selectedRec.llamacpp_backend && <span className="autoopt-chip">{selectedRec.llamacpp_backend}</span>}
                        {selectedRec.ctx_size > 0 && <span className="autoopt-chip">ctx {selectedRec.ctx_size.toLocaleString()}</span>}
                        {result?.sampling_defaults?.temperature !== undefined && <span className="autoopt-chip">temp {result.sampling_defaults.temperature}</span>}
                        {result?.sampling_defaults?.min_p !== undefined && <span className="autoopt-chip">min_p {result.sampling_defaults.min_p}</span>}
                      </div>
                    </div>
                    <div className="autoopt-rec-card__args">
                      <code data-autoopt-rec-args>{selectedRec.llamacpp_args}</code>
                      <CopyArgsButton text={selectedRec.llamacpp_args} />
                    </div>
                    {selectedRec.rationale.length > 0 && (
                      <ul className="autoopt-rec-card__rationale">
                        {selectedRec.rationale.map(line => <li key={line}>{line}</li>)}
                      </ul>
                    )}
                    {selectedRec.tradeoff && <p className="preset-help">{selectedRec.tradeoff}</p>}
                  </div>
                </div>
              )}

              {recommendations.length > 1 && (
                <div className="slideover__section">
                  <h3>Alternatives</h3>
                  <div className="autoopt-alt-table-wrap">
                    <table className="autoopt-alt-table" data-autoopt-alternatives>
                      <thead>
                        <tr>
                          <th>Option</th>
                          <th>Backend</th>
                          <th>Context</th>
                          <th>TTFT ms</th>
                          <th>tok/s</th>
                          <th>VRAM GB</th>
                          <th />
                        </tr>
                      </thead>
                      <tbody>
                        {recommendations.map((rec, index) => (
                          <tr key={rec.label} className={index === selectedRecIndex ? 'is-selected' : ''}>
                            <td>{rec.label}</td>
                            <td>{rec.llamacpp_backend || '—'}</td>
                            <td>{rec.ctx_size !== undefined ? rec.ctx_size.toLocaleString() : '—'}</td>
                            <td>{expectedValue(rec, 'ttft_ms')}</td>
                            <td>{expectedValue(rec, 'tps')}</td>
                            <td>{expectedValue(rec, 'vram_gb')}</td>
                            <td>
                              {index === selectedRecIndex
                                ? <span className="autoopt-chip">Selected</span>
                                : (
                                  <button
                                    type="button"
                                    className="btn btn--ghost btn--tiny"
                                    onClick={() => setSelectedRecIndex(index)}
                                    data-autoopt-use-alternative={rec.label}
                                  >
                                    Use this instead
                                  </button>
                                )}
                            </td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  </div>
                </div>
              )}

              <p className="autoopt-detail__notice" role="status" aria-live="polite" aria-atomic="true" data-autoopt-detail-notice>
                {notice ? `✓ ${notice}` : ''}
              </p>
              {actionError && <p className="preset-error" role="alert">⚠ {actionError}</p>}
            </div>

            {run.status === 'completed' && selectedRec && (
              <p className="preset-help autoopt-detail__cta-help" data-autoopt-cta-help>
                &ldquo;Create preset&rdquo; creates the preset and selects it for this model&rsquo;s future loads.
                &ldquo;Create preset &amp; apply now&rdquo; additionally loads the model with it right away.
                &ldquo;Try now without saving&rdquo; loads once and saves nothing.
              </p>
            )}
            <div className="slideover__foot">
              {run.status === 'completed' && selectedRec ? (
                <>
                  <button className="btn btn--ghost" onClick={() => void runAction('try')} title="Loads once with the recommended settings and saves nothing" data-autoopt-try-now>Try now without saving</button>
                  <button className="btn btn--ghost" onClick={() => void runAction('create')} title="Creates the preset and selects it for this model's future loads" data-autoopt-create-preset>Create preset</button>
                  <button className="btn btn--primary" onClick={() => void runAction('create-apply')} title="Creates the preset, selects it for this model's future loads, and loads the model with it now" data-autoopt-create-apply>Create preset &amp; apply now</button>
                </>
              ) : (
                <>
                  {isAutoOptRunActive(run) && (
                    <button className="btn btn--ghost" onClick={() => autoOptStore.cancelRun(run.id)}>Cancel run</button>
                  )}
                  <button className="btn btn--primary" onClick={onClose}>Close</button>
                </>
              )}
            </div>
          </>
        )}
      </aside>
    </>
  );
};

export default AutoOptRunDetail;
