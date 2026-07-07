import React, { useRef, useEffect, useState } from 'react';
import { type Trace, inspectStore } from '../../inspectStore';
import { Icon } from '../Icon';

interface TraceListProps {
  traces: Trace[];
  filteredTraces: Trace[];
  selectedTraceId: string | null;
  capturing: boolean;
  captureReady?: 'disconnected' | 'connecting' | 'ready' | 'unsupported';
  searchQuery: string;
  filterKind: 'All' | 'LLM' | 'EMBEDDING' | 'RERANKER' | 'Errors';
  handleOpenCreateModal: () => void;
  handleExportSession: () => void;
  formatTokens: (num: number) => string;
}

export function getRelativeTimeAgo(startTimeMs: number): string {
  const diffSeconds = Math.max(0, Math.floor((Date.now() - startTimeMs) / 1000));
  if (diffSeconds < 5) return 'Just now';
  if (diffSeconds < 60) return `${diffSeconds}s ago`;
  const diffMinutes = Math.floor(diffSeconds / 60);
  if (diffMinutes < 60) return `${diffMinutes}m ago`;
  const diffHours = Math.floor(diffMinutes / 60);
  if (diffHours < 24) return `${diffHours}h ago`;
  return new Date(startTimeMs).toLocaleDateString();
}

