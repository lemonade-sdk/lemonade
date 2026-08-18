type Listener = () => void;

let depth = 0;
const listeners = new Set<Listener>();

export function isModelSelectionLocked(): boolean {
  return depth > 0;
}

export function onModelSelectionUnlocked(listener: Listener): () => void {
  listeners.add(listener);
  return () => { listeners.delete(listener); };
}

export async function withModelSelectionLock<T>(run: () => Promise<T>): Promise<T> {
  depth += 1;
  try {
    return await run();
  } finally {
    depth = Math.max(0, depth - 1);
    if (depth === 0) {
      listeners.forEach(listener => {
        try { listener(); } catch { }
      });
    }
  }
}
