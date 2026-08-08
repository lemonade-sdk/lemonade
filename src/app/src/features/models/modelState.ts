import { useSyncExternalStore } from 'react';
import api, { type ModelStateSnapshot } from '../../api';

const subscribe = (listener: () => void): (() => void) => api.onModelStateChange(listener);
const getSnapshot = (): ModelStateSnapshot => api.modelStateSnapshot;

/**
 * Shared server model state for the renderer.
 *
 * The API singleton owns the canonical registry/health snapshot and coalesces
 * refreshes. Views subscribe here instead of fetching `/api/v1/models` into
 * independent local state.
 */
export function useServerModelState(): ModelStateSnapshot {
  return useSyncExternalStore(subscribe, getSnapshot, getSnapshot);
}
