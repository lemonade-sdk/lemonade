import React, { useCallback, useEffect, useRef, useState } from 'react';
import type { LoadedModel } from '../../api';
import { autoOptStore, AutoOptState } from './autoOptStore';
import { AutoOptRunRecord, isAutoOptRunActive } from './autoOptTypes';
import AutoOptWizard from './AutoOptWizard';
import AutoOptRunDetail from './AutoOptRunDetail';
import { Icon } from '../../components/Icon';
import WorkspaceRailHeader from '../../components/WorkspaceRailHeader';
import { WorkspaceActionButton } from '../../components/WorkspacePanels';

export const AUTOOPT_OPEN_RUN_EVENT = 'lemonade:autoopt-open-run';

export function openAutoOptRun(runId: string): void {
  window.dispatchEvent(new CustomEvent(AUTOOPT_OPEN_RUN_EVENT, { detail: { id: runId } }));
}

export type PresetLibraryFilter = 'all' | 'mine' | 'starters' | 'applied';

function runDate(run: AutoOptRunRecord): string {
  const raw = run.finished_at || run.created_at;
  const parsed = Date.parse(raw || '');
  if (!Number.isFinite(parsed) || parsed <= 0) return '';
  return new Date(parsed).toLocaleDateString();
}

function firstLine(text: string | undefined): string {
  return String(text || '').split('\n')[0].trim();
}

function statusChip(run: AutoOptRunRecord): React.ReactNode {
  switch (run.status) {
    case 'queued':
      return <span className="autoopt-status-chip autoopt-status-chip--queued"><span className="autoopt-status-dot" aria-hidden="true" />Queued</span>;
    case 'running':
      return (
        <span className="autoopt-status-chip autoopt-status-chip--running">
          <span className="autoopt-spinner" aria-hidden="true" />
          {run.progress
            ? `${run.progress.stage} · ${run.progress.stage_index + 1}/${run.progress.stage_count}`
            : 'Running'}
        </span>
      );
    case 'completed':
      return <span className="autoopt-status-chip autoopt-status-chip--completed">{runDate(run) || 'Completed'}</span>;
    case 'failed':
      return (
        <span className="autoopt-status-chip autoopt-status-chip--failed">
          <span className="autoopt-status-x" aria-hidden="true">✕</span>
          {firstLine(run.error) || 'Failed'}
        </span>
      );
    case 'cancelled':
      return <span className="autoopt-status-chip autoopt-status-chip--cancelled">Cancelled</span>;
  }
}

