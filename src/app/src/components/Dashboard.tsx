import React, { lazy, Suspense, useCallback, useMemo, useRef, useState } from 'react';
import { api, LoadedModel } from '../api';
import { capabilityFromLoaded } from '../modelCapabilities';
import { backendCompactLabel } from '../modelPresentation';
import { useDashboardData } from '../hooks/useDashboardData';
import { dashboardMemoryTopology } from '../features/dashboard/memoryTopology';
import { WorkspaceActionButton, WorkspaceList, WorkspaceListRow, WorkspacePaneHeader } from './WorkspacePanels';
import { useI18n } from '../i18n';

/* ── Helpers ───────────────────────────────────────────────── */

function pct(value: number | null): string {
  if (value == null || value < 0) return '—';
  return `${Math.round(value)}%`;
}


function elapsed(startMs: number): string {
  const s = Math.floor((Date.now() - startMs) / 1000);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (h > 0) return `${h}h ${m}m ${sec}s`;
  if (m > 0) return `${m}m ${sec}s`;
  return `${sec}s`;
}

function durationFromMs(ms: number): string {
  const totalSec = Math.max(0, Math.floor(ms / 1000));
  const h = Math.floor(totalSec / 3600);
  const m = Math.floor((totalSec % 3600) / 60);
  const sec = totalSec % 60;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${sec}s`;
  return `${sec}s`;
}

function typeIcon(type: string): string {
  switch (type) {
    case 'llm': return 'LLM';
    case 'embedding': return 'Emb';
    case 'reranking': return 'Rank';
    case 'transcription': return 'ASR';
    case 'image': return 'Image';
    case 'audio-generation': return 'Audio';
    case 'tts': return 'TTS';
    case '3d':
    case 'model3d': return '3D';
    default: return 'Model';
  }
}

const SLOT_COLORS = [
  'var(--chart-series-1)',
  'var(--chart-series-2)',
  'var(--chart-series-3)',
  'var(--chart-series-4)',
  'var(--chart-series-5)',
  'var(--chart-series-6)',
];

/* ── SVG Ring Gauge with glow ──────────────────────────────── */

const RingGauge = React.memo<{
  value: number | null;
  max?: number;
  size?: number;
  label: string;
  unit?: string;
  color?: string;
  subtitle?: string;
}>(({ value, max = 100, size = 110, label, unit = '%', color, subtitle }) => {
  const safeVal = value != null && value >= 0 ? value : 0;
  const pctVal = Math.min(100, (safeVal / max) * 100);
  const unavailable = value == null || value < 0;

  const r = 42;
  const circumference = 2 * Math.PI * r;
  const offset = circumference - (pctVal / 100) * circumference;

  const autoColor = pctVal < 50 ? 'var(--success)' : pctVal < 80 ? 'var(--accent)' : 'var(--danger)';
  const ringColor = color || autoColor;
  const filterId = `glow-${label.replace(/\s/g, '')}`;

  return (
    <div className="dash2-gauge">
      <svg width={size} height={size} viewBox="0 0 100 100" className="dash2-gauge__svg">
        <defs>
          <filter id={filterId} x="-50%" y="-50%" width="200%" height="200%">
            <feGaussianBlur stdDeviation="3" result="blur" />
            <feFlood floodColor={ringColor} floodOpacity="0.35" result="color" />
            <feComposite in="color" in2="blur" operator="in" result="glow" />
            <feMerge>
              <feMergeNode in="glow" />
              <feMergeNode in="SourceGraphic" />
            </feMerge>
          </filter>
        </defs>
        <circle cx="50" cy="50" r={r} fill="none"
          stroke="var(--border)" strokeWidth="5" />
        {!unavailable && pctVal > 0 && (
          <circle cx="50" cy="50" r={r} fill="none"
            stroke={ringColor} strokeWidth="5"
            strokeDasharray={circumference}
            strokeDashoffset={offset}
            strokeLinecap="round"
            transform="rotate(-90 50 50)"
            filter={`url(#${filterId})`}
            style={{ transition: 'stroke-dashoffset 0.8s cubic-bezier(0.4,0,0.2,1)' }}
          />
        )}
      </svg>
      <div className="dash2-gauge__center">
        <span className="dash2-gauge__value">
          {unavailable ? '—' : (unit === '%' ? `${Math.round(safeVal)}` : safeVal.toFixed(1))}
        </span>
        <span className="dash2-gauge__unit">{unavailable ? '' : unit}</span>
      </div>
      <span className="dash2-gauge__label">{label}</span>
      {subtitle && <span className="dash2-gauge__sub">{subtitle}</span>}
    </div>
  );
});

