import React, { useState, useEffect, useCallback, useRef, useMemo } from 'react';
import api, { HealthData, LoadedModel, StatsData, SystemStatsData, SlotData } from '../api';

/* ── History ring buffer ───────────────────────────────────── */

const HISTORY_LEN = 60;

interface HistoryPoint {
  ts: number;
  cpu: number | null;
  ram: number | null;
  gpu: number | null;
  vram: number | null;
  npu: number | null;
  tps: number | null;
  activeSlots: number;
  totalSlots: number;
  cacheUtil: number | null;
}

function emptyHistory(): HistoryPoint[] {
  return [];
}

/* ── Helpers ───────────────────────────────────────────────── */

function pct(value: number | null): string {
  if (value == null || value < 0) return '—';
  return `${Math.round(value)}%`;
}

function fmt(value: number | null, unit: string, decimals = 1): string {
  if (value == null || value < 0) return '—';
  return `${value.toFixed(decimals)} ${unit}`;
}

function relativeTime(ms: number): string {
  // Backend last_use may be uptime-relative (ms since process start) rather than Unix epoch.
  // If it looks like a valid recent Unix timestamp (after year 2020), use it directly.
  // Otherwise, treat it as an opaque value.
  const YEAR_2020_MS = 1577836800000;
  if (ms > YEAR_2020_MS) {
    const delta = Date.now() - ms;
    if (delta < 5000) return 'just now';
    if (delta < 60000) return `${Math.round(delta / 1000)}s ago`;
    if (delta < 3600000) return `${Math.round(delta / 60000)}m ago`;
    return `${Math.round(delta / 3600000)}h ago`;
  }
  // Uptime-relative: convert ms to human-readable duration
  const totalSec = Math.floor(ms / 1000);
  if (totalSec < 60) return `${totalSec}s uptime`;
  const m = Math.floor(totalSec / 60);
  if (m < 60) return `${m}m uptime`;
  const h = Math.floor(m / 60);
  if (h < 24) return `${h}h ${m % 60}m uptime`;
  const d = Math.floor(h / 24);
  return `${d}d ${h % 24}h uptime`;
}

