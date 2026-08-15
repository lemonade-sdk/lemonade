import React, { useState, useEffect, useLayoutEffect, useRef, useCallback, useMemo } from 'react';
import api, { LogEntry, LogStreamHandle } from '../api';
import { Icon } from './Icon';
import WorkspaceRailHeader from './WorkspaceRailHeader';
import { WorkspaceActionButton, WorkspacePaneHeader } from './WorkspacePanels';
import { useWorkspaceMobileRail } from '../hooks/useWorkspaceMobileRail';
import { useI18n } from '../i18n';

/* ── Constants ─────────────────────────────────────────────── */

const MAX_CLIENT_LOGS = 2000;
const LOG_SOURCE_LIMIT = 10;
const RECONNECT_DELAY = 5000;
const BOTTOM_THRESHOLD = 60;
const LINE_HEIGHT = 22;
const OVERSCAN = 10;
const BATCH_INTERVAL = 100; // ms — batch incoming entries
const OTHER_SOURCE_KEY = '__other__';

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

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

export function getDisplayedLogLine(entry: Pick<LogEntry, 'timestamp' | 'severity' | 'tag' | 'line'>): string {
  const line = String(entry.line || '');
  const timestamp = entry.timestamp.trim();
  const severity = entry.severity.trim();
  const tag = entry.tag.trim();
  if (!timestamp || !severity || !tag) return line;

  const prefix = new RegExp(
    `^\\s*${escapeRegExp(timestamp)}\\s+\\[${escapeRegExp(severity)}\\]\\s+\\(${escapeRegExp(tag)}\\)(?:\\s+|$)`,
    'i',
  );
  return line.replace(prefix, '');
}

/* ── Component ─────────────────────────────────────────────── */

interface LogViewerProps {
  embedded?: boolean;
}

