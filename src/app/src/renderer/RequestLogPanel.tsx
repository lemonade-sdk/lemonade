import React, { useCallback, useEffect, useMemo, useState } from 'react';
import {
  fetchRequestLogSearch,
  fetchRequestLogStats,
  RequestLogApiError,
  RequestLogEntry,
  RequestLogStats,
} from './utils/requestLogApi';

type KeepAliveFilter = 'any' | 'zero' | 'has';
type SinceFilter = '1h' | '24h' | '7d';

interface FilterState {
  model: string;
  clientIp: string;
  path: string;
  keepAlive: KeepAliveFilter;
  since: SinceFilter;
}

const DEFAULT_FILTERS: FilterState = {
  model: '',
  clientIp: '',
  path: '',
  keepAlive: 'any',
  since: '24h',
};

const PAGE_SIZE = 100;

function formatTime(iso: string): string {
  try {
    const date = new Date(iso);
    if (Number.isNaN(date.getTime())) {
      return iso;
    }
    return date.toLocaleString();
  } catch {
    return iso;
  }
}

function statusClass(status: number | null): string {
  if (status === null) {
    return '';
  }
  if (status >= 200 && status < 300) {
    return 'request-log-status--ok';
  }
  if (status >= 400 && status < 500) {
    return 'request-log-status--warn';
  }
  if (status >= 500) {
    return 'request-log-status--error';
  }
  return '';
}

function formatDuration(ms: number | null): string {
  if (ms === null || ms === undefined) {
    return '—';
  }
  if (ms < 1000) {
    return `${ms} ms`;
  }
  return `${(ms / 1000).toFixed(2)} s`;
}

function applyKeepAliveClientFilter(
  entries: RequestLogEntry[],
  keepAlive: KeepAliveFilter,
): RequestLogEntry[] {
  if (keepAlive === 'has') {
    return entries.filter((entry) => entry.keep_alive !== null && entry.keep_alive !== '');
  }
  return entries;
}