/* ── Smooth chart wrapper ───────────────────────────────────── */

// Recharts is by far the heaviest dependency of the default Monitor section.
// Render the dashboard shell immediately and hydrate charts in a nested async
// chunk so opening/reloading Monitor never waits for the charting library.
const LazyDashboardChart = lazy(() =>
  import(/* webpackChunkName: "monitor-charts" */ './DashboardChart')
);

const SmoothChart = React.memo<{
  data: Record<string, number>[];
  series: { key: string; color: string; name?: string }[];
  height?: number;
  unit?: string;
}>(({ data, series, height = 80, unit = '' }) => (
  <Suspense fallback={<div aria-hidden="true" style={{ height }} />}>
    <LazyDashboardChart data={data} series={series} height={height} unit={unit} />
  </Suspense>
));

/* ── Model row ─────────────────────────────────────────────── */

/**
 * The same loaded-model row the composer's picker renders. A monitoring view
 * showing what is resident, and a picker showing what you can switch to, are
 * looking at one list — so they use one row, eject included.
 */
const ModelRow = React.memo<{ model: LoadedModel; onEject: (name: string) => void; ejecting: boolean }>(
  ({ model, onEject, ejecting }) => {
    const { t, formatRelativeTime } = useI18n('monitor');
    const lastUse = (() => {
      const YEAR_2020_MS = 1577836800000;
      if (model.last_use <= YEAR_2020_MS) return t('dashboard.uptime', { duration: durationFromMs(model.last_use) });
      const delta = Math.max(0, Date.now() - model.last_use);
      if (delta < 5000) return formatRelativeTime(0, 'second', { numeric: 'auto' });
      if (delta < 60_000) return formatRelativeTime(-Math.max(1, Math.round(delta / 1000)), 'second', { numeric: 'always' });
      if (delta < 3_600_000) return formatRelativeTime(-Math.round(delta / 60_000), 'minute');
      return formatRelativeTime(-Math.round(delta / 3_600_000), 'hour');
    })();
    const facts = [
      model.recipe && backendCompactLabel(model.recipe),
      model.device,
      model.pid != null && `PID ${model.pid}`,
      lastUse,
    ].filter(Boolean) as string[];

    return (
    <WorkspaceListRow
      selectable={false}
      capability={capabilityFromLoaded(model)}
      title={model.model_name}
      // Every row in this card is loaded, so the card heading carries that and
      // no row repeats it. Only an eject in flight is a state worth a dot.
      status={ejecting ? 'busy' : undefined}
      statusText={ejecting ? t('dashboard.ejecting') : undefined}
      meta={facts}
      disabled={ejecting}
      ariaLabel={`${model.model_name}, ${facts.join(', ')}`}
      action={{
        icon: 'eject',
        label: t('dashboard.ejectModel', { name: model.model_name }),
        onClick: () => onEject(model.model_name),
        latched: true,
      }}
    />
    );
  },
);

/* ══════════════════════════════════════════════════════════════
   ██  MAIN DASHBOARD
   ══════════════════════════════════════════════════════════════ */

interface DashboardProps {
  isActive: boolean;
}

