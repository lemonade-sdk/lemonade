import api from '../../api';
import {
  AUTOOPT_STAGES,
  ControllerOutcome,
  executeAutoOptRun,
  resumeAutoOptRun,
} from './autoOptController';
import {
  AutoOptRunRecord,
  AutoOptStage,
  AutoOptStartRequest,
  SynthInputs,
  isAutoOptRunActive,
} from './autoOptTypes';

export interface AutoOptState {
  runs: AutoOptRunRecord[];
  activeRunId: string | null;
  lastError: string | null;
  pendingCancel: Set<string>;
}

type Listener = (state: AutoOptState) => void;

const STORAGE_PREFIX = 'lemonade_autoopt_runs_v2';
const MAX_RUNS = 20;

function storageKey(): string {
  let base = 'default';
  try { base = api.baseUrl || 'default'; } catch {}
  const normalized = String(base).toLowerCase().replace(/\/+$/, '');
  return `${STORAGE_PREFIX}::${normalized}`;
}

function isoNow(): string {
  return new Date().toISOString().replace(/\.\d{3}Z$/, 'Z');
}

function makeRunId(): string {
  const now = new Date();
  const pad = (n: number) => String(n).padStart(2, '0');
  const stamp = `${now.getUTCFullYear()}${pad(now.getUTCMonth() + 1)}${pad(now.getUTCDate())}`
    + `-${pad(now.getUTCHours())}${pad(now.getUTCMinutes())}${pad(now.getUTCSeconds())}`;
  const suffix = Math.floor(Math.random() * 0xffff).toString(16).padStart(4, '0');
  return `ao-${stamp}-${suffix}`;
}

function freshStages(): AutoOptStage[] {
  return AUTOOPT_STAGES.map(name => ({ name, status: 'pending' as const }));
}

function coerceStoredRun(raw: unknown): AutoOptRunRecord | null {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  const run = raw as AutoOptRunRecord;
  if (!run.id || typeof run.id !== 'string') return null;
  const coerced: AutoOptRunRecord = {
    ...run,
    stages: Array.isArray(run.stages) ? run.stages : [],
    measurements: {
      fit: Array.isArray(run.measurements?.fit) ? run.measurements.fit : [],
      bench: Array.isArray(run.measurements?.bench) ? run.measurements.bench : [],
    },
  };
  delete coerced.progress;

  if (isAutoOptRunActive(coerced) && !(coerced.job_id && coerced.synth_inputs)) {
    coerced.status = 'failed';
    coerced.error = 'interrupted — the page was closed while the run was active';
    coerced.finished_at = coerced.finished_at || isoNow();
  }
  return coerced;
}

function readStoredRuns(key: string): AutoOptRunRecord[] {
  if (typeof localStorage === 'undefined') return [];
  try {
    const raw = localStorage.getItem(key);
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    const items = Array.isArray(parsed?.runs) ? parsed.runs : [];
    return items
      .map(coerceStoredRun)
      .filter((run: AutoOptRunRecord | null): run is AutoOptRunRecord => !!run)
      .slice(0, MAX_RUNS);
  } catch {
    return [];
  }
}

function writeStoredRuns(key: string, runs: AutoOptRunRecord[]): void {
  if (typeof localStorage === 'undefined') return;
  localStorage.setItem(key, JSON.stringify({ version: 2, runs: runs.slice(0, MAX_RUNS) }));
}

function errorMessage(err: unknown): string {
  if (err instanceof Error) return err.message;
  return String(err || 'Unknown error');
}

function runProgress(run: AutoOptRunRecord, detail: string | undefined): AutoOptRunRecord['progress'] {
  let completed = 0;
  let current = '';
  for (const stage of run.stages) {
    if (stage.status === 'completed' || stage.status === 'skipped') completed++;
    if (stage.status === 'running') current = stage.name;
  }
  return {
    stage: current || (run.stages.length ? run.stages[run.stages.length - 1].name : ''),
    stage_index: completed,
    stage_count: run.stages.length,
    ...(detail ? { detail } : {}),
  };
}

