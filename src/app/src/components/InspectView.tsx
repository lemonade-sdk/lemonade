import React, { useState, useMemo, useEffect, useRef } from 'react';
import { useInspectStore, inspectStore, type Trace } from '../inspectStore';
import api from '../api';
import { Icon } from './Icon';
import { useCopyToClipboard } from '../hooks/useCopyToClipboard';

// Subcomponents
import TraceList from './inspect/TraceList';
import OverviewTab from './inspect/OverviewTab';
import MessagesTab from './inspect/MessagesTab';
import ReplayTab from './inspect/ReplayTab';
import ImproveTab from './inspect/ImproveTab';
import CreateModal from './inspect/CreateModal';
import CurlModal from './inspect/CurlModal';
import { type AccountSession } from '../features/accounts/accountStore';

interface InspectViewProps {
  accountSession: AccountSession;
  embedded?: boolean;
}

export default function InspectView({ accountSession, embedded = false }: InspectViewProps) {
  const { traces, selectedTraceId, capturing, captureReady, searchQuery, filterKind, toast } = useInspectStore();
  const [activeTab, setActiveTab] = useState<'overview' | 'messages' | 'replay' | 'improve'>('overview');
  const [railCollapsed, setRailCollapsed] = useState(false);

  // Shared Replay params state for ReplayTab and CurlModal preview sync
  const [replaySystemPrompt, setReplaySystemPrompt] = useState('');
  const [replayTemp, setReplayTemp] = useState(0.7);
  const [replayTopP, setReplayTopP] = useState(1.0);
  const [replayTopK, setReplayTopK] = useState(50);
  const [replayMaxTokens, setReplayMaxTokens] = useState(1024);

  // Modals state
  const [createModalOpen, setCreateModalOpen] = useState(false);
  const [curlModalOpen, setCurlModalOpen] = useState(false);

  const tablistRef = useRef<HTMLDivElement>(null);

  const availableModels = api.allModels;

  const selectedTrace = useMemo(() => {
    return traces.find((t) => t.id === selectedTraceId) || null;
  }, [selectedTraceId, traces]);

  // Sync tab active-tab reset on trace change
  useEffect(() => {
    setActiveTab('overview');
  }, [selectedTraceId]);

  // Sync replay initial values when selected trace changes
  useEffect(() => {
    if (selectedTrace) {
      const sysMsg = selectedTrace.messages.find((m) => m.role === 'system');
      setReplaySystemPrompt(sysMsg ? sysMsg.content : '');
      setReplayTemp(selectedTrace.temp ?? 0.7);
      setReplayTopP(selectedTrace.topP ?? 1.0);
      setReplayTopK(selectedTrace.topK ?? 50);
      setReplayMaxTokens(selectedTrace.max ?? 1024);
    } else {
      setReplaySystemPrompt('');
      setReplayTemp(0.7);
      setReplayTopP(1.0);
      setReplayTopK(50);
      setReplayMaxTokens(1024);
    }
  }, [selectedTraceId]);

  // Keyboard navigation for tab list (ratios, ArrowLeft/Right)
  const handleTabKeyDown = (e: React.KeyboardEvent, tabs: string[]) => {
    const currentIdx = tabs.indexOf(activeTab);
    let next = -1;
    if (e.key === 'ArrowRight') {
      e.preventDefault();
      next = (currentIdx + 1) % tabs.length;
    } else if (e.key === 'ArrowLeft') {
      e.preventDefault();
      next = (currentIdx - 1 + tabs.length) % tabs.length;
    } else if (e.key === 'Home') {
      e.preventDefault();
      next = 0;
    } else if (e.key === 'End') {
      e.preventDefault();
      next = tabs.length - 1;
    }

    if (next >= 0) {
      const nextTab = tabs[next];
      setActiveTab(nextTab as any);
      setTimeout(() => {
        const nextButton = tablistRef.current?.querySelector<HTMLButtonElement>(`[data-tab-name="${nextTab}"]`);
        nextButton?.focus();
      }, 0);
    }
  };

  const formatTokens = (num: number) => {
    if (num >= 1000) {
      return `${(num / 1000).toFixed(1).replace(/\.0$/, '')}k tok`;
    }
    return `${num} tok`;
  };

  const { copy: copyToClipboard } = useCopyToClipboard();

  const handleCopyFull = (text: string, label: string) => {
    copyToClipboard(text, label);
  };

  const handleExportSession = () => {
    // Export only non-synthetic traces to keep files clean and small
    const exportable = traces.filter(t => !t.synthetic);
    const dataStr = JSON.stringify(exportable, null, 2);
    navigator.clipboard.writeText(dataStr);
    inspectStore.showToast(`Session exported (${exportable.length} traces copied)`);
  };

  const filteredTraces = useMemo(() => {
    return traces.filter((t) => {
      // 1. Kind filter
      if (filterKind === 'Errors' && t.status !== 'error') return false;
      if (filterKind !== 'All' && filterKind !== 'Errors' && t.kind !== filterKind) return false;

      // 2. Search query substring check
      if (searchQuery) {
        const q = searchQuery.toLowerCase();
        const matchesModel = t.model.toLowerCase().includes(q);
        const matchesId = t.traceId.toLowerCase().includes(q);
        const matchesMessages = t.messages.some((m) => m.content.toLowerCase().includes(q));
        const matchesOutput = t.output.toLowerCase().includes(q);
        return matchesModel || matchesId || matchesMessages || matchesOutput;
      }
      return true;
    });
  }, [traces, filterKind, searchQuery]);

  return (
    <div className={`inspect-layout${embedded ? ' inspect-layout--embedded' : ''}${!embedded && railCollapsed ? ' workspace--rail-collapsed' : ''}`}>
      <TraceList
        traces={traces}
        filteredTraces={filteredTraces}
        selectedTraceId={selectedTraceId}
        capturing={capturing}
        captureReady={captureReady}
        searchQuery={searchQuery}
        filterKind={filterKind}
        handleOpenCreateModal={() => setCreateModalOpen(true)}
        handleExportSession={handleExportSession}
        formatTokens={formatTokens}
        collapsed={!embedded && railCollapsed}
        onToggleCollapsed={() => setRailCollapsed(value => !value)}
        embedded={embedded}
      />

      <div className="inspect-detail">
        {!selectedTrace ? (
          <div className="inspect-detail-empty">
            <span className="inspect-detail-empty__icon">
              <Icon name="scan-eye" size={48} />
            </span>
            <h4>Select a trace run to inspect details</h4>
            <p>Select any recorded inference from the left panel to review its timeline waterfall, prompts, metrics and optimization suggestions.</p>
          </div>
        ) : (
          <>
            <div className="inspect-detail__header">
              <div className="inspect-detail__identity">
                <span className={`detail-kind-badge ${selectedTrace.kind.toLowerCase()}`}>{selectedTrace.kind}</span>
                <h2 className="detail-model-name">{selectedTrace.model}</h2>
                {selectedTrace.operation && (
                  <span className="detail-operation">{selectedTrace.operation}</span>
                )}
                <div style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 'var(--space-2)' }}>
                  <span className={`trace-row__status-dot ${selectedTrace.status}`} aria-hidden="true"></span>
                  <span className="trace-row__status-label">
                    {selectedTrace.status === 'ok' ? 'OK' : selectedTrace.status.charAt(0).toUpperCase() + selectedTrace.status.slice(1)}
                  </span>
                </div>
              </div>

              <div className="inspect-metrics-strip">
                <div className="metric-card">
                  <span className="metric-card__label">TTFT</span>
                  <span className="metric-card__val">
                    {selectedTrace.ttft ? (
                      <>
                        {Math.round(selectedTrace.ttft)}
                        <span className="metric-card__unit"> ms</span>
                      </>
                    ) : '—'}
                  </span>
                </div>
                <div className="metric-card">
                  <span className="metric-card__label">THROUGHPUT</span>
                  <span className="metric-card__val">
                    {selectedTrace.tps ? (
                      <>
                        {Number(selectedTrace.tps).toFixed(1)}
                        <span className="metric-card__unit"> tok/s</span>
                      </>
                    ) : '—'}
                  </span>
                </div>
                <div className="metric-card">
                  <span className="metric-card__label">INPUT</span>
                  <span className="metric-card__val">
                    {selectedTrace.prompt !== undefined ? (
                      <>
                        {selectedTrace.prompt}
                        <span className="metric-card__unit"> tok</span>
                      </>
                    ) : '—'}
                  </span>
                </div>
                <div className="metric-card">
                  <span className="metric-card__label">OUTPUT</span>
                  <span className="metric-card__val">
                    {selectedTrace.completion !== undefined ? (
                      <>
                        {selectedTrace.completion}
                        <span className="metric-card__unit"> tok</span>
                      </>
                    ) : '—'}
                  </span>
                </div>
                <div className="metric-card highlight">
                  <span className="metric-card__label">TOTAL</span>
                  <span className="metric-card__val">
                    {selectedTrace.prompt !== undefined && selectedTrace.completion !== undefined ? (
                      <>
                        {selectedTrace.prompt + selectedTrace.completion}
                        <span className="metric-card__unit"> tok</span>
                      </>
                    ) : '—'}
                  </span>
                </div>
                <div className="metric-card">
                  <span className="metric-card__label">DURATION</span>
                  <span className="metric-card__val">
                    {selectedTrace.dur ? (
                      <>
                        {selectedTrace.dur}
                        <span className="metric-card__unit"> ms</span>
                      </>
                    ) : '—'}
                  </span>
                </div>
              </div>

              {/* Detail Tabs list */}
              <div
                ref={tablistRef}
                className="detail-tabs-list"
                role="tablist"
                onKeyDown={(e) => handleTabKeyDown(e, ['overview', 'messages', 'replay', 'improve'])}
              >
                {(['overview', 'messages', 'replay', 'improve'] as const).map((tab) => (
                  <button
                    key={tab}
                    type="button"
                    role="tab"
                    data-tab-name={tab}
                    id={`tab-${tab}`}
                    aria-selected={activeTab === tab}
                    aria-controls={`panel-${tab}`}
                    tabIndex={activeTab === tab ? 0 : -1}
                    className={`detail-tab ${activeTab === tab ? 'active' : ''}`}
                    onClick={() => setActiveTab(tab)}
                  >
                    {tab === 'improve' ? (
                      <span style={{ display: 'inline-flex', alignItems: 'center', gap: 'var(--space-1.5)' }}>
                        <Icon name="omni" size={14} /> Improve
                      </span>
                    ) : tab === 'replay' ? (
                      'Replay & compare'
                    ) : (
                      tab.charAt(0).toUpperCase() + tab.slice(1)
                    )}
                  </button>
                ))}
              </div>
            </div>

            {/* Tab Body Container */}
            <div className={`inspect-detail__body ${activeTab === 'replay' ? 'inspect-detail__body--replay' : ''}`}>
              {activeTab === 'overview' && (
                <OverviewTab selectedTrace={selectedTrace} setActiveTab={setActiveTab} />
              )}
              {activeTab === 'messages' && (
                <MessagesTab
                  selectedTrace={selectedTrace}
                  formatTokens={formatTokens}
                  handleCopyFull={handleCopyFull}
                />
              )}
              {activeTab === 'replay' && (
                <ReplayTab
                  selectedTrace={selectedTrace}
                  setCurlModalOpen={setCurlModalOpen}
                  replaySystemPrompt={replaySystemPrompt}
                  setReplaySystemPrompt={setReplaySystemPrompt}
                  replayTemp={replayTemp}
                  setReplayTemp={setReplayTemp}
                  replayTopP={replayTopP}
                  setReplayTopP={setReplayTopP}
                  replayTopK={replayTopK}
                  setReplayTopK={setReplayTopK}
                  replayMaxTokens={replayMaxTokens}
                  setReplayMaxTokens={setReplayMaxTokens}
                />
              )}
              {activeTab === 'improve' && (
                <ImproveTab selectedTrace={selectedTrace} />
              )}
            </div>
          </>
        )}
      </div>

      <CreateModal
        isOpen={createModalOpen}
        onClose={() => setCreateModalOpen(false)}
        availableModels={availableModels}
      />

      {selectedTrace && (
        <CurlModal
          isOpen={curlModalOpen}
          onClose={() => setCurlModalOpen(false)}
          selectedTrace={selectedTrace}
          replaySystemPrompt={replaySystemPrompt}
          replayTemp={replayTemp}
          replayTopP={replayTopP}
          replayTopK={replayTopK}
          replayMaxTokens={replayMaxTokens}
          handleCopyFull={handleCopyFull}
        />
      )}

      {/* Permanent invisible live region for screen readers */}
      <div className="sr-only" role="status" aria-live="polite" style={{ position: 'absolute', width: 1, height: 1, padding: 0, margin: -1, overflow: 'hidden', clip: 'rect(0, 0, 0, 0)', border: 0 }}>
        {toast || ''}
      </div>

      {/* Global Status Toast (Visual only) */}
      {toast && (
        <div className="inspect-toast-status" aria-hidden="true">
          {toast}
        </div>
      )}
    </div>
  );
}
