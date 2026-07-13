import api from '../../api';
import {
  AutoOptBudget,
  AutoOptRunDetail,
  AutoOptRunStatus,
  AutoOptRunSummary,
  AutoOptStage,
  AutoOptStageStatus,
  AutoOptStartRequest,
  isAutoOptRunActive,
} from './autoOptTypes';

export interface AutoOptState {
  runs: AutoOptRunSummary[];
  details: Record<string, AutoOptRunDetail>;
  activeRunId: string | null;
  lastError: string | null;
  pendingCancel: Set<string>;
}

type Listener = (state: AutoOptState) => void;

const ACTIVE_POLL_MS = 1500;
const IDLE_POLL_MS = 10000;

const RUN_STATUSES: AutoOptRunStatus[] = ['queued', 'running', 'completed', 'failed', 'cancelled'];
const STAGE_STATUSES: AutoOptStageStatus[] = ['pending', 'running', 'completed', 'failed', 'skipped'];
const BUDGETS: AutoOptBudget[] = ['quick', 'standard', 'thorough'];

function coerceStatus(value: unknown): AutoOptRunStatus {
  const s = String(value || '').toLowerCase();
  if (RUN_STATUSES.includes(s as AutoOptRunStatus)) return s as AutoOptRunStatus;
  if (s === 'canceled' || s === 'cancelling' || s === 'canceling') return 'cancelled';
  if (s === 'error' || s === 'failure') return 'failed';
  if (s === 'complete' || s === 'success' || s === 'done') return 'completed';
  if (s === 'pending') return 'queued';
  return 'queued';
}

function coerceStageStatus(value: unknown): AutoOptStageStatus {
  const s = String(value || '').toLowerCase();
  if (STAGE_STATUSES.includes(s as AutoOptStageStatus)) return s as AutoOptStageStatus;
  if (s === 'error' || s === 'failure') return 'failed';
  if (s === 'complete' || s === 'success' || s === 'done') return 'completed';
  if (s === 'skip') return 'skipped';
  return 'pending';
}

function coerceTimestamp(value: unknown): string | undefined {
  if (typeof value === 'number' && Number.isFinite(value) && value > 0) {
    const ms = value < 10_000_000_000 ? value * 1000 : value;
    return new Date(ms).toISOString();
  }
  if (typeof value === 'string' && value.trim()) {
    const parsed = Date.parse(value.trim());
    return Number.isFinite(parsed) ? new Date(parsed).toISOString() : undefined;
  }
  return undefined;
}

export function runCreatedAtMs(run: Pick<AutoOptRunSummary, 'created_at'>): number {
  const parsed = Date.parse(run.created_at || '');
  return Number.isFinite(parsed) ? parsed : 0;
}

function optionalString(value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined;
  const trimmed = value.trim();
  return trimmed ? trimmed : undefined;
}

function optionalNumber(value: unknown): number | undefined {
  const n = Number(value);
  return Number.isFinite(n) ? n : undefined;
}

export function normalizeRunSummary(raw: unknown): AutoOptRunSummary | null {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  const obj = raw as Record<string, unknown>;
  const id = optionalString(obj.id);
  if (!id) return null;
  const progressRaw = obj.progress && typeof obj.progress === 'object' && !Array.isArray(obj.progress)
    ? obj.progress as Record<string, unknown>
    : null;
  const budget = String(obj.budget || '').toLowerCase();
  return {
    id,
    model: optionalString(obj.model) || '',
    status: coerceStatus(obj.status),
    budget: BUDGETS.includes(budget as AutoOptBudget) ? budget as AutoOptBudget : 'standard',
    created_at: coerceTimestamp(obj.created_at) || new Date(0).toISOString(),
    finished_at: coerceTimestamp(obj.finished_at),
    summary: optionalString(obj.summary),
    error: optionalString(obj.error),
    lemonade_version: optionalString(obj.lemonade_version),
    progress: progressRaw ? {
      stage: optionalString(progressRaw.stage) || '',
      stage_index: optionalNumber(progressRaw.stage_index) ?? 0,
      stage_count: optionalNumber(progressRaw.stage_count) ?? 0,
      percent: optionalNumber(progressRaw.percent),
      detail: optionalString(progressRaw.detail),
      eta_seconds: optionalNumber(progressRaw.eta_seconds),
    } : undefined,
  };
}

