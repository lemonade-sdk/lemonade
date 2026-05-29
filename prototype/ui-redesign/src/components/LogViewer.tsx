import React, { useState, useEffect, useLayoutEffect, useRef, useCallback, useMemo } from 'react';
import api, { LogEntry, LogStreamHandle } from '../api';

/* ── Constants ─────────────────────────────────────────────── */

const MAX_CLIENT_LOGS = 2000;
const RECONNECT_DELAY = 5000;
const BOTTOM_THRESHOLD = 60;
const LINE_HEIGHT = 22;
const OVERSCAN = 10;
const BATCH_INTERVAL = 100; // ms — batch incoming entries

const LOG_LEVELS = ['trace', 'debug', 'info', 'warning', 'error', 'fatal'] as const;
type LogLevel = typeof LOG_LEVELS[number];

const SEVERITY_PRIORITY: Record<string, number> = {
  trace: 0, Trace: 0,
  debug: 1, Debug: 1,
  info: 2, Info: 2, notice: 2, Notice: 2,
  warning: 3, warn: 3, Warning: 3, Warn: 3,
  error: 4, Error: 4,
  fatal: 5, Fatal: 5,
};

function severityClass(severity: string): string {
  const s = severity.toLowerCase();
  if (s === 'trace') return 'log-trace';
  if (s === 'debug') return 'log-debug';
  if (s === 'info' || s === 'notice') return 'log-info';
  if (s === 'warning' || s === 'warn') return 'log-warn';
  if (s === 'error') return 'log-error';
  if (s === 'fatal') return 'log-fatal';
  return 'log-info';
}

function severityBadge(severity: string): string {
  const s = severity.toLowerCase();
  if (s === 'trace') return 'TRC';
  if (s === 'debug') return 'DBG';
  if (s === 'info' || s === 'notice') return 'INF';
  if (s === 'warning' || s === 'warn') return 'WRN';
  if (s === 'error') return 'ERR';
  if (s === 'fatal') return 'FTL';
  return 'INF';
}

/* ── Component ─────────────────────────────────────────────── */

