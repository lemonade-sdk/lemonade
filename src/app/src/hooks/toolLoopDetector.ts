/** Detect repeated, unchanged tool activity without limiting productive work. */

interface ToolInvocation {
  function: { name: string; arguments: string };
}

interface ToolOutcome {
  content: string;
  error?: boolean;
  artifacts?: readonly unknown[];
}

export interface ToolLoopNotice {
  action: 'warn' | 'stop';
  reason: string;
}

const HISTORY_SIZE = 48;
const MAX_CYCLE_SIZE = 4;
const WARN_REPETITIONS = 3;
const STOP_SINGLE_REPETITIONS = 5;
const STOP_CYCLE_REPETITIONS = 4;
const WAITING_WARNING_REPETITIONS = 8;

// These fields change during ordinary status reporting without changing the result.
const TRANSIENT_RESULT_KEYS = new Set([
  'timestamp', 'timestamp_ms', 'request_id', 'trace_id', 'elapsed_ms', 'duration_ms',
]);
const WAITING_STATES = new Set([
  'pending', 'queued', 'running', 'in_progress', 'in-progress', 'downloading', 'streaming',
]);

function stableJson(value: unknown, result: boolean, depth = 0): unknown {
  if (depth > 24 || value === null || typeof value !== 'object') return value;
  if (Array.isArray(value)) return value.map(item => stableJson(item, result, depth + 1));
  const record = value as Record<string, unknown>;
  return Object.fromEntries(Object.keys(record).sort()
    .filter(key => !result || !TRANSIENT_RESULT_KEYS.has(key))
    .map(key => [key, stableJson(record[key], result, depth + 1)]));
}

function normalized(value: string, result: boolean): string {
  try { return JSON.stringify(stableJson(JSON.parse(value), result)); }
  catch { return value.trim().replace(/\r\n?/g, '\n'); }
}

// Two independent 32-bit accumulators keep the history small even for large tool results.
function fingerprint(text: string): string {
  let a = 2166136261;
  let b = 0x9e3779b9;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    a = Math.imul(a ^ c, 16777619);
    b = Math.imul(b ^ c, 2246822519);
  }
  return `${text.length}:${(a >>> 0).toString(16)}:${(b >>> 0).toString(16)}`;
}

function parsedObject(content: string): Record<string, unknown> | null {
  try {
    const value: unknown = JSON.parse(content);
    return value !== null && typeof value === 'object' && !Array.isArray(value)
      ? value as Record<string, unknown> : null;
  } catch { return null; }
}

function isReportedProgress(outcome: ToolOutcome): boolean {
  if (outcome.error) return false;
  if (outcome.artifacts?.length) return true;
  const value = parsedObject(outcome.content);
  if (!value || value.error) return false;
  return value.changed === true || value.modified === true || value.state_changed === true;
}

function isWaiting(outcome: ToolOutcome): boolean {
  if (outcome.error) return false;
  const value = parsedObject(outcome.content);
  return !!value && !value.error && WAITING_STATES.has(String(value.status || value.state || '').toLowerCase());
}

function canonicalCycle(keys: readonly string[]): string {
  const rotations = keys.map((_, shift) =>
    keys.slice(shift).concat(keys.slice(0, shift)).join(','));
  return rotations.sort()[0];
}

export class ToolLoopDetector {
  private history: Array<{ key: string; names: string }> = [];
  private warnedPattern: string | null = null;
  private waitingKey: string | null = null;
  private waitingCount = 0;

  observe(calls: readonly ToolInvocation[], outcomes: readonly ToolOutcome[]): ToolLoopNotice | null {
    if (calls.length === 0 || calls.length !== outcomes.length) return null;

    // Explicit changes may be repeated safely. An ongoing job can also
    // require repeated status reads without proving that the agent is stuck.
    const changed = outcomes.some(isReportedProgress);
    const waiting = outcomes.every(isWaiting);
    if (changed) {
      this.history = [];
      this.warnedPattern = null;
      this.waitingKey = null;
      this.waitingCount = 0;
      return null;
    }

    const entries = calls.map((call, index) => {
      const outcome = outcomes[index];
      const name = call.function.name;
      return {
        name,
        key: [name, fingerprint(normalized(call.function.arguments || '{}', false)),
          fingerprint(normalized(outcome.content || '', true)), outcome.error ? 'error' : 'ok'].join('|'),
      };
    }).sort((a, b) => a.key.localeCompare(b.key));

    if (waiting) {
      const key = fingerprint(entries.map(entry => entry.key).join('\n'));
      this.waitingCount = key === this.waitingKey ? this.waitingCount + 1 : 1;
      this.waitingKey = key;
      this.history = [];
      this.warnedPattern = null;
      return this.waitingCount === WAITING_WARNING_REPETITIONS
        ? { action: 'warn', reason: 'The job is still running with no visible change; avoid rapid repeated polling' }
        : null;
    }
    this.waitingKey = null;
    this.waitingCount = 0;

    this.history.push({
      key: fingerprint(entries.map(entry => entry.key).join('\n')),
      names: [...new Set(entries.map(entry => entry.name))].join(', '),
    });
    if (this.history.length > HISTORY_SIZE) this.history.shift();

    for (let size = 1; size <= MAX_CYCLE_SIZE; size++) {
      const keys = this.history.map(item => item.key);
      if (keys.length < size * WARN_REPETITIONS) continue;
      const cycle = keys.slice(-size);
      let repetitions = 1;
      while (keys.length >= size * (repetitions + 1) &&
        cycle.every((key, i) => keys[keys.length - size * (repetitions + 1) + i] === key)) {
        repetitions++;
      }
      if (repetitions < WARN_REPETITIONS) continue;

      const pattern = `${size}:${canonicalCycle(cycle)}`;
      const names = [...new Set(this.history.slice(-size).flatMap(entry => entry.names.split(', ')))].join(', ');
      const reason = `Repeated ${size === 1 ? 'tool call' : `${size}-round tool cycle`} without an observable result change (${names})`;
      if (repetitions >= (size === 1 ? STOP_SINGLE_REPETITIONS : STOP_CYCLE_REPETITIONS)) {
        return { action: 'stop', reason };
      }
      if (this.warnedPattern !== pattern) {
        this.warnedPattern = pattern;
        return { action: 'warn', reason };
      }
      return null;
    }
    this.warnedPattern = null;
    return null;
  }
}