function elapsed(startMs: number): string {
  const s = Math.floor((Date.now() - startMs) / 1000);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${sec}s`;
  return `${sec}s`;
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

function stateLabel(state: number): string {
  return state === 1 ? 'Processing' : 'Idle';
}

function stateClass(state: number): string {
  return state === 1 ? 'slot-state--active' : 'slot-state--idle';
}

/* ── Gauge component ───────────────────────────────────────── */

const Gauge: React.FC<{
  label: string;
  value: number | null;
  max?: number;
  unit?: string;
  color?: string;
  subtitle?: string;
}> = ({ label, value, max = 100, unit = '%', color, subtitle }) => {
  const safeVal = value != null && value >= 0 ? value : 0;
  const pctVal = Math.min(100, (safeVal / max) * 100);
  const unavailable = value == null || value < 0;

  // Color ramp: green → gold → red
  const autoColor = pctVal < 50 ? 'var(--success)' : pctVal < 80 ? 'var(--accent)' : 'var(--danger)';
  const ringColor = color || autoColor;

  return (
    <div className="dash-gauge" data-gauge={label.toLowerCase()}>
      <svg viewBox="0 0 36 36" className="dash-gauge__ring">
        <circle cx="18" cy="18" r="15.9" fill="none" stroke="var(--surface-3)" strokeWidth="2.5" />
        {!unavailable && (
          <circle
            cx="18" cy="18" r="15.9" fill="none"
            stroke={ringColor}
            strokeWidth="2.5"
            strokeDasharray={`${pctVal} ${100 - pctVal}`}
            strokeDashoffset="25"
            strokeLinecap="round"
            style={{ transition: 'stroke-dasharray 0.6s ease-out' }}
          />
        )}
      </svg>
      <div className="dash-gauge__inner">
        <span className="dash-gauge__value">
          {unavailable ? '—' : (unit === '%' ? `${Math.round(safeVal)}` : safeVal.toFixed(1))}
        </span>
        <span className="dash-gauge__unit">{unavailable ? '' : unit}</span>
      </div>
      <span className="dash-gauge__label">{label}</span>
      {subtitle && <span className="dash-gauge__sub">{subtitle}</span>}
    </div>
  );
};

/* ── Sparkline component ───────────────────────────────────── */

const Sparkline: React.FC<{
  data: (number | null)[];
  width?: number;
  height?: number;
  color?: string;
  label?: string;
}> = ({ data, width = 120, height = 32, color = 'var(--accent)', label }) => {
  const filtered = data.filter((v): v is number => v != null && v >= 0);
  if (filtered.length < 2) {
    return (
      <div className="dash-spark" style={{ width, height }}>
        {label && <span className="dash-spark__label">{label}</span>}
        <span className="dash-spark__empty">waiting…</span>
      </div>
    );
  }
  const max = Math.max(...filtered, 1);
  const step = width / (filtered.length - 1);
  const points = filtered.map((v, i) => `${i * step},${height - (v / max) * (height - 4) - 2}`).join(' ');

  return (
    <div className="dash-spark" style={{ width }}>
      {label && <span className="dash-spark__label">{label}</span>}
      <svg width={width} height={height} className="dash-spark__svg">
        <polyline
          points={points}
          fill="none"
          stroke={color}
          strokeWidth="1.5"
          strokeLinejoin="round"
          strokeLinecap="round"
        />
      </svg>
    </div>
  );
};

/* ── Slot card ─────────────────────────────────────────────── */

const SlotCard: React.FC<{ slot: SlotData }> = ({ slot }) => {
  const cacheLen = slot.cache_tokens?.length || 0;
  const cacheUtil = slot.n_ctx > 0 ? (cacheLen / slot.n_ctx) * 100 : 0;
  const timings = slot.timings || {} as SlotTimings;

  return (
    <div className={`dash-slot ${slot.is_processing ? 'dash-slot--active' : ''}`}
      data-slot-id={slot.id}>
      <div className="dash-slot__head">
        <span className="dash-slot__id">Slot {slot.id}</span>
        <span className={`dash-slot__state ${stateClass(slot.state)}`}>
          <span className="dash-slot__state-dot" />
          {stateLabel(slot.state)}
        </span>
      </div>

      {/* KV cache bar */}
      <div className="dash-slot__cache">
        <div className="dash-slot__cache-bar">
          <div
            className="dash-slot__cache-fill"
            style={{
              width: `${Math.min(100, cacheUtil)}%`,
              background: cacheUtil > 80 ? 'var(--danger)' : cacheUtil > 50 ? 'var(--accent)' : 'var(--success)',
            }}
          />
        </div>
        <span className="dash-slot__cache-label">
          KV {cacheLen.toLocaleString()} / {slot.n_ctx.toLocaleString()} ({cacheUtil.toFixed(0)}%)
        </span>
      </div>

      {/* Metrics */}
      <div className="dash-slot__metrics">
        <div className="dash-slot__metric">
          <span className="dash-slot__metric-label">Prompt</span>
          <span className="dash-slot__metric-value">{slot.n_prompt_tokens_processed}</span>
        </div>
        <div className="dash-slot__metric">
          <span className="dash-slot__metric-label">Decoded</span>
          <span className="dash-slot__metric-value">{slot.n_decoded}</span>
        </div>
        {timings.predicted_per_second > 0 && (
          <div className="dash-slot__metric">
            <span className="dash-slot__metric-label">TPS</span>
            <span className="dash-slot__metric-value">
              {timings.predicted_per_second.toFixed(1)}
            </span>
          </div>
        )}
        {timings.prompt_per_second > 0 && (
          <div className="dash-slot__metric">
            <span className="dash-slot__metric-label">Prompt/s</span>
            <span className="dash-slot__metric-value">
              {timings.prompt_per_second.toFixed(0)}
            </span>
          </div>
        )}
      </div>

      {/* Sampling params */}
      <div className="dash-slot__params">
        temp {(slot.temperature ?? 0).toFixed(2)} · top_p {(slot.top_p ?? 0).toFixed(2)} · top_k {slot.top_k ?? 0}
      </div>
    </div>
  );
};

/* ── Main Dashboard ────────────────────────────────────────── */

const POLL_INTERVAL = 2500;

const Dashboard: React.FC = () => {
  const [health, setHealth] = useState<HealthData | null>(null);
  const [stats, setStats] = useState<StatsData | null>(null);
  const [sysStats, setSysStats] = useState<SystemStatsData | null>(null);
  const [slots, setSlots] = useState<SlotData[]>([]);
  const [history, setHistory] = useState<HistoryPoint[]>(emptyHistory);
  const [pollCount, setPollCount] = useState(0);
  const [lastError, setLastError] = useState<string | null>(null);
  const [paused, setPaused] = useState(false);

  const startTime = useRef(Date.now());
  const pollRef = useRef<ReturnType<typeof setInterval> | null>(null);

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

      // Try slots if any llm models are loaded
      let slotData: SlotData[] = [];
      if (h && h.all_models_loaded.some(m => m.recipe === 'llamacpp' || m.recipe === 'vllm')) {
        try {
          slotData = await api.slots();
        } catch {}
      }
      setSlots(slotData);

      // Build history point
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
          tps: st?.tokens_per_second ?? null,
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
  }, []);

  useEffect(() => {
    poll(); // initial
    if (!paused) {
      pollRef.current = setInterval(poll, POLL_INTERVAL);
    }
    return () => {
      if (pollRef.current) clearInterval(pollRef.current);
    };
  }, [poll, paused]);

  /* ── Derived stats ───────────────────────────────────────── */

  const loadedModels = health?.all_models_loaded || [];
  const modelsByType = useMemo(() => {
    const map: Record<string, LoadedModel[]> = {};
    for (const m of loadedModels) {
      (map[m.type] = map[m.type] || []).push(m);
    }
    return map;
  }, [loadedModels]);

  const activeSlotCount = slots.filter(s => s.is_processing).length;
  const idleSlotCount = slots.filter(s => !s.is_processing).length;
  const totalCacheTokens = slots.reduce((a, s) => a + (s.cache_tokens?.length || 0), 0);
  const totalCtx = slots.reduce((a, s) => a + s.n_ctx, 0);
  const overallCacheUtil = totalCtx > 0 ? (totalCacheTokens / totalCtx) * 100 : null;
  const cachedSlots = slots.filter(s => (s.cache_tokens?.length || 0) > 0).length;

  const hasGpu = sysStats?.gpu_percent != null && sysStats.gpu_percent >= 0;
  const hasVram = sysStats?.vram_gb != null && sysStats.vram_gb >= 0;
  const hasNpu = sysStats?.npu_percent != null && sysStats.npu_percent >= 0;

  /* ── Render ──────────────────────────────────────────────── */

  return (
    <section className="dashboard" data-view="dashboard">
      {/* ── Status bar ──────────────────────────────────── */}
      <div className="dash-status">
        <div className="dash-status__left">
          <span className="dash-status__dot" data-connected={!!health} />
          <span className="dash-status__text">
            {health ? `Lemonade ${health.version}` : 'Disconnected'}
          </span>
          {health && (
            <span className="dash-status__uptime">
              uptime {elapsed(startTime.current)}
            </span>
          )}
        </div>
        <div className="dash-status__right">
          {health?.websocket_port && (
            <span className="dash-status__meta">WS :{health.websocket_port}</span>
          )}
          <span className="dash-status__meta">poll #{pollCount}</span>
          <button
            className={`dash-status__pause ${paused ? 'is-paused' : ''}`}
            onClick={() => setPaused(p => !p)}
            title={paused ? 'Resume polling' : 'Pause polling'}>
            {paused ? '▶' : '⏸'}
          </button>
        </div>
      </div>

      {lastError && (
        <div className="dash-error">
          <span>⚠ {lastError}</span>
        </div>
      )}

      <div className="dash-body">
        {/* ── Section 1: System resources ────────────────── */}
        <div className="dash-section">
          <h2 className="dash-section__title">System Resources</h2>
          <div className="dash-gauges">
            <Gauge label="CPU" value={sysStats?.cpu_percent ?? null} subtitle={pct(sysStats?.cpu_percent ?? null)} />
            <Gauge label="RAM" value={sysStats?.memory_gb ?? null} max={64} unit="GB"
              color="var(--info)" subtitle={fmt(sysStats?.memory_gb ?? null, 'GB')} />
            {hasGpu && <Gauge label="GPU" value={sysStats!.gpu_percent!} subtitle={pct(sysStats!.gpu_percent)} />}
            {hasVram && <Gauge label="VRAM" value={sysStats!.vram_gb!} max={32} unit="GB"
              color="var(--info)" subtitle={fmt(sysStats!.vram_gb, 'GB')} />}
            {hasNpu && <Gauge label="NPU" value={sysStats!.npu_percent!} subtitle={pct(sysStats!.npu_percent)} />}
          </div>
          <div className="dash-sparks">
            <Sparkline data={history.map(h => h.cpu)} label="CPU %" color="var(--success)" />
            <Sparkline data={history.map(h => h.ram)} label="RAM GB" color="var(--info)" />
            {hasGpu && <Sparkline data={history.map(h => h.gpu)} label="GPU %" color="var(--accent)" />}
            <Sparkline data={history.map(h => h.tps)} label="TPS" color="var(--warn)" />
          </div>
        </div>

        {/* ── Section 2: Session overview ────────────────── */}
        <div className="dash-section">
          <h2 className="dash-section__title">Session Overview</h2>
          <div className="dash-kpis">
            <div className="dash-kpi" data-kpi="loaded">
              <span className="dash-kpi__value">{loadedModels.length}</span>
              <span className="dash-kpi__label">Models Loaded</span>
            </div>
            <div className="dash-kpi" data-kpi="slots">
              <span className="dash-kpi__value">{slots.length}</span>
              <span className="dash-kpi__label">Total Slots</span>
            </div>
            <div className="dash-kpi dash-kpi--accent" data-kpi="active">
              <span className="dash-kpi__value">{activeSlotCount}</span>
              <span className="dash-kpi__label">Active Sessions</span>
            </div>
            <div className="dash-kpi" data-kpi="idle">
              <span className="dash-kpi__value">{idleSlotCount}</span>
              <span className="dash-kpi__label">Idle Slots</span>
            </div>
            <div className="dash-kpi" data-kpi="cached">
              <span className="dash-kpi__value">{cachedSlots}</span>
              <span className="dash-kpi__label">Cached Contexts</span>
            </div>
            <div className="dash-kpi" data-kpi="cache-util">
              <span className="dash-kpi__value">
                {overallCacheUtil != null ? `${overallCacheUtil.toFixed(0)}%` : '—'}
              </span>
              <span className="dash-kpi__label">KV Cache Usage</span>
            </div>
          </div>
        </div>

        {/* ── Section 3: Inference metrics ───────────────── */}
        {stats && (
          <div className="dash-section">
            <h2 className="dash-section__title">Last Inference</h2>
            <div className="dash-kpis dash-kpis--inference">
              <div className="dash-kpi">
                <span className="dash-kpi__value">
                  {stats.tokens_per_second > 0 ? stats.tokens_per_second.toFixed(1) : '—'}
                </span>
                <span className="dash-kpi__label">Tokens/sec</span>
              </div>
              <div className="dash-kpi">
                <span className="dash-kpi__value">
                  {stats.time_to_first_token > 0 ? `${(stats.time_to_first_token * 1000).toFixed(0)}ms` : '—'}
                </span>
                <span className="dash-kpi__label">TTFT</span>
              </div>
              <div className="dash-kpi">
                <span className="dash-kpi__value">{stats.input_tokens}</span>
                <span className="dash-kpi__label">Prompt Tokens</span>
              </div>
              <div className="dash-kpi">
                <span className="dash-kpi__value">{stats.output_tokens}</span>
                <span className="dash-kpi__label">Completion Tokens</span>
              </div>
            </div>
          </div>
        )}

        {/* ── Section 4: Loaded models ──────────────────── */}
        <div className="dash-section">
          <h2 className="dash-section__title">
            Loaded Models
            {loadedModels.length > 0 && (
              <span className="dash-section__count">{loadedModels.length}</span>
            )}
          </h2>
          {loadedModels.length === 0 ? (
            <div className="dash-empty">No models loaded</div>
          ) : (
            <div className="dash-models">
              {loadedModels.map(m => (
                <div className="dash-model" key={m.model_name} data-model={m.model_name}>
                  <div className="dash-model__head">
                    <span className="dash-model__icon">{typeIcon(m.type)}</span>
                    <span className="dash-model__name">{m.model_name}</span>
                  </div>
                  <div className="dash-model__meta">
                    <span className="dash-model__chip dash-model__chip--type">{m.type}</span>
                    <span className="dash-model__chip dash-model__chip--device">{m.device}</span>
                    <span className="dash-model__chip dash-model__chip--recipe">{m.recipe}</span>
                  </div>
                  <div className="dash-model__details">
                    <span>PID {m.pid}</span>
                    <span>{m.backend_url}</span>
                    <span>Last use: {relativeTime(m.last_use)}</span>
                  </div>
                </div>
              ))}
            </div>
          )}
        </div>

        {/* ── Section 5: Parallel slots ─────────────────── */}
        {slots.length > 0 && (
          <div className="dash-section">
            <h2 className="dash-section__title">
              Parallel Slots
              <span className="dash-section__count">
                {activeSlotCount} active / {slots.length} total
              </span>
            </h2>
            <div className="dash-slots">
              {slots.map(s => <SlotCard key={s.id} slot={s} />)}
            </div>
            <div className="dash-sparks" style={{ marginTop: 'var(--space-4)' }}>
              <Sparkline data={history.map(h => h.activeSlots)} label="Active" color="var(--success)" />
              <Sparkline data={history.map(h => h.cacheUtil)} label="KV Cache %" color="var(--accent)" />
            </div>
          </div>
        )}

        {/* ── Section 6: Model capacity ─────────────────── */}
        {health?.max_models && (
          <div className="dash-section">
            <h2 className="dash-section__title">Model Capacity</h2>
            <div className="dash-capacity">
              {Object.entries(health.max_models).map(([type, max]) => {
                const loaded = modelsByType[type]?.length || 0;
                const pctUsed = max > 0 ? (loaded / max) * 100 : 0;
                return (
                  <div className="dash-capacity__row" key={type}>
                    <span className="dash-capacity__type">{typeIcon(type)} {type}</span>
                    <div className="dash-capacity__bar">
                      <div
                        className="dash-capacity__fill"
                        style={{
                          width: `${Math.min(100, pctUsed)}%`,
                          background: pctUsed >= 100 ? 'var(--danger)' : 'var(--accent)',
                        }}
                      />
                    </div>
                    <span className="dash-capacity__label">{loaded} / {max}</span>
                  </div>
                );
              })}
            </div>
          </div>
        )}
      </div>
    </section>
  );
};

export default Dashboard;