const LogViewer: React.FC = () => {
  const [logs, setLogs] = useState<LogEntry[]>([]);
  const [filterLevel, setFilterLevel] = useState<LogLevel>('info');
  const [serverLevel, setServerLevel] = useState<LogLevel>('info');
  const [searchQuery, setSearchQuery] = useState('');
  const [connStatus, setConnStatus] = useState<'connecting' | 'connected' | 'error' | 'disconnected'>('disconnected');
  const [autoScroll, setAutoScroll] = useState(true);
  const [isSettingLevel, setIsSettingLevel] = useState(false);
  const [scrollTop, setScrollTop] = useState(0);
  const [viewHeight, setViewHeight] = useState(0);

  const containerRef = useRef<HTMLDivElement>(null);
  const streamRef = useRef<LogStreamHandle | null>(null);
  const lastSeqRef = useRef<number | null>(null);
  const autoScrollRef = useRef(true);
  const reconnectRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const batchRef = useRef<LogEntry[]>([]);
  const batchTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const userScrollingRef = useRef(false);

  /* ── Auto-scroll ─────────────────────────────────────────── */

  const scrollToBottom = useCallback(() => {
    const el = containerRef.current;
    if (!el) return;
    // Set scrollTop to max. Use rAF to ensure the virtual container
    // has rendered at the new height before we scroll.
    el.scrollTop = el.scrollHeight;
    requestAnimationFrame(() => {
      el.scrollTop = el.scrollHeight;
    });
  }, []);

  const handleScroll = useCallback(() => {
    const el = containerRef.current;
    if (!el) return;
    setScrollTop(el.scrollTop);

    // Only update auto-scroll if the user is actively scrolling (not programmatic)
    if (userScrollingRef.current) {
      const nearBottom = el.scrollHeight - el.scrollTop <= el.clientHeight + BOTTOM_THRESHOLD;
      autoScrollRef.current = nearBottom;
      setAutoScroll(nearBottom);
    }
  }, []);

  // Track user-initiated scrolls via pointer/keyboard events
  useEffect(() => {
    const el = containerRef.current;
    if (!el) return;

    const onUserScrollStart = () => { userScrollingRef.current = true; };
    const onUserScrollEnd = () => { userScrollingRef.current = false; };

    el.addEventListener('pointerdown', onUserScrollStart);
    el.addEventListener('pointerup', onUserScrollEnd);
    el.addEventListener('keydown', onUserScrollStart);
    el.addEventListener('keyup', onUserScrollEnd);
    // Wheel is always user-initiated
    el.addEventListener('wheel', () => {
      userScrollingRef.current = true;
      // Reset after a short delay since wheel has no "end" event
      setTimeout(() => { userScrollingRef.current = false; }, 200);
    }, { passive: true });

    return () => {
      el.removeEventListener('pointerdown', onUserScrollStart);
      el.removeEventListener('pointerup', onUserScrollEnd);
      el.removeEventListener('keydown', onUserScrollStart);
      el.removeEventListener('keyup', onUserScrollEnd);
    };
  }, []);

  // Measure container height on mount and resize
  useEffect(() => {
    const el = containerRef.current;
    if (!el) return;
    const ro = new ResizeObserver(() => {
      setViewHeight(el.clientHeight);
    });
    ro.observe(el);
    setViewHeight(el.clientHeight);
    return () => ro.disconnect();
  }, []);

  /* ── Batch incoming entries ──────────────────────────────── */

  const flushBatch = useCallback(() => {
    batchTimerRef.current = null;
    const batch = batchRef.current;
    if (batch.length === 0) return;
    batchRef.current = [];

    setLogs(prev => {
      const merged = [...prev, ...batch];
      return merged.length > MAX_CLIENT_LOGS ? merged.slice(-MAX_CLIENT_LOGS) : merged;
    });
  }, []);

  const enqueuEntry = useCallback((entry: LogEntry) => {
    batchRef.current.push(entry);
    if (!batchTimerRef.current) {
      batchTimerRef.current = setTimeout(flushBatch, BATCH_INTERVAL);
    }
  }, [flushBatch]);

  /* ── Connect to log stream ───────────────────────────────── */

  const connect = useCallback(() => {
    if (streamRef.current) {
      streamRef.current.close();
      streamRef.current = null;
    }

    setConnStatus('connecting');

    const handle = api.connectLogStream({
      onConnected: () => setConnStatus('connected'),
      onDisconnected: () => {
        setConnStatus('disconnected');
        streamRef.current = null;
        // Auto-reconnect
        reconnectRef.current = setTimeout(connect, RECONNECT_DELAY);
      },
      onError: (msg) => {
        console.warn('[LogViewer] Error:', msg);
        setConnStatus('error');
      },
      onSnapshot: (entries) => {
        if (entries.length > 0) {
          lastSeqRef.current = entries[entries.length - 1].seq;
        }
        setLogs(prev => {
          // On reconnect, merge with existing logs instead of replacing
          if (prev.length === 0) return entries.slice(-MAX_CLIENT_LOGS);
          const lastExistingSeq = prev[prev.length - 1]?.seq ?? -1;
          const newEntries = entries.filter(e => e.seq > lastExistingSeq);
          if (newEntries.length === 0) return prev;
          const merged = [...prev, ...newEntries];
          return merged.length > MAX_CLIENT_LOGS ? merged.slice(-MAX_CLIENT_LOGS) : merged;
        });
      },
      onEntry: (entry) => {
        lastSeqRef.current = entry.seq;
        enqueuEntry(entry);
      },
    }, lastSeqRef.current);

    streamRef.current = handle;
  }, [enqueuEntry]);

  useEffect(() => {
    // Health check is needed for websocket_port — connect stream as soon as it's ready
    // Fetch server log level in parallel (non-blocking)
    const tryConnect = async () => {
      try {
        await api.health();
        connect();
      } catch {
        setConnStatus('error');
        reconnectRef.current = setTimeout(tryConnect, RECONNECT_DELAY);
      }
    };

    tryConnect();

    api.getLogLevel().then(level => {
      if (LOG_LEVELS.includes(level as LogLevel)) setServerLevel(level as LogLevel);
    }).catch(() => {});

    return () => {
      if (streamRef.current) streamRef.current.close();
      if (reconnectRef.current) clearTimeout(reconnectRef.current);
      if (batchTimerRef.current) clearTimeout(batchTimerRef.current);
    };
  }, [connect]);

  /* ── Server log level ────────────────────────────────────── */

  const handleServerLevelChange = useCallback(async (level: LogLevel) => {
    setIsSettingLevel(true);
    try {
      await api.setLogLevel(level);
      setServerLevel(level);
    } catch (err) {
      console.error('Failed to set log level:', err);
    }
    setIsSettingLevel(false);
  }, []);

  /* ── Clear logs ──────────────────────────────────────────── */

  const clearLogs = useCallback(() => {
    setLogs([]);
    lastSeqRef.current = null;
  }, []);

  /* ── Filtering ───────────────────────────────────────────── */

  const filterPriority = SEVERITY_PRIORITY[filterLevel] ?? 2;

  const filteredLogs = useMemo(() => {
    let result = logs.filter(l => (SEVERITY_PRIORITY[l.severity] ?? 2) >= filterPriority);
    if (searchQuery) {
      const q = searchQuery.toLowerCase();
      result = result.filter(l =>
        l.line.toLowerCase().includes(q) ||
        l.tag.toLowerCase().includes(q) ||
        l.severity.toLowerCase().includes(q)
      );
    }
    return result;
  }, [logs, filterPriority, searchQuery]);

  // Use useLayoutEffect so scroll happens synchronously after DOM update
  // (must be after filteredLogs declaration to avoid temporal dead zone)
  useLayoutEffect(() => {
    if (autoScrollRef.current) scrollToBottom();
  }, [filteredLogs.length, scrollToBottom]);

  // Re-scroll when the view becomes visible (user switches back to Logs tab)
  useEffect(() => {
    const el = containerRef.current;
    if (!el) return;
    const observer = new IntersectionObserver(
      ([entry]) => {
        if (entry.isIntersecting && autoScrollRef.current) {
          scrollToBottom();
        }
      },
      { threshold: 0.01 }
    );
    observer.observe(el);
    return () => observer.disconnect();
  }, [scrollToBottom]);

  /* ── Virtual scroll calculation ──────────────────────────── */

  const totalHeight = filteredLogs.length * LINE_HEIGHT;
  const startIdx = Math.max(0, Math.floor(scrollTop / LINE_HEIGHT) - OVERSCAN);
  const endIdx = Math.min(filteredLogs.length, Math.ceil((scrollTop + viewHeight) / LINE_HEIGHT) + OVERSCAN);
  const visibleLogs = filteredLogs.slice(startIdx, endIdx);

  /* ── Status indicator ────────────────────────────────────── */

  const statusDot = connStatus === 'connected' ? 'logs-status--connected' :
    connStatus === 'connecting' ? 'logs-status--connecting' :
    connStatus === 'error' ? 'logs-status--error' : 'logs-status--disconnected';

  const statusLabel = connStatus === 'connected' ? 'Live' :
    connStatus === 'connecting' ? 'Connecting…' :
    connStatus === 'error' ? 'Error' : 'Disconnected';

  /* ── Render ──────────────────────────────────────────────── */

  return (
    <section className="logs-view" data-view="logs">
      {/* ── Toolbar ──────────────────────────────────────── */}
      <div className="logs-toolbar">
        <div className="logs-toolbar__left">
          <span className={`logs-status__dot ${statusDot}`} />
          <span className="logs-status__label">{statusLabel}</span>
          <span className="logs-toolbar__count">
            {filteredLogs.length} / {logs.length} entries
          </span>
        </div>

        <div className="logs-toolbar__center">
          <input
            type="text"
            className="logs-search"
            placeholder="Filter logs…"
            value={searchQuery}
            onChange={e => setSearchQuery(e.target.value)}
          />
        </div>

        <div className="logs-toolbar__right">
          <label className="logs-level">
            <span className="logs-level__label">Show:</span>
            <select
              className="logs-level__select"
              value={filterLevel}
              onChange={e => setFilterLevel(e.target.value as LogLevel)}
            >
              {LOG_LEVELS.map(l => (
                <option key={l} value={l}>{l.charAt(0).toUpperCase() + l.slice(1)}+</option>
              ))}
            </select>
          </label>

          <label className="logs-level">
            <span className="logs-level__label">Server:</span>
            <select
              className="logs-level__select"
              value={serverLevel}
              disabled={isSettingLevel}
              onChange={e => handleServerLevelChange(e.target.value as LogLevel)}
            >
              {LOG_LEVELS.map(l => (
                <option key={l} value={l}>{l.charAt(0).toUpperCase() + l.slice(1)}</option>
              ))}
            </select>
          </label>

          <button className="logs-btn" onClick={clearLogs} title="Clear logs">
            Clear
          </button>

          {connStatus !== 'connected' && (
            <button className="logs-btn logs-btn--accent" onClick={connect} title="Reconnect">
              Reconnect
            </button>
          )}
        </div>
      </div>

      {/* ── Log output (virtualized) ────────────────────── */}
      <div
        className="logs-output"
        ref={containerRef}
        onScroll={handleScroll}
      >
        {filteredLogs.length === 0 ? (
          <div className="logs-empty">
            {logs.length === 0
              ? (connStatus === 'connected' ? 'Waiting for log entries…' : 'Not connected to log stream')
              : `No entries match "${searchQuery || filterLevel}+" filter`}
          </div>
        ) : (
          <div className="logs-virtual" style={{ height: totalHeight, position: 'relative' }}>
            <div style={{ position: 'absolute', top: startIdx * LINE_HEIGHT, left: 0, right: 0 }}>
              {visibleLogs.map(entry => (
                <div className={`logs-line ${severityClass(entry.severity)}`} key={entry.seq}>
                  <span className="logs-line__time">{entry.timestamp.split(' ')[1] || entry.timestamp}</span>
                  <span className={`logs-line__badge ${severityClass(entry.severity)}`}>
                    {severityBadge(entry.severity)}
                  </span>
                  <span className="logs-line__tag">{entry.tag}</span>
                  <span className="logs-line__text">{entry.line}</span>
                </div>
              ))}
            </div>
          </div>
        )}
      </div>

      {/* ── Jump to bottom ───────────────────────────────── */}
      {!autoScroll && filteredLogs.length > 0 && (
        <button
          className="logs-jump"
          onClick={() => {
            autoScrollRef.current = true;
            setAutoScroll(true);
            // Force scroll position to max, then re-apply after render
            // to account for virtual container height changes
            const el = containerRef.current;
            if (el) {
              el.scrollTop = el.scrollHeight;
              setScrollTop(el.scrollTop);
              requestAnimationFrame(() => {
                el.scrollTop = el.scrollHeight;
                setScrollTop(el.scrollTop);
              });
            }
          }}
        >
          ↓ Jump to bottom
        </button>
      )}
    </section>
  );
};

export default LogViewer;