const Dashboard: React.FC<DashboardProps> = ({ isActive }) => {
  const { t, formatNumber } = useI18n('monitor');
  const {
    health, stats, sysStats, systemInfo, slots, slotLive,
    lastError, slotsUnsupported, slotStatus, paused, setPaused,
    counters, getSlotTarget, loadedModels, refresh,
    latestTps, latestPP, activeSlotCount, overallCacheUtil,
    hasGpu, hasNpu, modelsByType,
    aggChartData, slotChartData, sysChartData, cacheChartData,
  } = useDashboardData(isActive);

  const [ejecting, setEjecting] = useState<string | null>(null);
  const ejectingRef = useRef<string | null>(null);
  const handleEject = useCallback((modelName: string) => {
    if (ejectingRef.current) return;
    ejectingRef.current = modelName;
    setEjecting(modelName);
    void (async () => {
      try {
        await api.unloadModel(modelName);
        await refresh();
      } finally {
        ejectingRef.current = null;
        setEjecting(null);
      }
    })();
  }, [refresh]);

  const serverVersion = String(health?.version ?? '').trim();
  const displayServerVersion = serverVersion
    && !['unknown', 'null', 'undefined', 'none', 'n/a'].includes(serverVersion.toLowerCase())
    ? serverVersion
    : t('dashboard.versionUnknown');
  const memoryTopology = useMemo(() => dashboardMemoryTopology(systemInfo), [systemInfo]);
  const ramUsedGb = sysStats?.memory_gb ?? null;
  const ramGaugeMax = memoryTopology.hostTotalGb
    ?? Math.max(64, Math.ceil((ramUsedGb || 0) / 16) * 16);
  const vramGaugeMax = memoryTopology.gpuTotalGb
    ?? Math.max(32, Math.ceil(((sysStats?.vram_gb ?? 0) || 0) / 8) * 8);

  /* ── Render ──────────────────────────────────────────────── */

  return (
    <section className="dash2 dashboard-workspace" data-view="dashboard">
      <div className="workspace-pane dashboard-main">
        <WorkspacePaneHeader
          className="dashboard-header"
          headingLevel={1}
          title={t('dashboard.title')}
          subtitle={t('dashboard.subtitle')}
          actions={<div className="dashboard-header__actions">
            <div className="dashboard-header__server" data-connected={!!health}>
              <span className="dash2-bar__dot" data-connected={!!health} />
              <span><strong>{health ? `Lemonade ${displayServerVersion}` : t('dashboard.disconnected')}</strong>{health && <small>{elapsed(counters.sessionStart)}</small>}</span>
            </div>
            <WorkspaceActionButton
              size="small"
              appearance="secondary"
              icon={paused ? 'play' : 'pause'}
              className={`dash2-bar__btn${paused ? ' is-paused' : ''}`}
              onClick={() => setPaused(p => !p)}
              title={paused ? t('dashboard.resumeUpdates') : t('dashboard.pauseUpdates')}
              aria-label={paused ? t('dashboard.resumeUpdates') : t('dashboard.pauseUpdates')}
              data-dashboard-poll-toggle>
              {paused ? t('dashboard.resume') : t('dashboard.pause')}
            </WorkspaceActionButton>
          </div>}
        />

      {lastError && <div className="dash2-err">{t('dashboard.warning', { error: lastError })}</div>}

      <div className="dash2-scroll">
        {/* ═══ HERO — Aggregate Throughput ═══ */}
        <div className="dash2-card">
          <h2 className="dash2-card__h">{t('dashboard.aggregate')}</h2>

          {/* Inline metrics — guaranteed visible with explicit colors */}
          <div className="dash2-hero-metrics">
            <div className="dash2-hero-metric">
              <span className="dash2-hero-metric__val dash2-hero-metric__val--tps">
                {latestTps > 0.05 ? formatNumber(latestTps, { minimumFractionDigits: 1, maximumFractionDigits: 1 }) : (counters.peakTps > 0 ? formatNumber(0, { minimumFractionDigits: 1, maximumFractionDigits: 1 }) : '—')}
              </span>
              <span className="dash2-hero-metric__unit">tok/s</span>
              {counters.peakTps > 0 && (
                <span className="dash2-hero-metric__peak">
                  {t('dashboard.peak', { value: formatNumber(counters.peakTps, { minimumFractionDigits: 1, maximumFractionDigits: 1 }) })}
                </span>
              )}
            </div>
            <div className="dash2-hero-metric">
              <span className="dash2-hero-metric__val dash2-hero-metric__val--pp">
                {latestPP > 0.05 ? formatNumber(latestPP, { maximumFractionDigits: 0 }) : (counters.peakPromptTps > 0 ? formatNumber(0, { maximumFractionDigits: 0 }) : '—')}
              </span>
              <span className="dash2-hero-metric__unit">pp/s</span>
            </div>
            <div className="dash2-hero-metric">
              <span className="dash2-hero-metric__val dash2-hero-metric__val--stream">
                {activeSlotCount}
              </span>
              <span className="dash2-hero-metric__unit">
                {t('dashboard.streams', { count: activeSlotCount })}
              </span>
            </div>
            <div className="dash2-hero-metric__totals">
              <span>{t('dashboard.totalTokens', { count: counters.totalTokensGenerated })}</span>
            </div>
          </div>

          <SmoothChart
            data={aggChartData}
            series={[
              { key: 'genTps', color: 'var(--chart-series-1)', name: t('dashboard.generationTps') },
              { key: 'ppTps', color: 'var(--chart-series-2)', name: t('dashboard.promptProcessing') },
            ]}
            height={120}
            unit=" tok/s"
          />
          <div className="dash2-chart-legend">
            <span className="dash2-chart-legend__item">
              <span className="dash2-chart-legend__swatch dash2-chart-legend__swatch--tps" />
              {t('dashboard.generationTps')}
            </span>
            <span className="dash2-chart-legend__item">
              <span className="dash2-chart-legend__swatch dash2-chart-legend__swatch--pp" />
              {t('dashboard.promptProcessing')}
            </span>
          </div>
        </div>

        {/* ═══ Parallel Slots — Per-Slot Metrics ═══ */}
        {slots.length > 0 ? (
          <div className="dash2-card">
            <h2 className="dash2-card__h">
              {t('dashboard.parallelSlots')}
              <span className="dash2-card__badge">{t('dashboard.activeSlots', { active: activeSlotCount, total: slots.length })}</span>
            </h2>
            <SmoothChart
              data={slotChartData}
              series={slots.map((s, i) => ({
                key: `slot${s.id}`,
                color: SLOT_COLORS[i % SLOT_COLORS.length],
                name: t('dashboard.slot', { id: s.id }),
              }))}
              height={160}
              unit=" tok/s"
            />
            <div className="dash2-slot-legend">
              {slots.map((s, i) => {
                const live = slotLive[s.id];
                const target = getSlotTarget(s.id);
                // Use the higher of poll-based live TPS or smoothed WS target
                const tps = Math.max(live?.tps || 0, target?.tps || 0);
                const isActive = tps > 0.05 || live?.isActive || target?.isActive || s.is_processing;
                const color = SLOT_COLORS[i % SLOT_COLORS.length];
                // Use target cacheUtil (updated from poll) — more reliable than
                // recalculating from cache_tokens which may be empty between requests
                const cu = target?.cacheUtil || 0;
                return (
                  <div key={s.id} className={`dash2-slot-legend__item${isActive ? '' : ' dash2-slot-legend__item--idle'}`}>
                    <span className="dash2-slot-legend__dot" style={{ background: color, boxShadow: isActive ? `0 0 6px ${color}` : 'none' }} />
                    <span className="dash2-slot-legend__label">{t('dashboard.slot', { id: s.id })}</span>
                    <span className={`dash2-slot-legend__tps ${isActive ? 'dash2-slot-legend__tps--active' : 'dash2-slot-legend__tps--idle'}`}>
                      {tps > 0.05 ? `${formatNumber(tps, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} tok/s` : t('dashboard.idle')}
                    </span>
                    <span className="dash2-slot-legend__kv">
                      KV {pct(cu)}
                    </span>
                  </div>
                );
              })}
            </div>
          </div>
        ) : (
          <div className="dash2-card dash2-card--notice">
            <h2 className="dash2-card__h">{t('dashboard.parallelSlots')}</h2>
            <p className="dash2-card__text">
              {slotsUnsupported ? t('dashboard.noCompatibleSlots') : t('dashboard.noSlotData')} {slotStatus}
            </p>
          </div>
        )}

        {/* ═══ Two-column: System Vitals | Last Inference ═══ */}
        <div className="dash2-grid-2col">
          <div className="dash2-card">
            <h2 className="dash2-card__h">{t('dashboard.systemVitals')}</h2>
            <div className="dash2-gauges">
              <RingGauge label="CPU" value={sysStats?.cpu_percent ?? null}
                subtitle={pct(sysStats?.cpu_percent ?? null)} />
              {memoryTopology.unified ? (
                <RingGauge
                  label="RAM / VRAM"
                  value={ramUsedGb}
                  max={ramGaugeMax}
                  unit="GB"
                  color="var(--info)"
                  subtitle={ramUsedGb == null ? t('dashboard.sharedMemoryPool') : t('dashboard.usedSharedPool', { value: formatNumber(ramUsedGb, { minimumFractionDigits: 1, maximumFractionDigits: 1 }) })}
                />
              ) : (
                <RingGauge label="RAM" value={ramUsedGb} max={ramGaugeMax} unit="GB"
                  color="var(--info)" subtitle={ramUsedGb == null ? '—' : `${formatNumber(ramUsedGb, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} GB`} />
              )}
              {hasGpu && <RingGauge label="GPU" value={sysStats!.gpu_percent!}
                color="var(--accent)" subtitle={pct(sysStats!.gpu_percent)} />}
              {!memoryTopology.unified && hasGpu && sysStats!.vram_gb != null && sysStats!.vram_gb >= 0 && (
                <RingGauge label="VRAM" value={sysStats!.vram_gb!} max={vramGaugeMax} unit="GB"
                  color="var(--warn)" subtitle={`${formatNumber(sysStats!.vram_gb!, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} GB`} />
              )}
              {hasNpu && <RingGauge label="NPU" value={sysStats!.npu_percent!}
                color="var(--chart-series-4)" subtitle={pct(sysStats!.npu_percent)} />}
              <RingGauge label="KV Cache" value={overallCacheUtil}
                color="var(--warn)" subtitle={overallCacheUtil != null ? `${overallCacheUtil.toFixed(0)}%` : '—'} />
            </div>
            <SmoothChart
              data={sysChartData}
              series={[
                { key: 'cpu', color: 'var(--chart-series-3)', name: 'CPU %' },
                ...(hasGpu ? [{ key: 'gpu', color: 'var(--chart-series-1)', name: 'GPU %' }] : []),
              ]}
              height={56}
              unit="%"
            />
          </div>

          {stats ? (
            <div className="dash2-card">
              <h2 className="dash2-card__h">{t('dashboard.lastInference')}</h2>
              <div className="dash2-inf-grid">
                <div className="dash2-inf">
                  <span className="dash2-inf__v">{stats.tokens_per_second > 0 ? formatNumber(stats.tokens_per_second, { minimumFractionDigits: 1, maximumFractionDigits: 1 }) : '—'}</span>
                  <span className="dash2-inf__l">{t('dashboard.decodeTps')}</span>
                </div>
                <div className="dash2-inf">
                  <span className="dash2-inf__v">{stats.time_to_first_token > 0 ? formatNumber(stats.time_to_first_token * 1000, { maximumFractionDigits: 0 }) : '—'}</span>
                  <span className="dash2-inf__l">{t('dashboard.ttft')}</span>
                </div>
                <div className="dash2-inf">
                  <span className="dash2-inf__v">{stats.prompt_tokens || stats.input_tokens}</span>
                  <span className="dash2-inf__l">{t('dashboard.promptTokens')}</span>
                </div>
                <div className="dash2-inf">
                  <span className="dash2-inf__v">{stats.output_tokens}</span>
                  <span className="dash2-inf__l">{t('dashboard.completionTokens')}</span>
                </div>
              </div>
              <div className="dash2-mt-auto">
                <SmoothChart
                  data={cacheChartData}
                  series={[{ key: 'cache', color: 'var(--chart-cache)', name: 'KV Cache %' }]}
                  height={56}
                  unit="%"
                />
              </div>
            </div>
          ) : (
            <div className="dash2-card">
              <h2 className="dash2-card__h">{t('dashboard.lastInference')}</h2>
              <div className="dash2-empty">{t('dashboard.noInference')}</div>
            </div>
          )}
        </div>

        {/* ═══ Two-column: Loaded Models | Model Capacity ═══ */}
        <div className="dash2-grid-2col">
          <div className="dash2-card">
            <h2 className="dash2-card__h">
              {t('dashboard.loadedModels')}
              {loadedModels.length > 0 && <span className="dash2-card__badge">{loadedModels.length}</span>}
            </h2>
            {loadedModels.length === 0 ? (
              <div className="dash2-empty">{t('dashboard.noModels')}</div>
            ) : (
              <WorkspaceList className="dash2-models" label={t('dashboard.loadedModelsAria')} selectable={false}>
                {loadedModels.map(m => (
                  <ModelRow
                    key={m.model_name}
                    model={m}
                    onEject={handleEject}
                    ejecting={ejecting === m.model_name}
                  />
                ))}
              </WorkspaceList>
            )}
          </div>

          {health?.max_models ? (
            <div className="dash2-card">
              <h2 className="dash2-card__h">{t('dashboard.modelCapacity')}</h2>
              <div className="dash2-caps">
                {Object.entries(health.max_models).map(([type, max]) => {
                  const loaded = modelsByType[type]?.length || 0;
                  const pctUsed = max > 0 ? (loaded / max) * 100 : 0;
                  return (
                    <div className="dash2-cap" key={type}>
                      <span className="dash2-cap__type">{typeIcon(type)} {type}</span>
                      <div className="dash2-cap__track">
                        <div className="dash2-cap__fill" style={{
                          width: `${Math.min(100, pctUsed)}%`,
                          background: pctUsed >= 100 ? 'var(--danger)' : 'var(--accent)',
                        }} />
                      </div>
                      <span className="dash2-cap__label">{loaded} / {max}</span>
                    </div>
                  );
                })}
              </div>
            </div>
          ) : (
            <div className="dash2-card">
              <h2 className="dash2-card__h">{t('dashboard.modelCapacity')}</h2>
              <div className="dash2-empty">{t('dashboard.connectCapacity')}</div>
            </div>
          )}
        </div>

        {/* ═══ Session Summary (hidden until inference happens) ═══ */}
        {(counters.totalTokensGenerated > 0 || counters.totalPromptTokens > 0 || counters.peakTps > 0) && (
        <div className="dash2-card dash2-card--summary">
          <h3 className="dash2-card__h">{t('dashboard.sessionSummary')}</h3>
          <div className="dash2-summary">
            <span className="dash2-summary__item">
              <span className="dash2-summary__val">{formatNumber(counters.totalTokensGenerated, { notation: 'compact', maximumFractionDigits: 1 })}</span>
              <span className="dash2-summary__lbl">{t('dashboard.tokensGenerated')}</span>
            </span>
            <span className="dash2-summary__item">
              <span className="dash2-summary__val">{formatNumber(counters.totalPromptTokens, { notation: 'compact', maximumFractionDigits: 1 })}</span>
              <span className="dash2-summary__lbl">{t('dashboard.tokensProcessed')}</span>
            </span>
            <span className="dash2-summary__item">
              <span className="dash2-summary__val">{counters.peakTps > 0 ? counters.peakTps.toFixed(1) : '—'}</span>
              <span className="dash2-summary__lbl">{t('dashboard.peakTps')}</span>
            </span>
            <span className="dash2-summary__item">
              <span className="dash2-summary__val">{elapsed(counters.sessionStart)}</span>
              <span className="dash2-summary__lbl">{t('dashboard.sessionTime')}</span>
            </span>
          </div>
        </div>
        )}
      </div>
      </div>
    </section>
  );
};

export default Dashboard;
