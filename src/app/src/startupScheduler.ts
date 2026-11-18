/**
 * Run non-critical renderer work only after the browser has had a chance to
 * paint the startup UI.  requestIdleCallback keeps parsing/hydration work out
 * of the first-input window; the timeout guarantees progress on busy pages and
 * on engines that throttle idle callbacks.
 */
export function scheduleIdleWork(task: () => void, timeout = 1000): () => void {
  let cancelled = false;
  let rafId = 0;
  let timerId = 0;
  let idleId: number | null = null;

  const run = () => {
    if (!cancelled) task();
  };

  rafId = window.requestAnimationFrame(() => {
    const requestIdle = (window as typeof window & {
      requestIdleCallback?: (callback: () => void, options?: { timeout: number }) => number;
    }).requestIdleCallback;

    if (typeof requestIdle === 'function') {
      idleId = requestIdle.call(window, run, { timeout });
    } else {
      timerId = window.setTimeout(run, Math.min(timeout, 250));
    }
  });

  return () => {
    cancelled = true;
    if (rafId) window.cancelAnimationFrame(rafId);
    if (timerId) window.clearTimeout(timerId);
    if (idleId !== null) {
      const cancelIdle = (window as typeof window & { cancelIdleCallback?: (id: number) => void }).cancelIdleCallback;
      if (cancelIdle) cancelIdle.call(window, idleId);
    }
  };
}