export default function TraceList({
  traces,
  filteredTraces,
  selectedTraceId,
  capturing,
  captureReady,
  searchQuery,
  filterKind,
  handleOpenCreateModal,
  handleExportSession,
  formatTokens
}: TraceListProps) {
  const listboxRef = useRef<HTMLDivElement>(null);
  const [activeTraceId, setActiveTraceId] = useState<string | null>(selectedTraceId);

  // Sync activeTraceId when selectedTraceId changes
  useEffect(() => {
    setActiveTraceId(selectedTraceId);
  }, [selectedTraceId]);

  const currentActiveId = activeTraceId && filteredTraces.some((t) => t.id === activeTraceId)
    ? activeTraceId
    : (filteredTraces[0]?.id || null);

  const handleListboxKeyDown = (e: React.KeyboardEvent) => {
    const options = listboxRef.current?.querySelectorAll<HTMLElement>('[role="option"]');
    if (!options?.length) return;

    const focusedEl = document.activeElement as HTMLElement;
    const items = Array.from(options);
    const currentIdx = items.indexOf(focusedEl);

    let next = -1;
    if (e.key === 'ArrowDown') {
      e.preventDefault();
      next = currentIdx < 0 ? 0 : Math.min(currentIdx + 1, items.length - 1);
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      next = currentIdx <= 0 ? 0 : currentIdx - 1;
    } else if (e.key === 'Home') {
      e.preventDefault();
      next = 0;
    } else if (e.key === 'End') {
      e.preventDefault();
      next = items.length - 1;
    } else if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      if (currentIdx >= 0) {
        const traceId = items[currentIdx].getAttribute('data-trace-id');
        if (traceId) {
          inspectStore.selectTrace(traceId);
        }
      }
      return;
    }

    if (next >= 0) {
      items[next].focus();
      const traceId = items[next].getAttribute('data-trace-id');
      if (traceId) {
        setActiveTraceId(traceId);
      }
    }
  };

  // Ensure that listbox roving focus behaves correctly
  useEffect(() => {
    if (currentActiveId && listboxRef.current) {
      const activeOption = listboxRef.current.querySelector<HTMLElement>(`[data-trace-id="${currentActiveId}"]`);
      if (activeOption && document.activeElement && listboxRef.current.contains(document.activeElement)) {
        activeOption.focus();
      }
    }
  }, [currentActiveId]);

  const getBadgeClass = () => {
    if (!capturing) return 'paused';
    if (captureReady === 'connecting') return 'connecting';
    if (captureReady === 'unsupported') return 'unsupported';
    return 'capturing';
  };

  const getBadgeLabel = () => {
    if (!capturing) return 'Paused';
    if (captureReady === 'connecting') return 'Connecting...';
    if (captureReady === 'unsupported') return 'Unsupported';
    return `Capturing · ${traces.length}`;
  };

  return (
    <div className="inspect-rail">
      <div className="inspect-rail__header">
        <div className="inspect-rail__title-row">
          <h3>Session Inspector</h3>
          <span className={`capture-badge ${getBadgeClass()}`}>
            <span className="capture-badge__dot"></span>
            {getBadgeLabel()}
          </span>
        </div>
        <span className="inspect-rail__subtitle">Local history — persisted in browser storage, never on server</span>

        <div className="inspect-rail__capture-group-row">
          <div className="inspect-rail__capture-label-group">
            <span className="inspect-rail__capture-label">Auto-capture inferences</span>
            <span className="inspect-rail__capture-sublabel">Enables OTel on demand — no server-side storage</span>
          </div>
          <button
            type="button"
            role="switch"
            aria-checked={capturing}
            className={`switch-control ${capturing ? 'active' : ''}`}
            onClick={() => inspectStore.toggleCapture()}
            aria-label="Toggle auto-capture"
          >
            <span className="switch-control__thumb"></span>
          </button>
        </div>

        <div className="inspect-rail__search-row">
          <input
            type="text"
            placeholder="Search model, trace ID, content..."
            value={searchQuery}
            onChange={(e) => inspectStore.setSearchQuery(e.target.value)}
            aria-label="Search traces"
            className="inspect-search-input"
          />
        </div>

        <div className="inspect-rail__filters">
          {(['All', 'LLM', 'EMBEDDING', 'RERANKER', 'Errors'] as const).map((k) => (
            <button
              key={k}
              type="button"
              aria-pressed={filterKind === k}
              className={`filter-chip ${filterKind === k ? 'active' : ''}`}
              onClick={() => inspectStore.setFilterKind(k)}
            >
              {k === 'EMBEDDING' ? 'Embed' : k === 'RERANKER' ? 'Rerank' : k}
            </button>
          ))}
        </div>
      </div>

      <div
        ref={listboxRef}
        className="inspect-rail__list"
        role="listbox"
        aria-label="Trace runs"
        tabIndex={-1}
        onKeyDown={handleListboxKeyDown}
      >
        {filteredTraces.length === 0 ? (
          <div className="inspect-empty-state">
            <span className="inspect-empty-state__glyph" style={{ display: 'inline-flex', alignItems: 'center', justifyContent: 'center' }}>
              <Icon name="search-check" size={32} />
            </span>
            <p>No captured requests yet</p>
            <span className="inspect-empty-state__hint">
              Run prompts in the Chat view to capture live requests here.
            </span>
          </div>
        ) : (
          filteredTraces.map((t) => {
            const statusLabel = t.status === 'ok' ? 'OK' : t.status.charAt(0).toUpperCase() + t.status.slice(1);
            const durationFormatted = t.kind === 'LLM'
              ? (t.ttft ? `${Math.round(t.ttft)}ms` : '—')
              : `${t.dur}ms`;
            const tokensFormatted = t.kind === 'LLM'
              ? formatTokens(t.completion ?? 0)
              : formatTokens(t.prompt ?? 0);

            const timeStr = getRelativeTimeAgo(t.startTimeMs);

            return (
              <button
                key={t.id}
                type="button"
                role="option"
                data-trace-id={t.id}
                tabIndex={currentActiveId === t.id ? 0 : -1}
                aria-selected={selectedTraceId === t.id}
                aria-current={selectedTraceId === t.id ? 'true' : undefined}
                aria-label={`Trace: ${t.model}, ${t.kind}, status ${statusLabel}, duration ${durationFormatted}, tokens ${tokensFormatted}, captured ${timeStr}`}
                className={`trace-row ${selectedTraceId === t.id ? 'selected' : ''} ${t.status}`}
                onClick={() => {
                  setActiveTraceId(t.id);
                  inspectStore.selectTrace(t.id);
                }}
                onFocus={() => {
                  setActiveTraceId(t.id);
                }}
              >
                <div className="trace-row__meta">
                  <span className="trace-row__model" title={t.model}>
                    {t.model}
                  </span>
                  <span className="trace-row__time">{timeStr}</span>
                </div>
                <div className="trace-row__details">
                  <div className="trace-row__kind-badge-group">
                    <span className={`trace-row__kind ${t.kind.toLowerCase()}`}>
                      {t.kind}
                    </span>
                    {t.synthetic && (
                      <span className="mock-badge">
                        Mock
                      </span>
                    )}
                    <span className={`trace-row__status-dot ${t.status}`} aria-hidden="true"></span>
                    <span className="trace-row__status-label">{statusLabel}</span>
                  </div>
                  <span className="trace-row__metrics">
                    {t.kind === 'LLM' ? (
                      <>
                        {t.ttft ? `${Math.round(t.ttft)}ms` : '—'} · {formatTokens(t.completion ?? 0)}
                      </>
                    ) : (
                      <>
                        {formatTokens(t.prompt ?? 0)} · {t.dur}ms
                      </>
                    )}
                  </span>
                </div>
              </button>
            );
          })
        )}
      </div>

      <div className="inspect-rail__footer">
        <button
          type="button"
          className="inspect-footer-btn primary-simulate"
          onClick={handleOpenCreateModal}
        >
          + Create
        </button>
        <button
          type="button"
          className="inspect-footer-btn outline"
          onClick={() => inspectStore.clearSession()}
        >
          Clear
        </button>
        <button
          type="button"
          className="inspect-footer-btn outline"
          onClick={handleExportSession}
        >
          Export
        </button>
      </div>
    </div>
  );
}
