import React, { useState, useEffect, useCallback, useRef, useMemo } from 'react';
import api, { HealthData, LoadedModel, StatsData, SystemStatsData, SlotData, SlotTimings } from '../api';

/* ── History ring buffer ───────────────────────────────────── */

const HISTORY_LEN = 60;

interface HistoryPoint {
  ts: number;
  cpu: number | null;
  ram: number | null;
  gpu: number | null;
  vram: number | null;
  npu: number | null;
  aggregateTps: number;
  aggregatePromptTps: number;
  activeSlots: number;
  totalSlots: number;
  cacheUtil: number | null;
}

/* ── Cumulative session counters ───────────────────────────── */

interface SessionCounters {
  totalTokensGenerated: number;
  totalPromptTokens: number;
  peakTps: number;
  peakPromptTps: number;
  sessionStart: number;
  prevSlotTokens: Map<number, { decoded: number; prompted: number }>;
}

function initCounters(): SessionCounters {
  return {
    totalTokensGenerated: 0,
    totalPromptTokens: 0,
    peakTps: 0,
    peakPromptTps: 0,
    sessionStart: Date.now(),
    prevSlotTokens: new Map(),
  };
}

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
    case 'llm': return '💬';
    case 'embedding': return '🔢';
    case 'reranking': return '🔀';
    case 'transcription': return '🎙';
    case 'image': return '🖼';
    case 'tts': return '🔊';
    default: return '⚙';
  }
}

/* ── SVG Ring Gauge with glow ──────────────────────────────── */

const RingGauge: React.FC<{
  value: number | null;
  max?: number;
  size?: number;
  label: string;
  unit?: string;
  color?: string;
  subtitle?: string;
}> = ({ value, max = 100, size = 110, label, unit = '%', color, subtitle }) => {
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
};

/* ── Area chart ────────────────────────────────────────────── */

const AreaChart: React.FC<{
  data: (number | null)[];
  width?: number;
  height?: number;
  color: string;
  fillOpacity?: number;
  label?: string;
  currentValue?: string;
}> = ({ data, width = 200, height = 60, color, fillOpacity = 0.15, label, currentValue }) => {
  const filtered = data.map(v => (v != null && v >= 0 ? v : 0));
  const max = Math.max(...filtered, 1);
  const step = width / Math.max(filtered.length - 1, 1);

  const points = filtered.map((v, i) => {
    const x = i * step;
    const y = height - (v / max) * (height - 8) - 4;
    return `${x},${y}`;
  }).join(' ');

  const fillPoints = `0,${height} ${points} ${width},${height}`;

  return (
    <div className="dash2-area">
      <div className="dash2-area__header">
        {label && <span className="dash2-area__label">{label}</span>}
        {currentValue && <span className="dash2-area__current" style={{ color }}>{currentValue}</span>}
      </div>
      <svg width="100%" height={height} viewBox={`0 0 ${width} ${height}`}
        preserveAspectRatio="none" className="dash2-area__svg">
        <defs>
          <linearGradient id={`areagrad-${label?.replace(/\s/g, '')}`} x1="0" y1="0" x2="0" y2="1">
            <stop offset="0%" stopColor={color} stopOpacity={fillOpacity} />
            <stop offset="100%" stopColor={color} stopOpacity="0" />
          </linearGradient>
        </defs>
        <polygon points={fillPoints} fill={`url(#areagrad-${label?.replace(/\s/g, '')})`} />
        <polyline
          points={points}
          fill="none"
          stroke={color}
          strokeWidth="2"
          strokeLinejoin="round"
          strokeLinecap="round"
          vectorEffect="non-scaling-stroke"
        />
      </svg>
    </div>
  );
};

/* ── Throughput hero stat ──────────────────────────────────── */

