import React, { useMemo } from 'react';
import { AreaChart as RechartArea, Area, YAxis, CartesianGrid, Tooltip, ResponsiveContainer } from 'recharts';
import { LoadedModel } from '../api';
import { useDashboardData, HISTORY_LEN, SessionCounters } from '../hooks/useDashboardData';

/* ── Helpers ───────────────────────────────────────────────── */

function pct(value: number | null): string {
  if (value == null || value < 0) return '—';
  return `${Math.round(value)}%`;
}

function fmtNum(n: number): string {
  if (n >= 1_000_000) return `${(n / 1_000_000).toFixed(1)}M`;
  if (n >= 1_000) return `${(n / 1_000).toFixed(1)}K`;
  return n.toLocaleString();
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

function relativeTime(ms: number): string {
  const YEAR_2020_MS = 1577836800000;
  if (ms > YEAR_2020_MS) {
    const delta = Date.now() - ms;
    if (delta < 5000) return 'just now';
    if (delta < 60000) return `${Math.round(delta / 1000)}s ago`;
    if (delta < 3600000) return `${Math.round(delta / 60000)}m ago`;
    return `${Math.round(delta / 3600000)}h ago`;
  }
  const totalSec = Math.floor(ms / 1000);
  if (totalSec < 60) return `${totalSec}s uptime`;
  const m = Math.floor(totalSec / 60);
  if (m < 60) return `${m}m uptime`;
  const h = Math.floor(m / 60);
  return `${h}h ${m % 60}m uptime`;
}

function typeIcon(type: string): string {
  switch (type) {
    case 'llm': return 'LLM';
    case 'embedding': return 'Emb';
    case 'reranking': return 'Rank';
    case 'transcription': return 'ASR';
    case 'image': return 'Image';
    case 'tts': return 'TTS';
    default: return 'Model';
  }
}

const SLOT_COLORS = ['#e8c66b', '#7baed4', '#7fb38a', '#b07df0', '#e07b7b', '#7bc8c8'];

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
          stroke="var(--surface-3)" strokeWidth="5" opacity="0.4" />
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

/* ── Smooth Recharts wrapper ────────────────────────────────── */

const CHART_TOOLTIP_STYLE: React.CSSProperties = {
  background: '#1d1b16',
  border: '1px solid rgba(255,245,220,0.1)',
  borderRadius: 8,
  fontSize: 12,
  padding: '6px 10px',
};

const SmoothChart = React.memo<{
  data: Record<string, number>[];
  series: { key: string; color: string; name?: string }[];
  height?: number;
  unit?: string;
}>(({ data, series, height = 80, unit = '' }) => (
  <ResponsiveContainer width="100%" height={height}>
    <RechartArea data={data} margin={{ top: 4, right: 0, bottom: 0, left: 0 }}>
      <defs>
        {series.map(s => (
          <linearGradient key={s.key} id={`grad-${s.key}`} x1="0" y1="0" x2="0" y2="1">
            <stop offset="0%" stopColor={s.color} stopOpacity={0.3} />
            <stop offset="100%" stopColor={s.color} stopOpacity={0} />
          </linearGradient>
        ))}
      </defs>
      <CartesianGrid strokeDasharray="3 3" stroke="rgba(255,245,220,0.04)" vertical={false} />
      <YAxis hide domain={[0, 'auto']} />
      <Tooltip
        contentStyle={CHART_TOOLTIP_STYLE}
        labelStyle={{ display: 'none' }}
        formatter={(value) => [`${Number(value ?? 0).toFixed(1)}${unit}`, '']}
        isAnimationActive={false}
      />
      {series.map(s => (
        <Area
          key={s.key}
          type="monotone"
          dataKey={s.key}
          stroke={s.color}
          fill={`url(#grad-${s.key})`}
          strokeWidth={2}
          dot={false}
          isAnimationActive={false}
          name={s.name || s.key}
        />
      ))}
    </RechartArea>
  </ResponsiveContainer>
));

/* ── Model row ─────────────────────────────────────────────── */

const ModelRow = React.memo<{ model: LoadedModel }>(({ model }) => (
  <div className="dash2-model">
    <span className="dash2-model__icon">{typeIcon(model.type)}</span>
    <div className="dash2-model__info">
      <span className="dash2-model__name">{model.model_name}</span>
      <span className="dash2-model__meta">
        {model.recipe} · {model.device} · PID {model.pid}
      </span>
    </div>
    <span className="dash2-model__badge" data-type={model.type}>{model.type}</span>
    <span className="dash2-model__time">{relativeTime(model.last_use)}</span>
  </div>
));

/* ══════════════════════════════════════════════════════════════
   ██  MAIN DASHBOARD
   ══════════════════════════════════════════════════════════════ */

const POLL_INTERVAL = 2000;

const Dashboard: React.FC = () => {
  const {
    health, stats, sysStats, slots, slotLive,
    lastError, slotsUnsupported, slotStatus, paused, setPaused,
    counters, getSlotTarget, loadedModels,
    latestTps, latestPP, activeSlotCount, overallCacheUtil,
    hasGpu, hasNpu, modelsByType,
    aggChartData, slotChartData, sysChartData, cacheChartData,
  } = useDashboardData();

  /* ── Render ──────────────────────────────────────────────── */

  return (
    <section className="dash2" data-view="dashboard">
      {/* ═══ Top bar ═══ */}
      <header className="dash2-bar">
        <div className="dash2-bar__left">
          <span className="dash2-bar__dot" data-connected={!!health} />
          <span className="dash2-bar__title">
            {health ? `Lemonade ${health.version}` : 'Disconnected'}
          </span>
          {health && <span className="dash2-bar__uptime">{elapsed(counters.sessionStart)}</span>}
        </div>
        <div className="dash2-bar__right">
          <button className={`dash2-bar__btn ${paused ? 'is-paused' : ''}`}
            onClick={() => setPaused(p => !p)} title={paused ? 'Resume' : 'Pause'}>
            {paused ? 'Resume' : 'Pause'}
          </button>
        </div>
      </header>

      {lastError && <div className="dash2-err">Warning: {lastError}</div>}

      <div className="dash2-scroll">
        {/* ═══ HERO — Aggregate Throughput ═══ */}
        <div className="dash2-card dash2-card--glow">
          <h2 className="dash2-card__h">Aggregate Throughput</h2>

          {/* Inline metrics — guaranteed visible with explicit colors */}
          <div className="dash2-hero-metrics">
            <div className="dash2-hero-metric">
              <span className="dash2-hero-metric__val dash2-hero-metric__val--tps">
                {latestTps > 0.05 ? latestTps.toFixed(1) : (counters.peakTps > 0 ? '0.0' : '—')}
              </span>
              <span className="dash2-hero-metric__unit">tok/s</span>
              {counters.peakTps > 0 && (
                <span className="dash2-hero-metric__peak">
                  peak {counters.peakTps.toFixed(1)}
                </span>
              )}
            </div>
            <div className="dash2-hero-metric">
              <span className="dash2-hero-metric__val dash2-hero-metric__val--pp">
                {latestPP > 0.05 ? latestPP.toFixed(0) : (counters.peakPromptTps > 0 ? '0' : '—')}
              </span>
              <span className="dash2-hero-metric__unit">pp/s</span>
            </div>
            <div className="dash2-hero-metric">
              <span className="dash2-hero-metric__val dash2-hero-metric__val--stream">
                {activeSlotCount}
              </span>
              <span className="dash2-hero-metric__unit">
                {activeSlotCount === 1 ? 'stream' : 'streams'}
              </span>
            </div>
            <div className="dash2-hero-metric__totals">
              <span>{fmtNum(counters.totalTokensGenerated)} total tokens</span>
            </div>
          </div>

          <SmoothChart
            data={aggChartData}
            series={[
              { key: 'genTps', color: '#e8c66b', name: 'Generation TPS' },
              { key: 'ppTps', color: '#7baed4', name: 'Prompt Processing' },
            ]}
            height={120}
            unit=" tok/s"
          />
          <div className="dash2-chart-legend">
            <span className="dash2-chart-legend__item">
              <span className="dash2-chart-legend__swatch dash2-chart-legend__swatch--tps" />
              Generation TPS
            </span>
            <span className="dash2-chart-legend__item">
              <span className="dash2-chart-legend__swatch dash2-chart-legend__swatch--pp" />
              Prompt Processing
            </span>
          </div>
        </div>

        {/* ═══ Parallel Slots — Per-Slot Metrics ═══ */}
        {slots.length > 0 ? (
          <div className="dash2-card">
            <h2 className="dash2-card__h">
              Parallel Slots
              <span className="dash2-card__badge">{activeSlotCount} / {slots.length} active</span>
            </h2>
            <SmoothChart
              data={slotChartData}
              series={slots.map((s, i) => ({
                key: `slot${s.id}`,
                color: SLOT_COLORS[i % SLOT_COLORS.length],
                name: `Slot ${s.id}`,
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
                    <span className="dash2-slot-legend__label">Slot {s.id}</span>
                    <span className={`dash2-slot-legend__tps ${isActive ? 'dash2-slot-legend__tps--active' : 'dash2-slot-legend__tps--idle'}`}>
                      {tps > 0.05 ? `${tps.toFixed(1)} tok/s` : 'idle'}
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
            <h2 className="dash2-card__h">Parallel Slots</h2>
            <p className="dash2-card__text">
              {slotsUnsupported ? 'No compatible slot data for the loaded backend.' : 'No slot data yet.'} {slotStatus}
            </p>
          </div>
        )}

        {/* ═══ Two-column: System Vitals | Last Inference ═══ */}
        <div className="dash2-grid-2col">
          <div className="dash2-card">
            <h2 className="dash2-card__h">System Vitals</h2>
            <div className="dash2-gauges">
              <RingGauge label="CPU" value={sysStats?.cpu_percent ?? null}
                subtitle={pct(sysStats?.cpu_percent ?? null)} />
              <RingGauge label="RAM" value={sysStats?.memory_gb ?? null} max={64} unit="GB"
                color="var(--info)" subtitle={`${(sysStats?.memory_gb ?? 0).toFixed(1)} GB`} />
              {hasGpu && <RingGauge label="GPU" value={sysStats!.gpu_percent!}
                color="var(--accent)" subtitle={pct(sysStats!.gpu_percent)} />}
              {hasGpu && sysStats!.vram_gb != null && sysStats!.vram_gb >= 0 && (
                <RingGauge label="VRAM" value={sysStats!.vram_gb!} max={32} unit="GB"
                  color="var(--warn)" subtitle={`${sysStats!.vram_gb!.toFixed(1)} GB`} />
              )}
              {hasNpu && <RingGauge label="NPU" value={sysStats!.npu_percent!}
                color="#b07df0" subtitle={pct(sysStats!.npu_percent)} />}
              <RingGauge label="KV Cache" value={overallCacheUtil}
                color="var(--warn)" subtitle={overallCacheUtil != null ? `${overallCacheUtil.toFixed(0)}%` : '—'} />
            </div>
            <SmoothChart
              data={sysChartData}
              series={[
                { key: 'cpu', color: '#7fb38a', name: 'CPU %' },
                ...(hasGpu ? [{ key: 'gpu', color: '#e8c66b', name: 'GPU %' }] : []),
              ]}
              height={56}
              unit="%"
            />
          </div>

          {stats ? (
            <div className="dash2-card">
              <h2 className="dash2-card__h">Last Inference</h2>
              <div className="dash2-inf-grid">
                <div className="dash2-inf">
                  <span className="dash2-inf__v">{stats.tokens_per_second > 0 ? stats.tokens_per_second.toFixed(1) : '—'}</span>
                  <span className="dash2-inf__l">Decode tok/s</span>
                </div>
                <div className="dash2-inf">
                  <span className="dash2-inf__v">{stats.time_to_first_token > 0 ? `${(stats.time_to_first_token * 1000).toFixed(0)}` : '—'}</span>
                  <span className="dash2-inf__l">TTFT (ms)</span>
                </div>
                <div className="dash2-inf">
                  <span className="dash2-inf__v">{stats.prompt_tokens || stats.input_tokens}</span>
                  <span className="dash2-inf__l">Prompt Tokens</span>
                </div>
                <div className="dash2-inf">
                  <span className="dash2-inf__v">{stats.output_tokens}</span>
                  <span className="dash2-inf__l">Completion Tokens</span>
                </div>
              </div>
              <div className="dash2-mt-auto">
                <SmoothChart
                  data={cacheChartData}
                  series={[{ key: 'cache', color: '#d9a35b', name: 'KV Cache %' }]}
                  height={56}
                  unit="%"
                />
              </div>
            </div>
          ) : (
            <div className="dash2-card">
              <h2 className="dash2-card__h">Last Inference</h2>
              <div className="dash2-empty">No inference data yet — send a request to see stats</div>
            </div>
          )}
        </div>

        {/* ═══ Two-column: Loaded Models | Model Capacity ═══ */}
        <div className="dash2-grid-2col">
          <div className="dash2-card">
            <h2 className="dash2-card__h">
              Loaded Models
              {loadedModels.length > 0 && <span className="dash2-card__badge">{loadedModels.length}</span>}
            </h2>
            {loadedModels.length === 0 ? (
              <div className="dash2-empty">No models loaded</div>
            ) : (
              <div className="dash2-models">
                {loadedModels.map(m => <ModelRow key={m.model_name} model={m} />)}
              </div>
            )}
          </div>

          {health?.max_models ? (
            <div className="dash2-card">
              <h2 className="dash2-card__h">Model Capacity</h2>
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
              <h2 className="dash2-card__h">Model Capacity</h2>
              <div className="dash2-empty">Connect to a server to see capacity</div>
            </div>
          )}
        </div>

        {/* ═══ Session Summary (hidden until inference happens) ═══ */}
        {(counters.totalTokensGenerated > 0 || counters.totalPromptTokens > 0 || counters.peakTps > 0) && (
        <div className="dash2-card dash2-card--summary">
          <h3 className="dash2-card__h">Session Summary</h3>
          <div className="dash2-summary">
            <span className="dash2-summary__item">
              <span className="dash2-summary__val">{fmtNum(counters.totalTokensGenerated)}</span>
              <span className="dash2-summary__lbl">Tokens generated</span>
            </span>
            <span className="dash2-summary__item">
              <span className="dash2-summary__val">{fmtNum(counters.totalPromptTokens)}</span>
              <span className="dash2-summary__lbl">Tokens processed</span>
            </span>
            <span className="dash2-summary__item">
              <span className="dash2-summary__val">{counters.peakTps > 0 ? counters.peakTps.toFixed(1) : '—'}</span>
              <span className="dash2-summary__lbl">Peak TPS</span>
            </span>
            <span className="dash2-summary__item">
              <span className="dash2-summary__val">{elapsed(counters.sessionStart)}</span>
              <span className="dash2-summary__lbl">Session time</span>
            </span>
          </div>
        </div>
        )}
      </div>
    </section>
  );
};

export default Dashboard;