export function normalizeRunDetail(raw: unknown): AutoOptRunDetail | null {
  const summary = normalizeRunSummary(raw);
  if (!summary) return null;
  const obj = raw as Record<string, unknown>;
  const stagesRaw = Array.isArray(obj.stages) ? obj.stages : [];
  const stages: AutoOptStage[] = stagesRaw
    .filter((stage): stage is Record<string, unknown> => !!stage && typeof stage === 'object' && !Array.isArray(stage))
    .map(stage => ({
      name: optionalString(stage.name) || '',
      status: coerceStageStatus(stage.status),
      duration_ms: optionalNumber(stage.duration_ms),
      error: optionalString(stage.error),
      data: stage.data && typeof stage.data === 'object' && !Array.isArray(stage.data)
        ? stage.data as Record<string, unknown>
        : undefined,
    }))
    .filter(stage => stage.name);
  const measurementsRaw = obj.measurements && typeof obj.measurements === 'object' && !Array.isArray(obj.measurements)
    ? obj.measurements as Record<string, unknown>
    : null;
  return {
    ...summary,
    answers: obj.answers && typeof obj.answers === 'object' && !Array.isArray(obj.answers)
      ? obj.answers as AutoOptRunDetail['answers']
      : undefined,
    stages,
    measurements: measurementsRaw ? {
      fit: Array.isArray(measurementsRaw.fit) ? measurementsRaw.fit as Array<Record<string, unknown>> : [],
      bench: Array.isArray(measurementsRaw.bench) ? measurementsRaw.bench as Array<Record<string, unknown>> : [],
    } : undefined,
    result: obj.result && typeof obj.result === 'object' && !Array.isArray(obj.result)
      ? obj.result as AutoOptRunDetail['result']
      : undefined,
  };
}

function sortRuns(runs: AutoOptRunSummary[]): AutoOptRunSummary[] {
  return [...runs].sort((a, b) => runCreatedAtMs(b) - runCreatedAtMs(a));
}

function errorMessage(err: unknown): string {
  if (err instanceof Error) return err.message;
  return String(err || 'Unknown error');
}

class AutoOptStore {
  private state: AutoOptState = {
    runs: [],
    details: {},
    activeRunId: null,
    lastError: null,
    pendingCancel: new Set(),
  };
  private listeners = new Set<Listener>();
  private pollTimer: ReturnType<typeof setTimeout> | null = null;
  private refreshInFlight: Promise<void> | null = null;
  private started = false;

  constructor() {
    if (typeof window !== 'undefined') {
      window.addEventListener('focus', () => { if (this.started) void this.refresh(); });
      window.addEventListener('online', () => { if (this.started) void this.refresh(); });
    }
    api.onStatusChange(status => {
      if (this.started && status === 'connected') void this.refresh();
    });
  }

  snapshot(): AutoOptState {
    return this.state;
  }

  subscribe(listener: Listener): () => void {
    this.listeners.add(listener);
    listener(this.state);
    this.start();
    return () => {
      this.listeners.delete(listener);
      if (this.listeners.size === 0) this.stop();
    };
  }

  private start(): void {
    if (this.started) return;
    this.started = true;
    void this.refresh();
  }

  private stop(): void {
    this.started = false;
    if (this.pollTimer) {
      clearTimeout(this.pollTimer);
      this.pollTimer = null;
    }
  }

  setActiveRun(id: string | null): void {
    if (this.state.activeRunId === id) return;
    this.setState({ activeRunId: id });
    if (id && !id.startsWith('pending-')) void this.refreshDetail(id);
  }