const RequestLogPanel: React.FC = () => {
  const [filters, setFilters] = useState<FilterState>(DEFAULT_FILTERS);
  const [appliedFilters, setAppliedFilters] = useState<FilterState>(DEFAULT_FILTERS);
  const [entries, setEntries] = useState<RequestLogEntry[]>([]);
  const [stats, setStats] = useState<RequestLogStats | null>(null);
  const [selectedId, setSelectedId] = useState<number | null>(null);
  const [offset, setOffset] = useState(0);
  const [hasMore, setHasMore] = useState(false);
  const [loading, setLoading] = useState(true);
  const [loadingMore, setLoadingMore] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [autoRefresh, setAutoRefresh] = useState(false);

  const selectedEntry = useMemo(
    () => entries.find((entry) => entry.id === selectedId) ?? null,
    [entries, selectedId],
  );

  const loadData = useCallback(async (opts?: { append?: boolean; filterState?: FilterState; startOffset?: number }) => {
    const activeFilters = opts?.filterState ?? appliedFilters;
    const isAppend = opts?.append ?? false;
    const startOffset = isAppend ? (opts?.startOffset ?? 0) : 0;

    if (isAppend) {
      setLoadingMore(true);
    } else {
      setLoading(true);
      setError(null);
    }

    try {
      const keepAliveParam =
        activeFilters.keepAlive === 'zero' ? '0' : undefined;

      const [searchResult, statsResult] = await Promise.all([
        fetchRequestLogSearch({
          model: activeFilters.model.trim() || undefined,
          client_ip: activeFilters.clientIp.trim() || undefined,
          path: activeFilters.path.trim() || undefined,
          since: activeFilters.since,
          keep_alive: keepAliveParam,
          limit: PAGE_SIZE,
          offset: startOffset,
        }),
        isAppend ? Promise.resolve(null) : fetchRequestLogStats(activeFilters.since),
      ]);

      let newEntries = searchResult.entries ?? [];
      if (activeFilters.keepAlive === 'has') {
        newEntries = applyKeepAliveClientFilter(newEntries, 'has');
      }

      if (isAppend) {
        setEntries((prev) => [...prev, ...newEntries]);
        setOffset(startOffset);
      } else {
        setEntries(newEntries);
        setOffset(0);
        setSelectedId(newEntries[0]?.id ?? null);
        if (statsResult) {
          setStats(statsResult);
        }
      }

      setHasMore((searchResult.entries?.length ?? 0) >= PAGE_SIZE);
      setError(null);
    } catch (err) {
      const message =
        err instanceof RequestLogApiError
          ? err.message
          : err instanceof Error
            ? err.message
            : 'Failed to load request logs';
      setError(message);
      if (!isAppend) {
        setEntries([]);
        setStats(null);
      }
    } finally {
      setLoading(false);
      setLoadingMore(false);
    }
  }, [appliedFilters]);

  const handleLoadMore = () => {
    void loadData({ append: true, startOffset: offset + PAGE_SIZE });
  };

  useEffect(() => {
    void loadData();
  }, [appliedFilters, loadData]);

  useEffect(() => {
    if (!autoRefresh) {
      return undefined;
    }
    const intervalId = window.setInterval(() => {
      void loadData();
    }, 30000);
    return () => window.clearInterval(intervalId);
  }, [autoRefresh, loadData]);

  const handleApply = () => {
    setAppliedFilters({ ...filters });
  };

  const handleClear = () => {
    setFilters(DEFAULT_FILTERS);
    setAppliedFilters(DEFAULT_FILTERS);
  };

  const handleCopyRow = async () => {
    if (!selectedEntry) {
      return;
    }
    try {
      await navigator.clipboard.writeText(JSON.stringify(selectedEntry, null, 2));
    } catch {
      // clipboard unavailable
    }
  };

  return (
    <div className="request-log-panel">
      <div className="request-log-toolbar">
        <h3 className="request-log-title">HTTP Request Logs</h3>
        <div className="request-log-toolbar-actions">
          <label className="request-log-auto-refresh">
            <input
              type="checkbox"
              checked={autoRefresh}
              onChange={(e) => setAutoRefresh(e.target.checked)}
            />
            Auto-refresh (30s)
          </label>
          <button type="button" className="logs-control-btn" onClick={() => void loadData()}>
            Refresh
          </button>
        </div>
      </div>

      {stats && !error && (
        <div className="request-log-stats">
          <span className="request-log-stat-chip">
            {stats.total_requests.toLocaleString()} requests
          </span>
          <span className="request-log-stat-chip">
            {stats.unique_client_ips.toLocaleString()} clients
          </span>
          <span className="request-log-stat-chip">
            {stats.keep_alive_requests.toLocaleString()} keep_alive
          </span>
          <span className="request-log-stat-chip">
            avg {Math.round(stats.avg_duration_ms)} ms
          </span>
          {Object.entries(stats.by_endpoint_type).map(([type, count]) => (
            <span key={type} className="request-log-stat-chip request-log-stat-chip--muted">
              {type}: {count}
            </span>
          ))}
        </div>
      )}

      <div className="request-log-filters">
        <input
          className="form-input request-log-filter-input"
          placeholder="Model"
          value={filters.model}
          onChange={(e) => setFilters((prev) => ({ ...prev, model: e.target.value }))}
        />
        <input
          className="form-input request-log-filter-input"
          placeholder="Client IP"
          value={filters.clientIp}
          onChange={(e) => setFilters((prev) => ({ ...prev, clientIp: e.target.value }))}
        />
        <input
          className="form-input request-log-filter-input"
          placeholder="Path contains"
          value={filters.path}
          onChange={(e) => setFilters((prev) => ({ ...prev, path: e.target.value }))}
        />
        <select
          className="form-input form-select request-log-filter-select"
          value={filters.keepAlive}
          onChange={(e) =>
            setFilters((prev) => ({
              ...prev,
              keepAlive: e.target.value as KeepAliveFilter,
            }))
          }
        >
          <option value="any">Keep-alive: any</option>
          <option value="zero">Keep-alive: 0</option>
          <option value="has">Keep-alive: set</option>
        </select>
        <select
          className="form-input form-select request-log-filter-select"
          value={filters.since}
          onChange={(e) =>
            setFilters((prev) => ({
              ...prev,
              since: e.target.value as SinceFilter,
            }))
          }
        >
          <option value="1h">Last 1 hour</option>
          <option value="24h">Last 24 hours</option>
          <option value="7d">Last 7 days</option>
        </select>
        <button type="button" className="logs-control-btn" onClick={handleApply}>
          Apply
        </button>
        <button type="button" className="logs-control-btn" onClick={handleClear}>
          Clear
        </button>
      </div>

      {error && (
        <div className="request-log-unavailable">
          <p>{error}</p>
          {error.includes('not enabled') && (
            <div className="request-log-unavailable-hint">
              <p>Start <code>lemond</code> with:</p>
              <pre className="request-log-setup-snippet">{`export LEMONADE_REQUEST_LOG_ENABLED=true
export LEMONADE_REQUEST_LOG_DATABASE_URL=postgresql://lemonade:change-me@127.0.0.1:<PORT>/lemonade_logs
./build/lemond`}</pre>
              <p>
                Use the URL printed by{' '}
                <code>./examples/start-request-log-db.sh</code>. Rebuild with{' '}
                <code>libpq-dev</code> installed so request logging is compiled in.
              </p>
            </div>
          )}
          {error.includes('database is unavailable') && (
            <div className="request-log-unavailable-hint">
              <p>Check that PostgreSQL is running and the URL matches the published port:</p>
              <pre className="request-log-setup-snippet">{`docker compose -f examples/docker-compose.request-log.yml ps
# then restart lemond with the correct LEMONADE_REQUEST_LOG_DATABASE_URL`}</pre>
            </div>
          )}
          {error.includes('not enabled') === false &&
            error.includes('database is unavailable') === false && (
            <p className="request-log-unavailable-hint">
              See{' '}
              <a
                href="https://lemonade-server.ai/docs/guide/configuration/request-log/"
                target="_blank"
                rel="noopener noreferrer"
              >
                request logging setup
              </a>
              .
            </p>
          )}
        </div>
      )}

      {!error && loading && (
        <div className="request-log-empty">Loading request logs…</div>
      )}

      {!error && !loading && entries.length === 0 && (
        <div className="request-log-empty">No matching requests</div>
      )}

      {!error && entries.length > 0 && (
        <div className="request-log-table-wrap">
          <table className="request-log-table">
            <thead>
              <tr>
                <th>Time</th>
                <th>Client</th>
                <th>Method</th>
                <th>Path</th>
                <th>Model</th>
                <th>Keep-alive</th>
                <th>Stream</th>
                <th>Status</th>
                <th>Duration</th>
              </tr>
            </thead>
            <tbody>
              {entries.map((entry) => (
                <tr
                  key={entry.id}
                  className={`request-log-row ${selectedId === entry.id ? 'selected' : ''} ${
                    entry.keep_alive === '0' ? 'request-log-row--keep-alive-zero' : ''
                  }`}
                  onClick={() => setSelectedId(entry.id)}
                >
                  <td>{formatTime(entry.created_at)}</td>
                  <td title={entry.forwarded_for ?? undefined}>
                    {entry.client_ip ?? '—'}
                  </td>
                  <td>{entry.method}</td>
                  <td className="request-log-path-cell" title={entry.path}>
                    {entry.path}
                  </td>
                  <td>{entry.model ?? '—'}</td>
                  <td>{entry.keep_alive ?? '—'}</td>
                  <td>
                    {entry.stream === null || entry.stream === undefined
                      ? '—'
                      : entry.stream
                        ? 'yes'
                        : 'no'}
                  </td>
                  <td className={statusClass(entry.status_code)}>
                    {entry.status_code ?? '—'}
                  </td>
                  <td>{formatDuration(entry.duration_ms)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}

      {!error && hasMore && (
        <div className="request-log-pagination">
          <button
            type="button"
            className="logs-control-btn"
            disabled={loadingMore}
            onClick={handleLoadMore}
          >
            {loadingMore ? 'Loading…' : 'Load more'}
          </button>
        </div>
      )}

      {selectedEntry && (
        <div className="request-log-detail">
          <div className="request-log-detail-header">
            <strong>Request #{selectedEntry.id}</strong>
            <button type="button" className="logs-control-btn" onClick={() => void handleCopyRow()}>
              Copy JSON
            </button>
          </div>
          <dl className="request-log-detail-grid">
            <dt>Endpoint type</dt>
            <dd>{selectedEntry.endpoint_type ?? '—'}</dd>
            <dt>Forwarded for</dt>
            <dd>{selectedEntry.forwarded_for ?? '—'}</dd>
            <dt>User agent</dt>
            <dd>{selectedEntry.user_agent ?? '—'}</dd>
            <dt>Query string</dt>
            <dd>{selectedEntry.query_string ?? '—'}</dd>
            <dt>Request bytes</dt>
            <dd>{selectedEntry.request_body_bytes ?? '—'}</dd>
            <dt>Response bytes</dt>
            <dd>{selectedEntry.response_body_bytes ?? '—'}</dd>
            <dt>Prompt chars</dt>
            <dd>{selectedEntry.prompt_chars ?? '—'}</dd>
            <dt>Messages chars</dt>
            <dd>{selectedEntry.messages_chars ?? '—'}</dd>
            <dt>Error</dt>
            <dd>{selectedEntry.error ?? '—'}</dd>
          </dl>
          {selectedEntry.redacted_body !== null && selectedEntry.redacted_body !== undefined && (
            <pre className="request-log-json">
              {JSON.stringify(selectedEntry.redacted_body, null, 2)}
            </pre>
          )}
        </div>
      )}
    </div>
  );
};

export default RequestLogPanel;
