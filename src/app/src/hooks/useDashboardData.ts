import { useState, useEffect, useCallback, useRef, useMemo } from 'react';
import api, { HealthData, StatsData, SystemStatsData, SlotData, LoadedModel, LogStreamHandle, getCacheTokenCount, friendlyErrorMessage } from '../api';

/* ── History ring buffer ───────────────────────────────────── */

export const HISTORY_LEN = 300;    // 300 × 200ms tick = 60s visible window
const TICK_MS = 200;               // Interpolation ticker interval
const LERP_SPEED = 0.15;          // Per-tick lerp factor toward target

export interface SlotHistoryPoint {
  ts: number;
  tps: number;
  ppTps: number;
  decoded: number;
  cacheUtil: number;
  isActive: boolean;
}

export interface HistoryPoint {
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

export interface SessionCounters {
  totalTokensGenerated: number;
  totalPromptTokens: number;
  peakTps: number;
  peakPromptTps: number;
  sessionStart: number;
  prevSlotTokens: Map<number, { decoded: number; prompted: number; ts: number }>;
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

/** Per-slot live TPS computed from token count deltas between polls */
export interface SlotLiveTps {
  tps: number;
  ppTps: number;
  isActive: boolean;
}

/* ── Smoothing ─────────────────────────────────────────────── */

const WINDOW_MS = 5000;     // 5-second sliding window for TPS averaging

const ATTACK_ALPHA = 1.0;
const RELEASE_ALPHA = 0.5;
function smoothTarget(current: number, raw: number): number {
  const alpha = raw >= current ? ATTACK_ALPHA : RELEASE_ALPHA;
  const result = current + (raw - current) * alpha;
  return result < 0.5 ? 0 : result;
}

/** Parse llama.cpp slot print_timing log lines for real-time throughput. */
function parseTimingLine(line: string): { slotId: number; decoded?: number; tg?: number; pp?: number } | null {
  const m = line.match(/slot\s+print_timing:\s*id\s+(\d+)/);
  if (!m) return null;
  const slotId = parseInt(m[1], 10);
  const decodedM = line.match(/n_decoded\s*=\s*(\d+)/);
  const tgM = line.match(/tg\s*=\s*([\d.]+)\s*t\/s/);
  const ppM = line.match(/pp\s*=\s*([\d.]+)\s*t\/s/);
  if (!tgM && !ppM) return null;
  return {
    slotId,
    decoded: decodedM ? parseInt(decodedM[1], 10) : undefined,
    tg: tgM ? parseFloat(tgM[1]) : undefined,
    pp: ppM ? parseFloat(ppM[1]) : undefined,
  };
}

/* ── Hook return type ──────────────────────────────────────── */

export interface SlotTarget {
  tps: number;
  ppTps: number;
  decoded: number;
  cacheUtil: number;
  isActive: boolean;
}

export interface DashboardData {
  health: HealthData | null;
  stats: StatsData | null;
  sysStats: SystemStatsData | null;
  slots: SlotData[];
  slotLive: Record<number, SlotLiveTps>;
  lastError: string | null;
  slotsUnsupported: boolean;
  slotStatus: string;
  paused: boolean;
  setPaused: React.Dispatch<React.SetStateAction<boolean>>;
  counters: SessionCounters;
  getSlotTarget: (slotId: number) => SlotTarget | undefined;
  loadedModels: LoadedModel[];
  latestTps: number;
  latestPP: number;
  activeSlotCount: number;
  overallCacheUtil: number | null;
  hasGpu: boolean;
  hasNpu: boolean;
  modelsByType: Record<string, LoadedModel[]>;
  aggChartData: Record<string, number>[];
  slotChartData: Record<string, number>[];
  sysChartData: Record<string, number>[];
  cacheChartData: Record<string, number>[];
}

/* ── Hook ──────────────────────────────────────────────────── */

const POLL_INTERVAL = 2000;

export function useDashboardData(): DashboardData {
  const [health, setHealth] = useState<HealthData | null>(null);
  const [stats, setStats] = useState<StatsData | null>(null);
  const [sysStats, setSysStats] = useState<SystemStatsData | null>(null);
  const [slots, setSlots] = useState<SlotData[]>([]);
  const [history, setHistory] = useState<HistoryPoint[]>([]);
  const [slotHistory, setSlotHistory] = useState<Record<number, SlotHistoryPoint[]>>({});
  const [slotLive, setSlotLive] = useState<Record<number, SlotLiveTps>>({});
  const [lastError, setLastError] = useState<string | null>(null);
  const [slotsUnsupported, setSlotsUnsupported] = useState(false);
  const [slotStatus, setSlotStatus] = useState('Slot telemetry is waiting for a compatible backend.');
  const [paused, setPaused] = useState(false);

  const countersRef = useRef<SessionCounters>(initCounters());
  const pollRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const failureCountRef = useRef(0);
  const slotWindowRef = useRef(new Map<number, { ts: number; decoded: number; prompted: number }[]>());
  const targetAggRef = useRef({
    tps: 0, ppTps: 0,
    cpu: null as number | null, ram: null as number | null,
    gpu: null as number | null, vram: null as number | null, npu: null as number | null,
    activeSlots: 0, totalSlots: 0, cacheUtil: null as number | null,
  });
  const targetSlotRef = useRef(new Map<number, SlotTarget>());
  const interpAggRef = useRef({ tps: 0, ppTps: 0 });
  const interpSlotRef = useRef(new Map<number, { tps: number; ppTps: number }>());

  /* ── Aggregate throughput from all slots ──────────────────── */

  const computeAggregates = useCallback((slotData: SlotData[]) => {
    const c = countersRef.current;
    const now = Date.now();

    let aggTps = 0;
    let aggPromptTps = 0;
    const perSlotLive: Map<number, SlotLiveTps> = new Map();
    const cutoff = now - WINDOW_MS;

    for (const s of slotData) {
      const currDecoded = s.n_decoded || 0;
      const currPrompted = s.n_prompt_tokens_processed || 0;
      const prev = c.prevSlotTokens.get(s.id);

      c.prevSlotTokens.set(s.id, { decoded: currDecoded, prompted: currPrompted, ts: now });

      const winMap = slotWindowRef.current;
      let samples = winMap.get(s.id) || [];
      if (prev && currDecoded < prev.decoded) samples = [];
      samples.push({ ts: now, decoded: currDecoded, prompted: currPrompted });
      samples = samples.filter(p => p.ts >= cutoff);
      winMap.set(s.id, samples);

      let slotTps = 0;
      let slotPpTps = 0;
      if (samples.length >= 2) {
        const oldest = samples[0];
        const newest = samples[samples.length - 1];
        const spanSec = (newest.ts - oldest.ts) / 1000;
        if (spanSec > 0.5) {
          slotTps = Math.max(0, newest.decoded - oldest.decoded) / spanSec;
          slotPpTps = Math.max(0, newest.prompted - oldest.prompted) / spanSec;
        }
      }

      // Fallback: use llama.cpp's own prompt_per_second when our delta is 0
      // (n_prompt_tokens_processed can reset between requests in some versions)
      if (slotPpTps < 0.05 && s.is_processing && s.timings?.prompt_per_second > 0) {
        slotPpTps = s.timings.prompt_per_second;
      }

      const slotActive = slotTps > 0.05 || s.is_processing;
      perSlotLive.set(s.id, { tps: slotTps, ppTps: slotPpTps, isActive: slotActive });

      aggTps += slotTps;
      aggPromptTps += slotPpTps;
    }

    if (aggTps > c.peakTps) c.peakTps = aggTps;
    if (aggPromptTps > c.peakPromptTps) c.peakPromptTps = aggPromptTps;

    return { aggTps, aggPromptTps, perSlotLive };
  }, []);

  /* ── Polling ─────────────────────────────────────────────── */

  const poll = useCallback(async () => {
    try {
      let healthError: unknown = null;
      const [h, st, ss] = await Promise.all([
        api.health().catch((err) => { healthError = err; return null; }),
        api.stats().catch(() => null),
        api.systemStats().catch(() => null),
      ]);

      if (!h) throw healthError || new Error(api.lastConnectionError || 'Server health endpoint is unavailable.');
      failureCountRef.current = 0;

      setHealth(h);
      if (st) setStats(st);
      if (ss) setSysStats(ss);

      let slotData: SlotData[] = [];
      const loaded = Array.isArray(h.all_models_loaded) ? h.all_models_loaded : [];
      const supportsSlots = loaded.some(m => m.recipe === 'llamacpp' || m.recipe === 'vllm');
      if (supportsSlots) {
        try {
          const response = await api.slots();
          slotData = Array.isArray(response) ? response : [];
          setSlotsUnsupported(false);
          setSlotStatus(slotData.length > 0 ? 'Slot telemetry is live.' : 'Compatible backend loaded, but no slot activity is currently reported.');
        } catch (err) {
          slotData = [];
          setSlotsUnsupported(true);
          setSlotStatus(`Slot telemetry unavailable: ${friendlyErrorMessage(err)}`);
        }
      } else {
        setSlotsUnsupported(loaded.length > 0);
        setSlotStatus(loaded.length > 0
          ? 'Current backend does not expose llama.cpp/vLLM slot telemetry.'
          : 'Load a llama.cpp or vLLM chat model to see slot telemetry.');
      }
      setSlots(slotData);

      const { aggTps, aggPromptTps, perSlotLive } = computeAggregates(slotData);

      const liveObj: Record<number, SlotLiveTps> = {};
      for (const [id, v] of perSlotLive) liveObj[id] = v;
      setSlotLive(liveObj);

      for (const s of slotData) {
        const live = perSlotLive.get(s.id);
        const cacheLen = getCacheTokenCount(s);
        const cu = s.n_ctx > 0 ? (cacheLen / s.n_ctx) * 100 : 0;
        const prev = targetSlotRef.current.get(s.id);
        targetSlotRef.current.set(s.id, {
          tps: smoothTarget(prev?.tps || 0, live?.tps || 0),
          ppTps: smoothTarget(prev?.ppTps || 0, live?.ppTps || 0),
          decoded: s.n_decoded || 0,
          cacheUtil: cu,
          isActive: live?.isActive || s.is_processing,
        });
      }

      const activeSlots = slotData.filter(s => {
        const live = perSlotLive.get(s.id);
        return live?.isActive || s.is_processing;
      }).length;
      const totalSlots = slotData.length;
      const totalCache = slotData.reduce((a, s) => a + getCacheTokenCount(s), 0);
      const totalCtx = slotData.reduce((a, s) => a + s.n_ctx, 0);
      const cacheUtil = totalCtx > 0 ? (totalCache / totalCtx) * 100 : null;

      const tgt = targetAggRef.current;
      tgt.tps = smoothTarget(tgt.tps, aggTps);
      tgt.ppTps = smoothTarget(tgt.ppTps, aggPromptTps);
      tgt.cpu = ss?.cpu_percent ?? null;
      tgt.ram = ss?.memory_gb ?? null;
      tgt.gpu = ss?.gpu_percent ?? null;
      tgt.vram = ss?.vram_gb ?? null;
      tgt.npu = ss?.npu_percent ?? null;
      tgt.activeSlots = activeSlots;
      tgt.totalSlots = totalSlots;
      tgt.cacheUtil = cacheUtil;

      setLastError(null);
    } catch (err) {
      failureCountRef.current += 1;
      setLastError(`${friendlyErrorMessage(err)}${failureCountRef.current >= 3 ? ' Polling paused after repeated failures.' : ''}`);
      setSlots([]);
      setSlotLive({});
      if (failureCountRef.current >= 3) setPaused(true);
    }
  }, [computeAggregates]);

  useEffect(() => {
    poll();
    if (!paused) {
      pollRef.current = setInterval(poll, POLL_INTERVAL);
    }
    return () => { if (pollRef.current) clearInterval(pollRef.current); };
  }, [poll, paused]);

  /* ── Live throughput from log WebSocket ───────────────────── */

  const liveSlotTps = useRef(new Map<number, { tg: number; pp: number }>());
  const lastLivePushRef = useRef(0);
  const pausedRef = useRef(paused);
  pausedRef.current = paused;
  const wsHandleRef = useRef<LogStreamHandle | null>(null);
  const wsReconnectRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  useEffect(() => {
    const wsPort = health?.websocket_port;
    if (!wsPort) return;
    let cancelled = false;

    const scheduleReconnect = () => {
      if (cancelled || pausedRef.current) return;
      if (wsReconnectRef.current) clearTimeout(wsReconnectRef.current);
      wsReconnectRef.current = setTimeout(connectWs, 5000);
    };

    const connectWs = () => {
      if (cancelled || pausedRef.current) return;
      if (wsReconnectRef.current) {
        clearTimeout(wsReconnectRef.current);
        wsReconnectRef.current = null;
      }
      if (wsHandleRef.current) wsHandleRef.current.close();

      const handle = api.connectLogStream({
        onEntry: (entry) => {
          if (pausedRef.current) return;
          const t = parseTimingLine(entry.line);
          if (!t) return;

          const map = liveSlotTps.current;
          map.set(t.slotId, { tg: t.tg ?? 0, pp: t.pp ?? 0 });

          const c = countersRef.current;
          if (t.decoded != null && t.decoded > 0) {
            c.totalTokensGenerated += t.decoded;
          }

          const now = Date.now();
          if (now - lastLivePushRef.current < 1000) return;
          lastLivePushRef.current = now;

          let aggTps = 0, aggPp = 0;
          for (const v of map.values()) {
            aggTps += v.tg;
            aggPp += v.pp;
          }

          if (aggTps > c.peakTps) c.peakTps = aggTps;
          if (aggPp > c.peakPromptTps) c.peakPromptTps = aggPp;

          const tgt = targetAggRef.current;
          tgt.tps = smoothTarget(tgt.tps, aggTps);
          tgt.ppTps = smoothTarget(tgt.ppTps, aggPp);

          for (const [slotId, v] of map) {
            const existing = targetSlotRef.current.get(slotId);
            if (existing) {
              existing.tps = smoothTarget(existing.tps, v.tg);
              existing.ppTps = smoothTarget(existing.ppTps, v.pp);
              existing.isActive = v.tg > 0.05 || v.pp > 0.05;
            }
          }
        },
        onDisconnected: () => {
          wsHandleRef.current = null;
          scheduleReconnect();
        },
      });

      wsHandleRef.current = handle;
    };

    connectWs();

    return () => {
      cancelled = true;
      if (wsReconnectRef.current) clearTimeout(wsReconnectRef.current);
      if (wsHandleRef.current) wsHandleRef.current.close();
      wsReconnectRef.current = null;
      wsHandleRef.current = null;
    };
  }, [health?.websocket_port, paused]);

  /* ── Interpolation ticker — smooth 200ms chart updates ───── */

  useEffect(() => {
    if (paused) return;

    const ticker = setInterval(() => {
      const aTarget = targetAggRef.current;
      const aI = interpAggRef.current;

      aI.tps += (aTarget.tps - aI.tps) * LERP_SPEED;
      aI.ppTps += (aTarget.ppTps - aI.ppTps) * LERP_SPEED;
      if (Math.abs(aI.tps) < 0.05) aI.tps = 0;
      if (Math.abs(aI.ppTps) < 0.05) aI.ppTps = 0;

      setHistory(prev => {
        const point: HistoryPoint = {
          ts: Date.now(),
          cpu: aTarget.cpu,
          ram: aTarget.ram,
          gpu: aTarget.gpu,
          vram: aTarget.vram,
          npu: aTarget.npu,
          aggregateTps: aI.tps,
          aggregatePromptTps: aI.ppTps,
          activeSlots: aTarget.activeSlots,
          totalSlots: aTarget.totalSlots,
          cacheUtil: aTarget.cacheUtil,
        };
        const next = [...prev, point];
        return next.length > HISTORY_LEN ? next.slice(-HISTORY_LEN) : next;
      });

      const sI = interpSlotRef.current;
      const sT = targetSlotRef.current;

      setSlotHistory(prev => {
        const next = { ...prev };
        for (const [slotId, target] of sT) {
          const interp = sI.get(slotId) || { tps: 0, ppTps: 0 };
          interp.tps += (target.tps - interp.tps) * LERP_SPEED;
          interp.ppTps += (target.ppTps - interp.ppTps) * LERP_SPEED;
          if (Math.abs(interp.tps) < 0.05) interp.tps = 0;
          if (Math.abs(interp.ppTps) < 0.05) interp.ppTps = 0;
          sI.set(slotId, interp);

          const point: SlotHistoryPoint = {
            ts: Date.now(),
            tps: interp.tps,
            ppTps: interp.ppTps,
            decoded: target.decoded,
            cacheUtil: target.cacheUtil,
            isActive: target.isActive,
          };
          const arr = next[slotId] || [];
          next[slotId] = arr.length >= HISTORY_LEN ? [...arr.slice(-HISTORY_LEN + 1), point] : [...arr, point];
        }
        return next;
      });
    }, TICK_MS);

    return () => clearInterval(ticker);
  }, [paused]);

  /* ── Derived ─────────────────────────────────────────────── */

  const loadedModels = health?.all_models_loaded || [];
  const counters = countersRef.current;
  const latestTps = history.length > 0 ? history[history.length - 1].aggregateTps : 0;
  const latestPP = history.length > 0 ? history[history.length - 1].aggregatePromptTps : 0;
  const activeSlotCount = slots.filter(s => {
    const live = slotLive[s.id];
    const target = targetSlotRef.current.get(s.id);
    return live?.isActive || target?.isActive || s.is_processing;
  }).length;
  const totalCacheTokens = slots.reduce((a, s) => a + getCacheTokenCount(s), 0);
  const totalCtx = slots.reduce((a, s) => a + s.n_ctx, 0);
  const overallCacheUtil = totalCtx > 0 ? (totalCacheTokens / totalCtx) * 100 : null;

  const hasGpu = sysStats?.gpu_percent != null && sysStats.gpu_percent >= 0;
  const hasNpu = sysStats?.npu_percent != null && sysStats.npu_percent >= 0;

  const modelsByType = useMemo(() => {
    const map: Record<string, LoadedModel[]> = {};
    for (const m of loadedModels) (map[m.type] = map[m.type] || []).push(m);
    return map;
  }, [loadedModels]);

  const aggChartData = useMemo(() =>
    history.map(h => ({ genTps: h.aggregateTps, ppTps: h.aggregatePromptTps })),
    [history]
  );

  const slotChartData = useMemo(() => {
    if (slots.length === 0) return [];
    // Use aggregate history length as the timeline — pad slot histories
    // with leading zeros so both charts align horizontally
    const timelineLen = history.length;
    if (timelineLen === 0) return [];
    const data: Record<string, number>[] = [];
    for (let i = 0; i < timelineLen; i++) {
      const point: Record<string, number> = {};
      for (const s of slots) {
        const hist = slotHistory[s.id] || [];
        const offset = timelineLen - hist.length;
        point[`slot${s.id}`] = i >= offset ? (hist[i - offset]?.tps || 0) : 0;
      }
      data.push(point);
    }
    return data;
  }, [slots, slotHistory, history.length]);

  const sysChartData = useMemo(() =>
    history.map(h => ({ cpu: h.cpu ?? 0, gpu: h.gpu ?? 0 })),
    [history]
  );

  const cacheChartData = useMemo(() =>
    history.map(h => ({ cache: h.cacheUtil ?? 0 })),
    [history]
  );

  const getSlotTarget = useCallback((slotId: number) =>
    targetSlotRef.current.get(slotId),
  []);

  return {
    health, stats, sysStats, slots, slotLive,
    lastError, slotsUnsupported, slotStatus, paused, setPaused,
    counters, getSlotTarget, loadedModels,
    latestTps, latestPP, activeSlotCount, overallCacheUtil,
    hasGpu, hasNpu, modelsByType,
    aggChartData, slotChartData, sysChartData, cacheChartData,
  };
}