  async refresh(): Promise<void> {
    if (this.refreshInFlight) return this.refreshInFlight;
    this.refreshInFlight = (async () => {
      if (!api.isConnected) {
        this.scheduleNext();
        return;
      }
      try {
        const serverRuns = (await api.autoOptRuns())
          .map(normalizeRunSummary)
          .filter((run): run is AutoOptRunSummary => !!run);
        const pending = this.state.runs.filter(run => run.id.startsWith('pending-'));
        this.setState({ runs: sortRuns([...pending, ...serverRuns]), lastError: null });
        const activeId = this.state.activeRunId;
        if (activeId && !activeId.startsWith('pending-')) {
          const active = serverRuns.find(run => run.id === activeId);
          if (active && (isAutoOptRunActive(active) || !this.state.details[activeId])) {
            await this.refreshDetail(activeId);
          }
        }
      } catch (err) {
        this.setState({ lastError: errorMessage(err) });
      } finally {
        this.scheduleNext();
      }
    })();
    try {
      await this.refreshInFlight;
    } finally {
      this.refreshInFlight = null;
    }
  }

  async refreshDetail(id: string): Promise<AutoOptRunDetail | null> {
    try {
      const detail = normalizeRunDetail(await api.autoOptRun(id));
      if (!detail) return null;
      this.setState({ details: { ...this.state.details, [id]: detail } });
      return detail;
    } catch (err) {
      this.setState({ lastError: errorMessage(err) });
      return null;
    }
  }

  async startRun(request: AutoOptStartRequest): Promise<string> {
    const syntheticId = `pending-${Date.now()}`;
    const synthetic: AutoOptRunSummary = {
      id: syntheticId,
      model: request.model,
      status: 'queued',
      budget: request.budget,
      created_at: new Date().toISOString(),
    };
    this.setState({ runs: sortRuns([synthetic, ...this.state.runs]), lastError: null });
    try {
      const { id } = await api.autoOptStart(request);
      this.setState({
        runs: sortRuns(this.state.runs.map(run => run.id === syntheticId ? { ...run, id } : run)),
        activeRunId: this.state.activeRunId === syntheticId ? id : this.state.activeRunId,
      });
      void this.refresh();
      return id;
    } catch (err) {
      this.setState({
        runs: this.state.runs.filter(run => run.id !== syntheticId),
        lastError: errorMessage(err),
      });
      throw err;
    }
  }

  async cancelRun(id: string): Promise<void> {
    const previousRuns = this.state.runs;
    const pendingCancel = new Set(this.state.pendingCancel);
    pendingCancel.add(id);
    this.setState({
      runs: this.state.runs.map(run => run.id === id ? { ...run, status: 'cancelled' } : run),
      pendingCancel,
    });
    try {
      await api.autoOptCancel(id);
      void this.refresh();
    } catch (err) {
      this.setState({ runs: previousRuns, lastError: errorMessage(err) });
      throw err;
    } finally {
      const next = new Set(this.state.pendingCancel);
      next.delete(id);
      this.setState({ pendingCancel: next });
    }
  }

  async deleteRun(id: string): Promise<void> {
    const previousRuns = this.state.runs;
    const details = { ...this.state.details };
    delete details[id];
    this.setState({
      runs: this.state.runs.filter(run => run.id !== id),
      details,
      activeRunId: this.state.activeRunId === id ? null : this.state.activeRunId,
    });
    try {
      await api.autoOptDelete(id);
    } catch (err) {
      this.setState({ runs: previousRuns, lastError: errorMessage(err) });
      throw err;
    }
  }

  clearError(): void {
    if (this.state.lastError) this.setState({ lastError: null });
  }

  private setState(patch: Partial<AutoOptState>): void {
    this.state = { ...this.state, ...patch };
    this.listeners.forEach(listener => listener(this.state));
  }

  private scheduleNext(): void {
    if (!this.started) return;
    if (this.pollTimer) clearTimeout(this.pollTimer);
    const activeRun = this.state.activeRunId
      ? this.state.runs.find(run => run.id === this.state.activeRunId)
      : undefined;
    const busy = this.state.runs.some(isAutoOptRunActive) || (activeRun && isAutoOptRunActive(activeRun));
    this.pollTimer = setTimeout(() => void this.refresh(), busy ? ACTIVE_POLL_MS : IDLE_POLL_MS);
  }
}

export const autoOptStore = new AutoOptStore();
