import { useSyncExternalStore } from 'react';
import type { ModelStateSnapshot } from '../../api';

const EMPTY_MODEL_STATE: ModelStateSnapshot = {
  status: 'disconnected',
  health: null,
  models: null,
  refreshing: false,
  error: null,
};

type ModelStateApi = {
  readonly modelStateSnapshot: ModelStateSnapshot;
  onModelStateChange(listener: () => void): () => void;
};

let snapshot = EMPTY_MODEL_STATE;
let attachedApi: ModelStateApi | null = null;
let detachApi: (() => void) | null = null;
const listeners = new Set<() => void>();

function emit(): void {
  for (const listener of listeners) listener();
}

function subscribe(listener: () => void): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

function getSnapshot(): ModelStateSnapshot {
  return snapshot;
}

/**
 * Attach the already-loaded API singleton to the tiny renderer store.
 *
 * This is deliberately explicit.  The old hook imported the 100 KB+ API
 * module from a useEffect on every cold start, which raced the default Chat
 * render and defeated App's post-paint host bootstrap.  App now loads the API
 * once on the post-paint path and hands the same singleton to this bridge.
 */
export function attachServerModelState(api: ModelStateApi): void {
  if (attachedApi === api) return;
  detachApi?.();
  attachedApi = api;
  snapshot = api.modelStateSnapshot;
  emit();
  detachApi = api.onModelStateChange(() => {
    const next = api.modelStateSnapshot;
    if (next === snapshot) return;
    snapshot = next;
    emit();
  });
}

/** Shared server model state for the renderer. */
export function useServerModelState(): ModelStateSnapshot {
  return useSyncExternalStore(subscribe, getSnapshot, getSnapshot);
}