const HeroStat: React.FC<{
  value: number;
  label: string;
  unit: string;
  peak?: number;
  color: string;
  secondary?: string;
}> = ({ value, label, unit, peak, color, secondary }) => (
  <div className="dash2-hero">
    <div className="dash2-hero__num">
      <span className="dash2-hero__val" style={{ color }}>
        {value > 0 ? value.toFixed(1) : '—'}
      </span>
      <span className="dash2-hero__unit">{unit}</span>
    </div>
    <span className="dash2-hero__label">{label}</span>
    {peak != null && peak > 0 && (
      <span className="dash2-hero__peak">⚡ Peak {peak.toFixed(1)} {unit}</span>
    )}
    {secondary && <span className="dash2-hero__secondary">{secondary}</span>}
  </div>
);

/* ── Slot mini card ────────────────────────────────────────── */

const SlotMini: React.FC<{ slot: SlotData }> = ({ slot }) => {
  const cacheLen = slot.cache_tokens?.length || 0;
  const cacheUtil = slot.n_ctx > 0 ? (cacheLen / slot.n_ctx) * 100 : 0;
  const t = slot.timings || {} as SlotTimings;

  return (
    <div className={`dash2-slot ${slot.is_processing ? 'dash2-slot--active' : ''}`}>
      <div className="dash2-slot__head">
        <span className="dash2-slot__id">Slot {slot.id}</span>
        <span className={`dash2-slot__state ${slot.is_processing ? 'dash2-slot__state--on' : ''}`}>
          <span className="dash2-slot__dot" />
          {slot.is_processing ? 'Active' : 'Idle'}
        </span>
      </div>
      <div className="dash2-slot__bar">
        <div className="dash2-slot__fill" style={{
          width: `${Math.min(100, cacheUtil)}%`,
          background: cacheUtil > 80 ? 'var(--danger)' : cacheUtil > 50 ? 'var(--accent)' : 'var(--success)',
        }} />
      </div>
      <div className="dash2-slot__stats">
        <span><b>{t.predicted_per_second > 0 ? t.predicted_per_second.toFixed(1) : '—'}</b> tok/s</span>
        <span><b>{t.prompt_per_second > 0 ? t.prompt_per_second.toFixed(0) : '—'}</b> pp/s</span>
        <span><b>{slot.n_decoded}</b> decoded</span>
        <span><b>{pct(cacheUtil)}</b> KV</span>
      </div>
    </div>
  );
};

/* ── Model row ─────────────────────────────────────────────── */

const ModelRow: React.FC<{ model: LoadedModel }> = ({ model }) => (
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
);

/* ══════════════════════════════════════════════════════════════
   ██  MAIN DASHBOARD
   ══════════════════════════════════════════════════════════════ */

const POLL_INTERVAL = 2000;

