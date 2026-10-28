import api, { friendlyErrorMessage, type HealthModelSync, type ModelSyncStatus } from '../../api';

export interface ModelSyncSnapshot {
  /** Detailed status of the sync running now. */
  active: ModelSyncStatus | null;
  /** Outcome of the last sync that finished while this client was connected, until dismissed. */
  result: ModelSyncStatus | null;
  submitting: boolean;
  error: string | null;
}

type Listener = (snapshot: ModelSyncSnapshot) => void;

const POLL_MS = 1500;
const RETRY_MS = 3000;
const TERMINAL = new Set<ModelSyncStatus['status']>(['success', 'failed']);

export function isModelSyncRunning(snapshot: ModelSyncSnapshot): boolean {
  return snapshot.submitting || snapshot.active?.status === 'in_progress';
}

export function modelSyncFailureText(status: ModelSyncStatus | null): string | null {
  if (!status) return null;
  const failures = Object.entries(status.failed_models || {}).map(([name, reason]) => `${name}: ${reason}`);
  if (status.status !== 'failed' && !status.terminal_error && failures.length === 0) return null;
  return [status.terminal_error, ...failures].filter(Boolean).join('; ') || 'Model update failed.';
}

export function openServerLogs(): void {
  window.dispatchEvent(new CustomEvent('lemonade:navigate', { detail: { view: 'logs' } }));
}

// A sync is server-owned and can be started by any client, the CLI, or the
// server's own auto-update, so the client follows the server's health summary
// instead of only the syncs it started itself.
class ModelSyncStore {
  private state: ModelSyncSnapshot = { active: null, result: null, submitting: false, error: null };
  private listeners = new Set<Listener>();
  private started = false;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private pollInFlight: Promise<void> | null = null;
  // completed_sync_id already accounted for on this connection. Results at or
  // below it finished before this client looked, so they are not news.
  private baseline: number | null = null;
  private lastHealthSync: HealthModelSync | undefined;

  snapshot(): ModelSyncSnapshot {
    return this.state;
  }

  subscribe(listener: Listener): () => void {
    this.listeners.add(listener);
    listener(this.state);
    this.start();
    return () => { this.listeners.delete(listener); };
  }

  async startSync(models?: string[]): Promise<void> {
    if (isModelSyncRunning(this.state)) return;
    this.set({ submitting: true, error: null, result: null });
    try {
      const status = await api.startModelSync(models);
      this.set({ active: status.status === 'in_progress' ? status : this.state.active });
      void this.poll();
    } catch (error) {
      this.set({ error: friendlyErrorMessage(error) });
    } finally {
      this.set({ submitting: false });
    }
  }

  dismissResult(): void {
    this.set({ result: null, error: null });
  }

  private start(): void {
    if (this.started) return;
    this.started = true;
    api.onModelStateChange(() => this.onModelState());
    this.onModelState();
  }

  private onModelState(): void {
    const snapshot = api.modelStateSnapshot;
    if (snapshot.status !== 'connected') {
      this.reset();
      return;
    }
    const sync = snapshot.health?.model_sync;
    if (!sync || sync === this.lastHealthSync) return;
    this.lastHealthSync = sync;
    if (this.baseline === null || sync.completed_sync_id < this.baseline) {
      // First look on this connection, or the server restarted and its ids began again.
      this.baseline = sync.completed_sync_id;
    }
    if (sync.status === 'in_progress' || sync.completed_sync_id > this.baseline) void this.poll();
  }

  private poll(): Promise<void> {
    if (this.pollInFlight) return this.pollInFlight;
    this.clearTimer();
    this.pollInFlight = (async () => {
      let next: number | null = null;
      try {
        const status = await api.getModelSyncStatus();
        if (status.status === 'in_progress') {
          this.set({ active: status, error: null });
          next = POLL_MS;
          return;
        }
        const completed = status.completed_sync_id ?? status.sync_id;
        if (this.baseline !== null && completed < this.baseline) this.baseline = completed;
        const isNews = this.baseline === null || completed > this.baseline || this.state.active !== null;
        this.baseline = Math.max(this.baseline ?? 0, completed);
        this.set({
          active: null,
          error: null,
          result: isNews && TERMINAL.has(status.status) ? status : this.state.result,
        });
        void api.refresh();
      } catch (error) {
        if (this.state.active) {
          this.set({ error: friendlyErrorMessage(error) });
          next = RETRY_MS;
        }
      } finally {
        this.pollInFlight = null;
        if (next !== null) this.timer = setTimeout(() => { this.timer = null; void this.poll(); }, next);
      }
    })();
    return this.pollInFlight;
  }

  private reset(): void {
    this.clearTimer();
    this.baseline = null;
    this.lastHealthSync = undefined;
    if (this.state.active || this.state.submitting) {
      this.set({ active: null, submitting: false });
    }
  }

  private clearTimer(): void {
    if (this.timer === null) return;
    clearTimeout(this.timer);
    this.timer = null;
  }

  private set(patch: Partial<ModelSyncSnapshot>): void {
    this.state = { ...this.state, ...patch };
    this.listeners.forEach(listener => { try { listener(this.state); } catch {} });
  }
}

export const modelSyncStore = new ModelSyncStore();
