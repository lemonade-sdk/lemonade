import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import api from '../../api';
import { useFocusTrap } from '../../hooks/useFocusTrap';
import { autoOptStore, AutoOptState } from './autoOptStore';
import { createPresetFromRun, applyRunNow } from './presetFromRun';
import {
  AutoOptRecommendation,
  AutoOptRunDetail as AutoOptRunDetailData,
  AutoOptStage,
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

function expectedDepthKeys(recs: AutoOptRecommendation[], field: 'pp_ts' | 'tg_ts'): string[] {
  const keys = new Set<string>();
  for (const rec of recs) {
    const value = rec.expected?.[field];
    if (value && typeof value === 'object') Object.keys(value).forEach(key => keys.add(key));
  }
  return [...keys].sort();
}

function expectedValue(rec: AutoOptRecommendation, field: 'pp_ts' | 'tg_ts', depthKey?: string): string {
  const value = rec.expected?.[field];
  if (value === undefined) return '—';
  if (typeof value === 'number') return depthKey && depthKey !== 'd0' ? '—' : formatNumber(value);
  return formatNumber(depthKey ? value[depthKey] : Object.values(value)[0]);
}

function benchLadderRows(detail: AutoOptRunDetailData | undefined): Array<Record<string, unknown>> {
  const bench = detail?.measurements?.bench || [];
  return bench.filter(row => row && typeof row === 'object' && ('b' in row || 'ub' in row));
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

  const summary = runId ? storeState.runs.find(run => run.id === runId) : undefined;
  const detail = runId ? storeState.details[runId] : undefined;
  const run = detail || (summary ? { ...summary, stages: [] as AutoOptStage[] } : undefined);
  const result = detail?.result;

  const recommendations = useMemo<AutoOptRecommendation[]>(() => {
    if (!result) return [];
    return [result.primary, ...(result.alternatives || [])];
  }, [result]);
  const selectedRec = recommendations[selectedRecIndex] || recommendations[0];
  const ppDepthKeys = expectedDepthKeys(recommendations, 'pp_ts');
  const ladder = benchLadderRows(detail);

  const modelInfo = useMemo(() => {
    if (!run) return null;
    const target = run.model.trim().toLowerCase();
    return api.allModels.find(model => String((model as Record<string, unknown>).model_name || model.name || model.id || '').trim().toLowerCase() === target) || null;
  }, [run]);

  const runAction = useCallback(async (action: 'create' | 'create-apply' | 'try') => {
    if (!detail || !selectedRec) return;
    setActionError(null);
    setNotice(null);
    try {
      if (action === 'create') {
        const preset = createPresetFromRun(detail, selectedRec, modelInfo);
        setNotice(`Created preset "${preset.name}" and linked it to ${detail.model}.`);
      } else if (action === 'create-apply') {
        const preset = createPresetFromRun(detail, selectedRec, modelInfo);
        await applyRunNow(detail, selectedRec, { save: true });
        setNotice(`Created preset "${preset.name}" and applied it to ${detail.model}.`);
      } else {
        await applyRunNow(detail, selectedRec, { save: false });
        setNotice(`Loaded ${detail.model} with the recommended settings (nothing saved).`);
      }
    } catch (err) {
      setActionError(err instanceof Error ? err.message : 'Action failed.');
    }
  }, [detail, selectedRec, modelInfo]);

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
                      </li>
                    ))}
                  </ul>
                </div>
              )}

              {detail?.measurements && (detail.measurements.fit.length > 0 || detail.measurements.bench.length > 0) && (
                <div className="slideover__section">
                  <h3>Measurements</h3>
                  <p className="preset-help">
                    {detail.measurements.fit.length} memory-fit probes · {detail.measurements.bench.length} benchmark samples
                  </p>
                  {ladder.length > 0 && (
                    <div className="autoopt-alt-table-wrap">
                      <table className="autoopt-alt-table" data-autoopt-bench-ladder>
                        <thead>
                          <tr><th>batch</th><th>ubatch</th><th>pp t/s</th><th>tg t/s</th></tr>
                        </thead>
                        <tbody>
                          {ladder.map((row, index) => (
                            <tr key={index}>
                              <td>{String(row.b ?? '—')}</td>
                              <td>{String(row.ub ?? '—')}</td>
                              <td>{formatNumber(row.pp_ts)}</td>
                              <td>{formatNumber(row.tg_ts)}</td>
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
                        {selectedRec.ctx_size !== undefined && <span className="autoopt-chip">ctx {selectedRec.ctx_size.toLocaleString()}</span>}
                        {selectedRec.mmproj_enabled !== undefined && <span className="autoopt-chip">vision {selectedRec.mmproj_enabled ? 'on' : 'off'}</span>}
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
                          {ppDepthKeys.length > 0
                            ? ppDepthKeys.map(key => <th key={key}>pp t/s @{key.replace(/^d/, '')}</th>)
                            : <th>pp t/s</th>}
                          <th>tg t/s</th>
                          <th>VRAM MiB</th>
                          <th />
                        </tr>
                      </thead>
                      <tbody>
                        {recommendations.map((rec, index) => (
                          <tr key={rec.label} className={index === selectedRecIndex ? 'is-selected' : ''}>
                            <td>{rec.label}</td>
                            <td>{rec.llamacpp_backend || '—'}</td>
                            <td>{rec.ctx_size !== undefined ? rec.ctx_size.toLocaleString() : '—'}</td>
                            {ppDepthKeys.length > 0
                              ? ppDepthKeys.map(key => <td key={key}>{expectedValue(rec, 'pp_ts', key)}</td>)
                              : <td>{expectedValue(rec, 'pp_ts')}</td>}
                            <td>{expectedValue(rec, 'tg_ts')}</td>
                            <td>{rec.expected?.vram_mib !== undefined ? rec.expected.vram_mib.toLocaleString() : '—'}</td>
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

            <div className="slideover__foot">
              {run.status === 'completed' && selectedRec ? (
                <>
                  <button className="btn btn--ghost" onClick={() => void runAction('try')} data-autoopt-try-now>Try now without saving</button>
                  <button className="btn btn--ghost" onClick={() => void runAction('create')} data-autoopt-create-preset>Create preset</button>
                  <button className="btn btn--primary" onClick={() => void runAction('create-apply')} data-autoopt-create-apply>Create preset &amp; apply now</button>
                </>
              ) : (
                <>
                  {summary && isAutoOptRunActive(summary) && (
                    <button className="btn btn--ghost" onClick={() => void autoOptStore.cancelRun(run.id).catch(() => {})}>Cancel run</button>
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