const Dashboard: React.FC = () => {
  const [health, setHealth] = useState<HealthData | null>(null);
  const [stats, setStats] = useState<StatsData | null>(null);
  const [sysStats, setSysStats] = useState<SystemStatsData | null>(null);
  const [slots, setSlots] = useState<SlotData[]>([]);
  const [history, setHistory] = useState<HistoryPoint[]>([]);
  const [pollCount, setPollCount] = useState(0);
  const [lastError, setLastError] = useState<string | null>(null);
  const [paused, setPaused] = useState(false);

  const countersRef = useRef<SessionCounters>(initCounters());
  const pollRef = useRef<ReturnType<typeof setInterval> | null>(null);

  /* ── Aggregate throughput from all slots ──────────────────── */

  const computeAggregates = useCallback((slotData: SlotData[]) => {
    const c = countersRef.current;

    let aggTps = 0;
    let aggPromptTps = 0;

    for (const s of slotData) {
      const t = s.timings || {} as SlotTimings;
      if (t.predicted_per_second > 0) aggTps += t.predicted_per_second;
      if (t.prompt_per_second > 0) aggPromptTps += t.prompt_per_second;
    }

    // Accumulate token deltas for session totals
    for (const s of slotData) {
      const prev = c.prevSlotTokens.get(s.id);
      const currDecoded = s.n_decoded || 0;
      const currPrompted = s.n_prompt_tokens_processed || 0;

      if (prev) {
        if (currDecoded > prev.decoded) c.totalTokensGenerated += currDecoded - prev.decoded;
        if (currPrompted > prev.prompted) c.totalPromptTokens += currPrompted - prev.prompted;
      } else {
        c.totalTokensGenerated += currDecoded;
        c.totalPromptTokens += currPrompted;
      }
      c.prevSlotTokens.set(s.id, { decoded: currDecoded, prompted: currPrompted });
    }

    if (aggTps > c.peakTps) c.peakTps = aggTps;
    if (aggPromptTps > c.peakPromptTps) c.peakPromptTps = aggPromptTps;

    return { aggTps, aggPromptTps };
  }, []);

  /* ── Polling ─────────────────────────────────────────────── */

  const poll = useCallback(async () => {
    try {
      const [h, st, ss] = await Promise.all([
        api.health().catch(() => null),
        api.stats().catch(() => null),
        api.systemStats().catch(() => null),
      ]);

      if (h) setHealth(h);
      if (st) setStats(st);
      if (ss) setSysStats(ss);

      let slotData: SlotData[] = [];
      if (h && h.all_models_loaded.some(m => m.recipe === 'llamacpp' || m.recipe === 'vllm')) {
        try { slotData = await api.slots(); } catch {}
      }
      setSlots(slotData);

      const { aggTps, aggPromptTps } = computeAggregates(slotData);

      const activeSlots = slotData.filter(s => s.is_processing).length;
      const totalSlots = slotData.length;
      const totalCache = slotData.reduce((a, s) => a + (s.cache_tokens?.length || 0), 0);
      const totalCtx = slotData.reduce((a, s) => a + s.n_ctx, 0);
      const cacheUtil = totalCtx > 0 ? (totalCache / totalCtx) * 100 : null;

      setHistory(prev => {
        const next = [...prev, {
          ts: Date.now(),
          cpu: ss?.cpu_percent ?? null,
          ram: ss?.memory_gb ?? null,
          gpu: ss?.gpu_percent ?? null,
          vram: ss?.vram_gb ?? null,
          npu: ss?.npu_percent ?? null,
          aggregateTps: aggTps,
          aggregatePromptTps: aggPromptTps,
          activeSlots,
          totalSlots,
          cacheUtil,
        }];
        return next.length > HISTORY_LEN ? next.slice(-HISTORY_LEN) : next;
      });

      setPollCount(c => c + 1);
      setLastError(null);
    } catch (err: any) {
      setLastError(err.message || 'Poll failed');
    }
  }, [computeAggregates]);

  useEffect(() => {
    poll();
    if (!paused) {
      pollRef.current = setInterval(poll, POLL_INTERVAL);
    }
    return () => { if (pollRef.current) clearInterval(pollRef.current); };
  }, [poll, paused]);

  /* ── Derived ─────────────────────────────────────────────── */

  const loadedModels = health?.all_models_loaded || [];
  const counters = countersRef.current;
  const latestTps = history.length > 0 ? history[history.length - 1].aggregateTps : 0;
  const latestPP = history.length > 0 ? history[history.length - 1].aggregatePromptTps : 0;
  const activeSlotCount = slots.filter(s => s.is_processing).length;
  const totalCacheTokens = slots.reduce((a, s) => a + (s.cache_tokens?.length || 0), 0);
  const totalCtx = slots.reduce((a, s) => a + s.n_ctx, 0);
  const overallCacheUtil = totalCtx > 0 ? (totalCacheTokens / totalCtx) * 100 : null;

  const hasGpu = sysStats?.gpu_percent != null && sysStats.gpu_percent >= 0;
  const hasNpu = sysStats?.npu_percent != null && sysStats.npu_percent >= 0;

  const modelsByType = useMemo(() => {
    const map: Record<string, LoadedModel[]> = {};
    for (const m of loadedModels) (map[m.type] = map[m.type] || []).push(m);
    return map;
  }, [loadedModels]);

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
            {paused ? '▶' : '⏸'}
          </button>
        </div>
      </header>

      {lastError && <div className="dash2-err">⚠ {lastError}</div>}

      <div className="dash2-scroll">
        {/* ═══ HERO — Aggregate Throughput ═══ */}
        <div className="dash2-card dash2-card--glow">
          <h2 className="dash2-card__h">⚡ Aggregate Throughput</h2>

          <div className="dash2-hero-row">
            <HeroStat value={latestTps} label="Generation Speed" unit="tok/s"
              peak={counters.peakTps} color="var(--accent)"
              secondary={`${fmtNum(counters.totalTokensGenerated)} total generated`} />
            <HeroStat value={latestPP} label="Prompt Processing" unit="tok/s"
              peak={counters.peakPromptTps} color="var(--info)"
              secondary={`${fmtNum(counters.totalPromptTokens)} total processed`} />
            <HeroStat value={activeSlotCount} label="Active Streams" unit={activeSlotCount === 1 ? 'stream' : 'streams'}
              color="var(--success)"
              secondary={`${slots.length} total slots`} />
          </div>

          <div className="dash2-charts">
            <AreaChart data={history.map(h => h.aggregateTps)} color="#e8c66b"
              label="Generation TPS" currentValue={latestTps > 0 ? `${latestTps.toFixed(1)} tok/s` : 'idle'}
              height={80} />
            <AreaChart data={history.map(h => h.aggregatePromptTps)} color="#7baed4"
              label="Prompt Processing" currentValue={latestPP > 0 ? `${latestPP.toFixed(0)} tok/s` : 'idle'}
              height={80} />
          </div>
        </div>

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
            <div className="dash2-charts">
              <AreaChart data={history.map(h => h.cpu)} color="#7fb38a"
                label="CPU" currentValue={pct(sysStats?.cpu_percent ?? null)} height={56} />
              {hasGpu && <AreaChart data={history.map(h => h.gpu)} color="#e8c66b"
                label="GPU" currentValue={pct(sysStats?.gpu_percent ?? null)} height={56} />}
            </div>
          </div>

          {stats ? (
            <div className="dash2-card">
              <h2 className="dash2-card__h">Last Inference</h2>
              <div className="dash2-inf-col">
                <div className="dash2-inf">
                  <span className="dash2-inf__v">{stats.tokens_per_second > 0 ? stats.tokens_per_second.toFixed(1) : '—'}</span>
                  <span className="dash2-inf__l">Tokens/sec</span>
                </div>
                <div className="dash2-inf">
                  <span className="dash2-inf__v">{stats.time_to_first_token > 0 ? `${(stats.time_to_first_token * 1000).toFixed(0)}` : '—'}</span>
                  <span className="dash2-inf__l">TTFT (ms)</span>
                </div>
                <div className="dash2-inf">
                  <span className="dash2-inf__v">{stats.input_tokens}</span>
                  <span className="dash2-inf__l">Prompt Tokens</span>
                </div>
                <div className="dash2-inf">
                  <span className="dash2-inf__v">{stats.output_tokens}</span>
                  <span className="dash2-inf__l">Completion Tokens</span>
                </div>
              </div>
              <div className="dash2-charts" style={{ marginTop: 'auto' }}>
                <AreaChart data={history.map(h => h.cacheUtil)} color="#d9a35b"
                  label="KV Cache" currentValue={overallCacheUtil != null ? `${overallCacheUtil.toFixed(0)}%` : '—'} height={56} />
              </div>
            </div>
          ) : (
            <div className="dash2-card">
              <h2 className="dash2-card__h">Last Inference</h2>
              <div className="dash2-empty">No inference data yet — send a request to see stats</div>
            </div>
          )}
        </div>

        {/* ═══ Parallel Slots ═══ */}
        {slots.length > 0 && (
          <div className="dash2-card">
            <h2 className="dash2-card__h">
              Parallel Slots
              <span className="dash2-card__badge">{activeSlotCount} / {slots.length} active</span>
            </h2>
            <div className="dash2-slots">
              {slots.map(s => <SlotMini key={s.id} slot={s} />)}
            </div>
          </div>
        )}

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