const AutoOptRail: React.FC<{
  loadedModels: LoadedModel[];
  collapsed: boolean;
  onToggleCollapsed: () => void;
  mobileOpen: boolean;
  onMobileClose: () => void;
  railRef: React.Ref<HTMLElement>;
  libraryFilter: PresetLibraryFilter;
  onLibraryFilterChange: (filter: PresetLibraryFilter) => void;
  userPresetCount: number;
  starterCount: number;
  appliedCount: number;
}> = ({ loadedModels, collapsed, onToggleCollapsed, mobileOpen, onMobileClose, railRef, libraryFilter, onLibraryFilterChange, userPresetCount, starterCount, appliedCount }) => {
  const [state, setState] = useState<AutoOptState>(() => autoOptStore.snapshot());
  const [selectedRunId, setSelectedRunId] = useState('');
  const [wizardOpen, setWizardOpen] = useState(false);
  const [detailRunId, setDetailRunId] = useState<string | null>(null);
  const [announcement, setAnnouncement] = useState('');
  const previousStatuses = useRef<Map<string, AutoOptRunRecord['status']>>(new Map());

  useEffect(() => autoOptStore.subscribe(setState), []);

  useEffect(() => {
    const previous = previousStatuses.current;
    for (const run of state.runs) {
      const before = previous.get(run.id);
      if (before && before !== run.status) {
        if (run.status === 'completed') setAnnouncement(`AutoOpt run for ${run.model} completed.`);
        else if (run.status === 'failed') setAnnouncement(`AutoOpt run for ${run.model} failed${run.error ? `: ${firstLine(run.error)}` : '.'}`);
      }
    }
    previousStatuses.current = new Map(state.runs.map(run => [run.id, run.status]));
  }, [state.runs]);

  useEffect(() => {
    if (!selectedRunId && state.runs.length > 0) setSelectedRunId(state.runs[0].id);
    else if (selectedRunId && !state.runs.some(run => run.id === selectedRunId)) {
      setSelectedRunId(state.runs[0]?.id || '');
    }
  }, [state.runs, selectedRunId]);

  const openDetail = useCallback((runId: string) => {
    autoOptStore.setActiveRun(runId);
    setDetailRunId(runId);
  }, []);

  const closeDetail = useCallback(() => {
    autoOptStore.setActiveRun(null);
    setDetailRunId(null);
  }, []);

  const deleteRun = useCallback((runId: string) => {
    const previousSelection = selectedRunId;
    try {
      autoOptStore.deleteRun(runId);
    } catch {
      setSelectedRunId(previousSelection);
    }
  }, [selectedRunId]);

  useEffect(() => {
    const onOpenRun = (event: Event) => {
      const id = String((event as CustomEvent).detail?.id || '');
      if (id) openDetail(id);
    };
    window.addEventListener(AUTOOPT_OPEN_RUN_EVENT, onOpenRun as EventListener);
    return () => window.removeEventListener(AUTOOPT_OPEN_RUN_EVENT, onOpenRun as EventListener);
  }, [openDetail]);

  return (
    <>
      <aside
        ref={railRef}
        id="preset-filters-panel"
        className={`workspace-rail mobile-context-panel context-rail context-rail--autoopt${collapsed && !mobileOpen ? ' is-collapsed' : ''}${mobileOpen ? ' is-mobile-open' : ''}`}
        aria-label="Preset filters"
        role={mobileOpen ? 'dialog' : undefined}
        aria-modal={mobileOpen ? true : undefined}
      >
        <WorkspaceRailHeader
          title="Filters"
          sidebarLabel="preset filters"
          purpose="filter"
          collapsed={collapsed && !mobileOpen}
          onToggle={onToggleCollapsed}
          onMobileClose={mobileOpen ? onMobileClose : undefined}
        />
        <div className="context-rail__body">
          <nav className="workspace-filter-list preset-library-nav" aria-label="Preset filters">
            {([
              ['all', 'Library', 'All available presets', 'library', userPresetCount + starterCount],
              ['mine', 'My presets', 'Created and imported', 'user-round-cog', userPresetCount],
              ['starters', 'Starter library', 'Bundled foundations', 'gem', starterCount],
              ['applied', 'Applied models', 'Staged model intent', 'hard-drive', appliedCount],
            ] as const).map(([id, label, description, icon, count]) => (
              <button key={id} type="button" className={`workspace-filter-list__item${libraryFilter === id ? ' is-active' : ''}`} aria-current={libraryFilter === id ? 'true' : undefined} aria-label={label} title={`${label} — ${description}`} onClick={() => onLibraryFilterChange(id)}>
                <Icon className="workspace-filter-list__icon" name={icon} size={14} />
                <span className="workspace-filter-list__label">{label}</span>
                <span className="workspace-filter-list__count">{count}</span>
              </button>
            ))}
          </nav>
          <div className="context-rail__section-head">
            <span>Auto optimizer</span>
            <small>{state.runs.length}</small>
          </div>
          <p className="context-rail__hint">Benchmark a loaded model and save the winning runtime configuration as model tuning.</p>
          <WorkspaceActionButton
            appearance="primary"
            size="small"
            icon="play"
            onClick={() => setWizardOpen(true)}
            data-autoopt-run-optimizer
          >
            Run optimizer
          </WorkspaceActionButton>
          <p className="sr-only" role="status" aria-live="polite" aria-atomic="true" data-autoopt-announcement>{announcement}</p>
          {state.lastError && (
            <p className="context-rail__notice preset-error" data-autoopt-rail-error>⚠ {state.lastError}</p>
          )}
          <div className="auto-run-list" data-autoopt-run-list>
            {state.runs.length === 0 && (
              <p className="context-rail__hint" data-autoopt-empty>No optimization runs yet on this server.</p>
            )}
            {state.runs.map(run => {
              const active = isAutoOptRunActive(run);
              const cancelling = state.pendingCancel.has(run.id);
              return (
                <article key={run.id} className={`auto-run-card auto-run-card--${run.status}${selectedRunId === run.id ? ' is-active' : ''}`} data-autoopt-run={run.id}>
                  <button type="button" className="auto-run-card__main" onClick={() => setSelectedRunId(run.id)} aria-pressed={selectedRunId === run.id}>
                    <span className="auto-run-card__icon" aria-hidden="true">⚙️</span>
                    <span className="auto-run-card__text">
                      <strong>{run.model || run.id}</strong>
                      <span data-autoopt-run-status={run.status}>{statusChip(run)}</span>
                      {run.lemonade_version && <span>Lemonade {run.lemonade_version}</span>}
                    </span>
                  </button>
                  <div className="auto-run-card__actions">
                    <WorkspaceActionButton size="small" onClick={() => openDetail(run.id)} data-autoopt-inspect={run.id}>Inspect</WorkspaceActionButton>
                    {active ? (
                      <WorkspaceActionButton
                        size="small"
                        disabled={cancelling}
                        onClick={() => autoOptStore.cancelRun(run.id)}
                        aria-label={`Cancel AutoOpt run for ${run.model}`}
                        data-autoopt-cancel={run.id}
                      >
                        {cancelling ? 'Cancelling…' : 'Cancel'}
                      </WorkspaceActionButton>
                    ) : (
                      <WorkspaceActionButton
                        appearance="danger"
                        size="small"
                        icon="trash"
                        iconOnly
                        className="auto-run-card__delete-action"
                        onClick={() => deleteRun(run.id)}
                        aria-label={`Delete AutoOpt run for ${run.model}`}
                        title="Delete this run"
                        data-autoopt-delete={run.id}
                      />
                    )}
                  </div>
                </article>
              );
            })}
          </div>
        </div>
      </aside>

      <AutoOptWizard open={wizardOpen} onClose={() => setWizardOpen(false)} loadedModels={loadedModels} />
      <AutoOptRunDetail runId={detailRunId} onClose={closeDetail} />
    </>
  );
};

export default AutoOptRail;
