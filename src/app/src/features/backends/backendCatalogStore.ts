import { useSyncExternalStore } from 'react';
import { parseBackendCatalog, type BackendCatalog } from './backendCatalog';

export type BackendCatalogStatus = 'idle' | 'loading' | 'ready' | 'error';

export interface BackendCatalogSnapshot {
  status: BackendCatalogStatus;
  catalog: BackendCatalog | null;
  error: string | null;
}

type BackendCatalogApi = {
  readonly systemInfoData: Record<string, unknown> | null;
  systemInfo(): Promise<Record<string, unknown>>;
  onSystemInfoChange(listener: () => void): () => void;
};

const EMPTY: BackendCatalogSnapshot = { status: 'idle', catalog: null, error: null };

let snapshot = EMPTY;
let attachedApi: BackendCatalogApi | null = null;
let detachApi: (() => void) | null = null;
let generation = 0;
const listeners = new Set<() => void>();

function emit(next: BackendCatalogSnapshot): void {
  snapshot = next;
  for (const listener of listeners) listener();
}

function subscribe(listener: () => void): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

function getSnapshot(): BackendCatalogSnapshot {
  return snapshot;
}

/**
 * Bridge the API singleton to the renderer store, mirroring
 * attachServerModelState: App loads the API once on the post-paint path and
 * hands the same instance here, rather than each view importing it from an
 * effect and racing first paint.
 */
export function attachBackendCatalog(api: BackendCatalogApi): void {
  if (attachedApi === api) return;
  detachApi?.();
  attachedApi = api;

  if (api.systemInfoData) {
    generation += 1;
    emit({ status: 'ready', catalog: parseBackendCatalog(api.systemInfoData), error: null });
  }

  detachApi = api.onSystemInfoChange(() => {
    const data = api.systemInfoData;
    if (!data) return;
    generation += 1;
    emit({ status: 'ready', catalog: parseBackendCatalog(data), error: null });
  });
}

/**
 * Refetch system-info. A refresh that fails after a newer one has already
 * landed is dropped, so a slow error can never replace a fresher catalog.
 */
export async function refreshBackendCatalog(): Promise<BackendCatalog | null> {
  const api = attachedApi;
  if (!api) return snapshot.catalog;

  const ticket = ++generation;
  if (!snapshot.catalog) emit({ status: 'loading', catalog: null, error: null });

  try {
    await api.systemInfo();
  } catch (error) {
    if (ticket !== generation) return snapshot.catalog;
    emit({
      status: 'error',
      catalog: snapshot.catalog,
      error: error instanceof Error ? error.message : String(error),
    });
    return null;
  }
  return snapshot.catalog;
}

/** Shared backend catalog for the renderer. */
export function useBackendCatalog(): BackendCatalogSnapshot {
  return useSyncExternalStore(subscribe, getSnapshot, getSnapshot);
}

/** Same snapshot, for callers outside the React tree. */
export function getBackendCatalogSnapshot(): BackendCatalogSnapshot {
  return snapshot;
}

/** Test seam: drop all store state between cases. */
export function resetBackendCatalogForTests(): void {
  detachApi?.();
  attachedApi = null;
  detachApi = null;
  generation = 0;
  snapshot = EMPTY;
}