const LogViewer: React.FC<LogViewerProps> = ({ embedded = false }) => {
  const { t } = useI18n('logs');
  const [logs, setLogs] = useState<LogEntry[]>([]);
  const [filterLevel, setFilterLevel] = useState<LogLevel>('info');
  const [serverLevel, setServerLevel] = useState<LogLevel>('info');
  const [searchQuery, setSearchQuery] = useState('');
  const [tagFilter, setTagFilter] = useState('all');
  const [railCollapsed, setRailCollapsed] = useState(false);
  const mobileFilters = useWorkspaceMobileRail();
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
  const shouldReconnectRef = useRef(true);
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
    const onWheel = () => {
      userScrollingRef.current = true;
      // Reset after a short delay since wheel has no "end" event
      window.setTimeout(() => { userScrollingRef.current = false; }, 200);
    };
    el.addEventListener('wheel', onWheel, { passive: true });

    return () => {
      el.removeEventListener('pointerdown', onUserScrollStart);
      el.removeEventListener('pointerup', onUserScrollEnd);
      el.removeEventListener('keydown', onUserScrollStart);
      el.removeEventListener('keyup', onUserScrollEnd);
      el.removeEventListener('wheel', onWheel);
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
    shouldReconnectRef.current = true;
    if (reconnectRef.current) {
      clearTimeout(reconnectRef.current);
      reconnectRef.current = null;
    }
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
        if (!shouldReconnectRef.current) return;
        if (reconnectRef.current) clearTimeout(reconnectRef.current);
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
    shouldReconnectRef.current = true;
    // Health check verifies the API before opening the log stream.
    // Fetch server log level in parallel (non-blocking)
    const tryConnect = async () => {
      if (!shouldReconnectRef.current) return;
      try {
        await api.health();
        connect();
      } catch {
        setConnStatus('error');
        if (!shouldReconnectRef.current) return;
        if (reconnectRef.current) clearTimeout(reconnectRef.current);
        reconnectRef.current = setTimeout(tryConnect, RECONNECT_DELAY);
      }
    };

    tryConnect();

    api.getLogLevel().then(level => {
      if (LOG_LEVELS.includes(level as LogLevel)) setServerLevel(level as LogLevel);
    }).catch(() => {});

    return () => {
      shouldReconnectRef.current = false;
      if (streamRef.current) streamRef.current.close();
      if (reconnectRef.current) clearTimeout(reconnectRef.current);
      if (batchTimerRef.current) clearTimeout(batchTimerRef.current);
      streamRef.current = null;
      reconnectRef.current = null;
      batchTimerRef.current = null;
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
    if (tagFilter !== 'all') result = result.filter(l => (String(l.tag || '').trim() || OTHER_SOURCE_KEY) === tagFilter);
    if (searchQuery) {
      const q = searchQuery.toLowerCase();
      result = result.filter(l =>
        String(l.line || '').toLowerCase().includes(q) ||
        String(l.tag || '').toLowerCase().includes(q) ||
        String(l.severity || '').toLowerCase().includes(q)
      );
    }
    return result;
  }, [logs, filterPriority, searchQuery, tagFilter]);

  const logSources = useMemo(() => {
    const counts = new Map<string, number>();
    logs.forEach(log => {
      const tag = String(log.tag || '').trim() || OTHER_SOURCE_KEY;
      counts.set(tag, (counts.get(tag) || 0) + 1);
    });
    const ranked = Array.from(counts.entries())
      .sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]));
    const top = ranked.slice(0, LOG_SOURCE_LIMIT);
    // The selected source must stay clickable even once it falls out of the
    // top slice, or the list looks empty with no visible way back.
    if (tagFilter !== 'all' && !top.some(([tag]) => tag === tagFilter)) {
      const selected = ranked.find(([tag]) => tag === tagFilter);
      if (selected) top.push(selected);
    }
    return { sources: top, hiddenCount: Math.max(0, ranked.length - LOG_SOURCE_LIMIT) };
  }, [logs, tagFilter]);

  // Clearing the buffer retires every tag; a filter left pointing at one that
  // no longer exists would silently hide the whole stream.
  useEffect(() => {
    if (tagFilter !== 'all' && !logSources.sources.some(([tag]) => tag === tagFilter)) {
      setTagFilter('all');
    }
  }, [logSources, tagFilter]);

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

  const renderedLogRows = useMemo(() => {
    const rows: { entry: LogEntry; line: string; partIndex: number }[] = [];
    filteredLogs.forEach(entry => {
      const parts = getDisplayedLogLine(entry).split(/\r?\n/);
      (parts.length ? parts : ['']).forEach((line, partIndex) => {
        rows.push({ entry, line, partIndex });
      });
    });
    return rows;
  }, [filteredLogs]);

  const totalHeight = renderedLogRows.length * LINE_HEIGHT;
  const startIdx = Math.max(0, Math.floor(scrollTop / LINE_HEIGHT) - OVERSCAN);
  const endIdx = Math.min(renderedLogRows.length, Math.ceil((scrollTop + viewHeight) / LINE_HEIGHT) + OVERSCAN);
  const visibleLogRows = renderedLogRows.slice(startIdx, endIdx);

  /* ── Status indicator ────────────────────────────────────── */

  const statusDot = connStatus === 'connected' ? 'logs-status--connected' :
    connStatus === 'connecting' ? 'logs-status--connecting' :
    connStatus === 'error' ? 'logs-status--error' : 'logs-status--disconnected';

  const statusLabel = connStatus === 'connected' ? t('status.live') :
    connStatus === 'connecting' ? t('status.connecting') :
    connStatus === 'error' ? t('status.error') : t('status.disconnected');

  /* ── Render ──────────────────────────────────────────────── */

  return (
    <section className={`logs-view logs-workspace${embedded ? ' logs-workspace--embedded' : ''}${embedded && mobileFilters.isOpen ? ' logs-workspace--filters-open' : ''}${!embedded && railCollapsed ? ' workspace--rail-collapsed' : ''}`} data-view="logs">
      {embedded && mobileFilters.isOpen && (
        <div className="workspace-mobile-rail-backdrop" onClick={mobileFilters.close} aria-hidden="true" />
      )}
      <aside
        ref={embedded ? mobileFilters.panelRef : undefined}
        className={`${embedded ? 'monitor-subpanel' : 'workspace-rail'} logs-rail${!embedded && railCollapsed ? ' is-collapsed' : ''}${embedded && mobileFilters.isOpen ? ' is-mobile-open' : ''}`}
        aria-label={t('filters.aria')}
        role={embedded && mobileFilters.isOpen ? 'dialog' : undefined}
        aria-modal={embedded && mobileFilters.isOpen ? true : undefined}
      >
        {embedded ? (
          <header className="monitor-subpanel__header">
            <div className="monitor-subpanel__title-row">
              <div>
                <h2>{t('filters.streamTitle')}</h2>
                <p>{t('counts.entriesReceived', { count: logs.length })}</p>
              </div>
              <WorkspaceActionButton
                className="logs-rail__mobile-close"
                appearance="quiet"
                size="toolbar"
                icon="x"
                iconOnly
                aria-label={t('filters.close')}
                onClick={mobileFilters.close}
              />
            </div>
          </header>
        ) : (
          <WorkspaceRailHeader
            title={t('filters.title')}
            sidebarLabel={t('filters.sidebar')}
            purpose="filter"
            collapsed={railCollapsed}
            onToggle={() => setRailCollapsed(value => !value)}
          />
        )}

        <div className={`${embedded ? 'monitor-subpanel__body' : 'workspace-rail__body'} logs-rail__body`}>
          <div className="logs-rail__status">
            <span className={`logs-status__dot ${statusDot}`} />
            <span className="logs-status__label">{statusLabel}</span>
            <span className="logs-toolbar__count">{t('counts.entries', { count: logs.length })}</span>
          </div>

          <div className="logs-rail__search">
            <input
              type="text"
              className="inspect-search-input logs-search"
              placeholder={t('filters.searchPlaceholder')}
              value={searchQuery}
              onChange={e => setSearchQuery(e.target.value)}
              aria-label={t('filters.searchAria')}
            />
          </div>

          <div className="workspace-control-group">
            <span className="workspace-control-group__label">{t('filters.visibility')}</span>
            <label className="logs-level">
              <span className="logs-level__label">{t('filters.minimumLevel')}</span>
              <select
                className="select logs-level__select"
                value={filterLevel}
                onChange={e => setFilterLevel(e.target.value as LogLevel)}
              >
                {LOG_LEVELS.map(l => (
                  <option key={l} value={l}>{t(`levels.${l}`)}+</option>
                ))}
              </select>
            </label>

            <label className="logs-level">
              <span className="logs-level__label">{t('filters.serverCaptureLevel')}</span>
              <select
                className="select logs-level__select"
                value={serverLevel}
                disabled={isSettingLevel}
                onChange={e => handleServerLevelChange(e.target.value as LogLevel)}
              >
                {LOG_LEVELS.map(l => (
                  <option key={l} value={l}>{t(`levels.${l}`)}</option>
                ))}
              </select>
            </label>
          </div>

          <div className="workspace-control-group workspace-filter-list logs-sources">
            <span className="workspace-control-group__label">{t('filters.sources')}</span>
            <button type="button" className={`workspace-filter-list__item${tagFilter === 'all' ? ' is-active' : ''}`} onClick={() => setTagFilter('all')}>
              <span className="workspace-filter-list__icon"><Icon name="logs" size={14} aria-hidden="true" /></span>
              <span className="workspace-filter-list__label">{t('filters.allSources')}</span><small className="workspace-filter-list__count">{logs.length}</small>
            </button>
            {logSources.sources.map(([tag, count]) => (
              <button key={tag} type="button" className={`workspace-filter-list__item${tagFilter === tag ? ' is-active' : ''}`} onClick={() => setTagFilter(tag)}>
                <span className="workspace-filter-list__icon"><Icon name="terminal-square" size={14} aria-hidden="true" /></span>
                <span className="workspace-filter-list__label">{tag === OTHER_SOURCE_KEY ? t('filters.otherSource') : tag}</span><small className="workspace-filter-list__count">{count}</small>
              </button>
            ))}
            {logSources.hiddenCount > 0 && (
              <p className="workspace-filter-list__note">{t('filters.hiddenSources', { count: logSources.hiddenCount })}</p>
            )}
          </div>
        </div>

        <div className={`${embedded ? 'monitor-subpanel__footer' : 'workspace-rail__footer'} logs-rail__actions`}>
          <WorkspaceActionButton className="logs-btn" icon="trash" onClick={clearLogs} title={t('actions.clearTitle')} aria-label={t('actions.clearAria')}>{t('actions.clearOutput')}</WorkspaceActionButton>

          {connStatus !== 'connected' && (
            <WorkspaceActionButton className="logs-btn" appearance="primary" icon="rotate-ccw" onClick={connect} title={t('actions.reconnect')} aria-label={t('actions.reconnectAria')}>{t('actions.reconnect')}</WorkspaceActionButton>
          )}
        </div>
      </aside>

      <div className="workspace-pane logs-main">
        <WorkspacePaneHeader
          className="logs-main__header"
          title={t('main.title')}
          subtitle={t('counts.shown', { shown: filteredLogs.length, total: logs.length })}
          actions={<div className="logs-main__tools">
            {embedded && (
              <WorkspaceActionButton
                ref={mobileFilters.triggerRef}
                className="logs-filter-toggle"
                appearance="secondary"
                size="toolbar"
                icon="funnel"
                iconOnly
                aria-label={t('actions.openFilters')}
                aria-expanded={mobileFilters.isOpen}
                onClick={mobileFilters.toggle}
              />
            )}
          </div>}
        />

      {/* ── Log output (virtualized) ────────────────────── */}
      <div
        className="logs-output"
        ref={containerRef}
        onScroll={handleScroll}
      >
        {filteredLogs.length === 0 ? (
          <div className="logs-empty">
            {logs.length === 0
              ? (connStatus === 'connected' ? t('empty.waiting') : t('empty.notConnected'))
              : t('empty.noMatch', { filter: `${searchQuery || filterLevel}+` })}
          </div>
        ) : (
          <div className="logs-virtual" style={{ height: totalHeight, position: 'relative' }}>
            <div style={{ position: 'absolute', top: startIdx * LINE_HEIGHT, left: 0, right: 0 }}>
              {visibleLogRows.map(({ entry, line, partIndex }) => {
                const isContinuation = partIndex > 0;
                return (
                  <div className={`logs-line ${severityClass(entry.severity)}${isContinuation ? ' logs-line--continuation' : ''}`} key={`${entry.seq}:${partIndex}`}>
                    <span className="logs-line__time">{isContinuation ? '' : (entry.timestamp.split(' ')[1] || entry.timestamp)}</span>
                    <span className={`logs-line__badge ${severityClass(entry.severity)}`}>
                      {isContinuation ? '' : severityBadge(entry.severity)}
                    </span>
                    <span className="logs-line__tag">{isContinuation ? '...' : entry.tag}</span>
                    <span className="logs-line__text">{line || ' '}</span>
                  </div>
                );
              })}
            </div>
          </div>
        )}
      </div>

      {/* ── Jump to bottom ───────────────────────────────── */}
      {!autoScroll && filteredLogs.length > 0 && (
        <WorkspaceActionButton
          className="logs-jump"
          icon="chevron-down"
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
          {t('actions.jumpBottom')}
        </WorkspaceActionButton>
      )}
      </div>
    </section>
  );
};

export default LogViewer;
