import React, { useRef, useEffect, useState } from 'react';
import { type Trace, inspectStore } from '../../inspectStore';
import { Icon, type CapabilityIconTarget } from '../Icon';
import WorkspaceRailHeader from '../WorkspaceRailHeader';
import {
  WorkspaceActionButton,
  WorkspaceList,
  WorkspaceListRow,
  type WorkspaceListRowStatus,
} from '../WorkspacePanels';

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
  collapsed: boolean;
  onToggleCollapsed: () => void;
  embedded?: boolean;
}

const TRACE_FILTERS = [
  ['All', 'All', 'globe', 'var(--text-tertiary)'],
  ['LLM', 'LLM', 'chat', 'var(--cap-chat)'],
  ['EMBEDDING', 'Embed', 'embedding', 'var(--cap-embedding)'],
  ['RERANKER', 'Rerank', 'reranking', 'var(--cap-reranking)'],
  ['Errors', 'Errors', 'alert', 'var(--danger)'],
] as const;

const TRACE_KIND_CAPABILITY: Record<Trace['kind'], CapabilityIconTarget> = {
  LLM: 'chat',
  EMBEDDING: 'embedding',
  RERANKER: 'reranking',
};

const TRACE_STATUS: Record<Trace['status'], WorkspaceListRowStatus> = {
  ok: 'ready',
  slow: 'attention',
  error: 'error',
};

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
  formatTokens,
  collapsed,
  onToggleCollapsed,
  embedded = false,
}: TraceListProps) {
  const listboxRef = useRef<HTMLUListElement>(null);
  const [activeTraceId, setActiveTraceId] = useState<string | null>(selectedTraceId);

  // Sync activeTraceId when selectedTraceId changes
  useEffect(() => {
    setActiveTraceId(selectedTraceId);
  }, [selectedTraceId]);

  const currentActiveId = activeTraceId && filteredTraces.some((t) => t.id === activeTraceId)
    ? activeTraceId
    : (filteredTraces[0]?.id || null);

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
    <div className={`inspect-rail ${embedded ? 'monitor-subpanel' : 'workspace-rail'}${collapsed ? ' is-collapsed' : ''}`}>
      {embedded ? (
        <header className="monitor-subpanel__header">
          <h2>Request history</h2>
          <p>{filteredTraces.length} of {traces.length} captured</p>
        </header>
      ) : (
        <WorkspaceRailHeader
          title="Requests"
          sidebarLabel="request history"
          purpose="history"
          collapsed={collapsed}
          onToggle={onToggleCollapsed}
        />
      )}
      <div className="inspect-rail__controls">
        <div className="inspect-rail__capture-group-row">
          <div className="inspect-rail__capture-label-group">
            <span className="inspect-rail__capture-label">Auto-capture inferences</span>
            <span className="inspect-rail__capture-sublabel">Enables OTel on demand — no server-side storage</span>
            <span className={`capture-badge ${getBadgeClass()}`}>
              <span className="capture-badge__dot"></span>
              {getBadgeLabel()}
            </span>
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

        <nav
          className="model-nav-rail__chip-list"
          aria-label="Request filters"
        >
          {TRACE_FILTERS.map(([kind, label, icon, color]) => {
            const active = filterKind === kind;

            return (
              <button
                key={kind}
                type="button"
                aria-pressed={active}
                className={`model-nav-rail__filter-chip model-nav-rail__task-chip${active ? ' is-active' : ''}`}
                style={{ '--filter-chip-color': color } as React.CSSProperties}
                onClick={() => inspectStore.setFilterKind(kind)}
              >
                <Icon
                  name={icon}
                  size={13}
                  className="model-nav-rail__task-icon"
                />
                <span>{label}</span>
              </button>
            );
          })}
        </nav>
      </div>

      {filteredTraces.length === 0 ? (
        <div className="inspect-rail__list">
          <div className="inspect-empty-state">
            <span className="inspect-empty-state__glyph">
              <Icon name="search-check" size={32} />
            </span>
            <p>No captured requests yet</p>
            <span className="inspect-empty-state__hint">
              Run prompts in the Chat view to capture live requests here.
            </span>
          </div>
        </div>
      ) : (
        <WorkspaceList
          listRef={listboxRef}
          className="inspect-rail__list"
          label="Trace runs"
          tabIndex={-1}
          onRowFocus={setActiveTraceId}
          onRowActivate={id => inspectStore.selectTrace(id)}
        >
          {filteredTraces.map((t) => {
            const statusLabel = t.status === 'ok' ? 'OK' : t.status.charAt(0).toUpperCase() + t.status.slice(1);
            const durationFormatted = t.kind === 'LLM'
              ? (t.ttft ? `${Math.round(t.ttft)}ms` : '—')
              : `${t.dur}ms`;
            const tokensFormatted = t.kind === 'LLM'
              ? formatTokens(t.completion ?? 0)
              : formatTokens(t.prompt ?? 0);

            const timeStr = getRelativeTimeAgo(t.startTimeMs);
            const metrics = t.kind === 'LLM'
              ? `${durationFormatted} · ${tokensFormatted}`
              : `${tokensFormatted} · ${durationFormatted}`;

            // "ok" is the nominal case and says nothing; every other state
            // names itself here so the dot is not the only carrier.
            const meta = t.status === 'error'
              ? `Failed${t.diag?.title ? ` · ${t.diag.title}` : ''}`
              : t.status === 'slow'
                ? `Slow · ${metrics}`
                : `${t.synthetic ? 'mock · ' : ''}${metrics}`;

            return (
              <WorkspaceListRow
                key={t.id}
                rowId={t.id}
                capability={TRACE_KIND_CAPABILITY[t.kind]}
                title={t.model}
                meta={meta}
                metaMono
                anchor={timeStr}
                status={TRACE_STATUS[t.status]}
                statusLabel={`Status ${statusLabel}`}
                selected={selectedTraceId === t.id}
                tabIndex={currentActiveId === t.id ? 0 : -1}
                dataAttributes={{ 'data-trace-id': t.id }}
                ariaLabel={`Trace: ${t.model}, ${t.kind}, status ${statusLabel}, duration ${durationFormatted}, tokens ${tokensFormatted}, captured ${timeStr}`}
                onClick={() => {
                  setActiveTraceId(t.id);
                  inspectStore.selectTrace(t.id);
                }}
                onFocus={() => {
                  setActiveTraceId(t.id);
                }}
                action={{
                  icon: 'trash',
                  label: `Delete request: ${t.model}, captured ${timeStr}`,
                  onClick: () => inspectStore.removeTrace(t.id),
                }}
              />
            );
          })}
        </WorkspaceList>
      )}

      <div className="inspect-rail__footer">
        <WorkspaceActionButton
          appearance="primary"
          size="small"
          icon="compose"
          onClick={handleOpenCreateModal}
        >
          Create
        </WorkspaceActionButton>
        <WorkspaceActionButton
          appearance="danger"
          size="small"
          icon="trash"
          onClick={() => inspectStore.clearSession()}
        >
          Clear
        </WorkspaceActionButton>
        <WorkspaceActionButton
          appearance="secondary"
          size="small"
          icon="copy"
          onClick={handleExportSession}
        >
          Export
        </WorkspaceActionButton>
      </div>
    </div>
  );
}
