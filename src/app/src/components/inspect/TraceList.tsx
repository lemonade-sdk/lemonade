import React, { useRef, useEffect, useState } from 'react';
import { type Trace, inspectStore } from '../../inspectStore';
import { Icon, type CapabilityIconTarget } from '../Icon';
import WorkspaceRailHeader from '../WorkspaceRailHeader';
import { useI18n } from '../../i18n';
import { traceDiagnosticText, traceStatusLabel } from './tracePresentation';
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
  ['All', 'traces.filters.all', 'globe', 'var(--text-tertiary)'],
  ['LLM', 'traces.filters.llm', 'chat', 'var(--cap-chat)'],
  ['EMBEDDING', 'traces.filters.embedding', 'embedding', 'var(--cap-embedding)'],
  ['RERANKER', 'traces.filters.reranker', 'reranking', 'var(--cap-reranking)'],
  ['Errors', 'traces.filters.errors', 'alert', 'var(--danger)'],
] as const;

const TRACE_KIND_CAPABILITY: Record<Trace['kind'], CapabilityIconTarget> = {
  LLM: 'chat',
  EMBEDDING: 'embedding',
  RERANKER: 'reranking',
};

/* A successful request has nothing in flight and nothing to warn about, so it
   shows no status at all — its metrics stand on their own. */
const TRACE_STATUS: Record<Trace['status'], WorkspaceListRowStatus | undefined> = {
  ok: undefined,
  slow: 'attention',
  error: 'error',
};


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
  const { t: tr, formatRelativeTime } = useI18n('inspect');
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
    if (!capturing) return tr('traces.capture.paused');
    if (captureReady === 'connecting') return tr('traces.capture.connecting');
    if (captureReady === 'unsupported') return tr('traces.capture.unsupported');
    return tr('traces.capture.capturing', { count: traces.length });
  };

  return (
    <div className={`inspect-rail ${embedded ? 'monitor-subpanel' : 'workspace-rail'}${collapsed ? ' is-collapsed' : ''}`}>
      {embedded ? (
        <header className="monitor-subpanel__header">
          <h2>{tr('traces.history')}</h2>
          <p>{tr('traces.capturedCount', { filtered: filteredTraces.length, total: traces.length })}</p>
        </header>
      ) : (
        <WorkspaceRailHeader
          title={tr('traces.requests')}
          sidebarLabel={tr('traces.sidebar')}
          purpose="history"
          collapsed={collapsed}
          onToggle={onToggleCollapsed}
        />
      )}
      <div className="inspect-rail__controls">
        <div className="inspect-rail__capture-group-row">
          <div className="inspect-rail__capture-label-group">
            <span className="inspect-rail__capture-label">{tr('traces.capture.label')}</span>
            <span className="inspect-rail__capture-sublabel">{tr('traces.capture.hint')}</span>
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
            aria-label={tr('traces.capture.toggle')}
          >
            <span className="switch-control__thumb"></span>
          </button>
        </div>

        <div className="inspect-rail__search-row">
          <input
            type="text"
            placeholder={tr('traces.search')}
            value={searchQuery}
            onChange={(e) => inspectStore.setSearchQuery(e.target.value)}
            aria-label={tr('traces.searchAria')}
            className="inspect-search-input"
          />
        </div>

        <nav
          className="model-nav-rail__chip-list"
          aria-label={tr('traces.filtersAria')}
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
                <span>{tr(label)}</span>
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
            <p>{tr('traces.empty')}</p>
            <span className="inspect-empty-state__hint">
              {tr('traces.emptyHint')}
            </span>
          </div>
        </div>
      ) : (
        <WorkspaceList
          listRef={listboxRef}
          className="inspect-rail__list"
          label={tr('traces.listAria')}
          tabIndex={-1}
          onRowFocus={setActiveTraceId}
          onRowActivate={id => inspectStore.selectTrace(id)}
        >
          {filteredTraces.map((t) => {
            const statusLabel = traceStatusLabel(t.status, tr);
            const diagnosticText = traceDiagnosticText(t.diag, t.dur, tr);
            const durationFormatted = t.kind === 'LLM'
              ? (t.ttft ? `${Math.round(t.ttft)}ms` : '—')
              : `${t.dur}ms`;
            const tokensFormatted = t.kind === 'LLM'
              ? formatTokens(t.completion ?? 0)
              : formatTokens(t.prompt ?? 0);

            const diffSeconds = Math.max(0, Math.floor((Date.now() - t.startTimeMs) / 1000));
            const timeStr = diffSeconds < 60
              ? formatRelativeTime(-diffSeconds, 'second', { numeric: 'auto' })
              : diffSeconds < 3600
                ? formatRelativeTime(-Math.floor(diffSeconds / 60), 'minute', { numeric: 'auto' })
                : formatRelativeTime(-Math.floor(diffSeconds / 3600), 'hour', { numeric: 'auto' });
            const metrics = t.kind === 'LLM'
              ? `${durationFormatted} · ${tokensFormatted}`
              : `${tokensFormatted} · ${durationFormatted}`;

            // A captured request that succeeded has nothing to report beyond
            // its metrics, and DESIGN.md rules out a redundant "OK" here. A slow
            // one keeps its metrics in the status line — they are the evidence,
            // and that is exactly when they matter most.
            const statusText = t.status === 'error'
              ? `${tr('traces.failed')}${diagnosticText ? ` · ${diagnosticText.title}` : ''}`
              : t.status === 'slow' ? `${tr('traces.slow')} · ${metrics}` : undefined;
            const meta = [t.synthetic && 'mock', metrics].filter(Boolean);

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
                statusText={statusText}
                statusLabel={tr('traces.status', { status: statusLabel })}
                selected={selectedTraceId === t.id}
                tabIndex={currentActiveId === t.id ? 0 : -1}
                dataAttributes={{ 'data-trace-id': t.id }}
                ariaLabel={tr('traces.rowAria', { model: t.model, kind: t.kind, status: statusLabel, duration: durationFormatted, tokens: tokensFormatted, time: timeStr })}
                onClick={() => {
                  setActiveTraceId(t.id);
                  inspectStore.selectTrace(t.id);
                }}
                onFocus={() => {
                  setActiveTraceId(t.id);
                }}
                action={{
                  icon: 'trash',
                  label: tr('traces.deleteAria', { model: t.model, time: timeStr }),
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
          {tr('traces.create')}
        </WorkspaceActionButton>
        <WorkspaceActionButton
          appearance="danger"
          size="small"
          icon="trash"
          onClick={() => inspectStore.clearSession()}
        >
          {tr('traces.clear')}
        </WorkspaceActionButton>
        <WorkspaceActionButton
          appearance="secondary"
          size="small"
          icon="copy"
          onClick={handleExportSession}
        >
          {tr('traces.export')}
        </WorkspaceActionButton>
      </div>
    </div>
  );
}