class AutoOptStore {
  private scopeKey = storageKey();
  private scopeGeneration = 0;
  private state: AutoOptState = {
    runs: readStoredRuns(this.scopeKey),
    activeRunId: null,
    lastError: null,
    pendingCancel: new Set(),
  };
  private listeners = new Set<Listener>();
  private controllers = new Map<string, AbortController>();
  private progressDetail = new Map<string, string>();

  constructor() {
    this.reattachActiveRuns();
    try {
      api.onStatusChange(() => this.syncStorageScope());
    } catch {}
  }

  private syncStorageScope(): void {
    let key: string;
    try {
      key = storageKey();
    } catch {
      return;
    }
    if (key === this.scopeKey) return;
    this.scopeKey = key;
    this.scopeGeneration++;
    for (const controller of this.controllers.values()) controller.abort();
    this.controllers.clear();
    this.progressDetail.clear();
    this.setState({
      runs: readStoredRuns(this.scopeKey),
      activeRunId: null,
      lastError: null,
      pendingCancel: new Set(),
    });
    this.reattachActiveRuns();
  }

  private reattachActiveRuns(): void {
    for (const run of this.state.runs) {
      if (!isAutoOptRunActive(run) || !run.job_id || !run.synth_inputs) continue;
      if (this.controllers.has(run.id)) continue;
      const controller = new AbortController();
      this.controllers.set(run.id, controller);
      void this.runController(run.id, controller, cb => resumeAutoOptRun(run, cb, controller.signal));
    }
  }

  snapshot(): AutoOptState {
    return this.state;
  }

  subscribe(listener: Listener): () => void {
    this.listeners.add(listener);
    listener(this.state);
    return () => {
      this.listeners.delete(listener);
    };
  }

  setActiveRun(id: string | null): void {
    if (this.state.activeRunId === id) return;
    this.setState({ activeRunId: id });
  }

  async startRun(request: AutoOptStartRequest): Promise<string> {
    if (this.state.runs.some(isAutoOptRunActive)) {
      throw new Error('an AutoOpt run is already active');
    }
    const modelInfo = api.allModels.find(model =>
      String((model as Record<string, unknown>).model_name || model.name || model.id || '').trim() === request.model);
    const run: AutoOptRunRecord = {
      id: makeRunId(),
      model: request.model,
      checkpoint: String((modelInfo as Record<string, unknown> | undefined)?.checkpoint || ''),
      budget: request.budget,
      answers: request.answers,
      allow_unload: request.allow_unload,
      status: 'running',
      created_at: isoNow(),
      lemonade_version: api.healthData?.version,
      stages: freshStages(),
      measurements: { fit: [], bench: [] },
    };
    run.progress = runProgress(run, undefined);
    this.setState({ runs: [run, ...this.state.runs].slice(0, MAX_RUNS), lastError: null });
    try {
      this.persist();
    } catch {}

    const controller = new AbortController();
    this.controllers.set(run.id, controller);
    void this.runController(run.id, controller, cb => executeAutoOptRun(request, controller.signal, cb));
    return run.id;
  }

