export const DEFAULT_VISIBLE_BACKEND_COUNT = 6;

export interface BackendRailVisibilityState {
  order: string[];
  visibleCount: number;
}

/** Keep the user's rail ordering while reconciling backends that appeared or disappeared. */
export function resolveBackendRailVisibility(
  availableValues: readonly string[],
  state: BackendRailVisibilityState | null,
): BackendRailVisibilityState {
  const available = new Set(availableValues);
  const seen = new Set<string>();
  const order: string[] = [];

  const append = (value: string) => {
    if (!available.has(value) || seen.has(value)) return;
    seen.add(value);
    order.push(value);
  };

  for (const value of state?.order || []) append(value);
  for (const value of availableValues) append(value);

  const minimumVisible = Math.min(DEFAULT_VISIBLE_BACKEND_COUNT, order.length);
  const requestedVisible = state?.visibleCount ?? minimumVisible;

  return {
    order,
    visibleCount: Math.min(order.length, Math.max(minimumVisible, requestedVisible)),
  };
}

export function revealNextBackend(state: BackendRailVisibilityState): BackendRailVisibilityState {
  return {
    ...state,
    visibleCount: Math.min(state.order.length, state.visibleCount + 1),
  };
}

/** Move a visible backend behind the remaining choices so the next reveal advances. */
export function hideBackendShortcut(
  state: BackendRailVisibilityState,
  backend: string,
): BackendRailVisibilityState {
  if (
    state.visibleCount <= DEFAULT_VISIBLE_BACKEND_COUNT
    || !state.order.slice(0, state.visibleCount).includes(backend)
  ) {
    return state;
  }

  return {
    order: [...state.order.filter(value => value !== backend), backend],
    visibleCount: state.visibleCount - 1,
  };
}