  private async runController(
    runId: string,
    controller: AbortController,
    run: (cb: Parameters<typeof executeAutoOptRun>[2]) => Promise<ControllerOutcome>,
  ): Promise<void> {
    const gen = this.scopeGeneration;
    const alive = () => gen === this.scopeGeneration;
    const callbacks = {
      stage: (name: string, status: AutoOptStage['status'], patch?: Partial<AutoOptStage>) => {
        if (!alive()) return;
        this.updateRun(runId, r => {
          const stages = r.stages.some(stage => stage.name === name)
            ? r.stages.map(stage => stage.name === name
              ? { ...stage, ...patch, status, ...(patch?.data ? { data: { ...(stage.data || {}), ...patch.data } } : {}) }
              : stage)
            : [...r.stages, { name, status, ...patch }];
          const next = { ...r, stages };
          next.progress = runProgress(next, this.progressDetail.get(runId));
          return next;
        });
      },
      progress: (detail: string) => {
        if (!alive()) return;
        this.progressDetail.set(runId, detail);
        this.updateRun(runId, r => ({ ...r, progress: runProgress(r, detail) }), false);
      },
      fit: (estimate: AutoOptRunRecord['measurements']['fit'][number]) => {
        if (!alive()) return;
        this.updateRun(runId, r => ({
          ...r,
          measurements: { ...r.measurements, fit: [...r.measurements.fit, estimate] },
        }));
      },
      bench: (points: AutoOptRunRecord['measurements']['bench']) => {
        if (!alive()) return;
        this.updateRun(runId, r => ({
          ...r,
          measurements: { ...r.measurements, bench: points },
        }));
      },
      jobCreated: (jobId: string, synthInputs: SynthInputs) => {
        if (!alive()) return;
        this.updateRun(runId, r => ({ ...r, job_id: jobId, synth_inputs: synthInputs }));
      },
    };

    try {
      const outcome = await run(callbacks);
      if (alive()) this.updateRun(runId, run => {
        const next: AutoOptRunRecord = {
          ...run,
          status: controller.signal.aborted ? 'cancelled' : 'completed',
          finished_at: isoNow(),
          summary: outcome.summary,
          result: outcome.result,
        };
        delete next.progress;
        return next;
      });
    } catch (err) {
      const cancelled = controller.signal.aborted || (err as { name?: string } | null)?.name === 'AbortError';
      if (alive()) this.updateRun(runId, run => {
        const next: AutoOptRunRecord = {
          ...run,
          status: cancelled ? 'cancelled' : 'failed',
          error: cancelled ? 'cancelled by user' : errorMessage(err),
          finished_at: isoNow(),
        };
        delete next.progress;
        return next;
      });
    } finally {
      this.controllers.delete(runId);
      this.progressDetail.delete(runId);
      if (alive()) {
        const pendingCancel = new Set(this.state.pendingCancel);
        if (pendingCancel.delete(runId)) this.setState({ pendingCancel });
      }
    }
  }

  cancelRun(id: string): void {
    const run = this.state.runs.find(candidate => candidate.id === id);
    if (run?.job_id) void api.interruptJob(run.job_id).catch(() => {});
    const controller = this.controllers.get(id);
    if (!controller) return;
    const pendingCancel = new Set(this.state.pendingCancel);
    pendingCancel.add(id);
    this.setState({ pendingCancel });
    controller.abort();
  }

  deleteRun(id: string): void {
    const run = this.state.runs.find(candidate => candidate.id === id);
    if (!run) return;
    if (isAutoOptRunActive(run)) {
      throw new Error('run is still active — cancel it before deleting');
    }
    if (run.job_id) void api.deleteJob(run.job_id).catch(() => {});
    const previousRuns = this.state.runs;
    const previousActiveRunId = this.state.activeRunId;
    this.setState({
      runs: this.state.runs.filter(candidate => candidate.id !== id),
      activeRunId: this.state.activeRunId === id ? null : this.state.activeRunId,
    });
    try {
      this.persist();
    } catch (err) {
      this.setState({
        runs: previousRuns,
        activeRunId: previousActiveRunId,
        lastError: errorMessage(err),
      });
      throw err;
    }
  }

  clearError(): void {
    if (this.state.lastError) this.setState({ lastError: null });
  }

  private updateRun(id: string, updater: (run: AutoOptRunRecord) => AutoOptRunRecord, persist = true): void {
    const runs = this.state.runs.map(run => run.id === id ? updater(run) : run);
    this.setState({ runs });
    if (persist) {
      try {
        this.persist();
      } catch {

      }
    }
  }

  private persist(): void {
    writeStoredRuns(this.scopeKey, this.state.runs);
  }

  private setState(patch: Partial<AutoOptState>): void {
    this.state = { ...this.state, ...patch };
    this.listeners.forEach(listener => listener(this.state));
  }
}

export const autoOptStore = new AutoOptStore();
