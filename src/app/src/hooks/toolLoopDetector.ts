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
const MAX_FAILURE_HISTORY = 128;
const MAX_RETRY_SPACING = 8;

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

function failureSignature(outcome: ToolOutcome): string | null {
  const value = parsedObject(outcome.content);
  if (!outcome.error && !value?.error) return null;
  return fingerprint(normalized(outcome.content || '', true));
}

function callSignature(call: ToolInvocation): string {
  return `${call.function.name}|${fingerprint(normalized(call.function.arguments || '{}', false))}`;
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

interface TrackedFailure {
  result: string;
  count: number;
  retryAfter: number;
  retrySpacing: number;
  probing: boolean;
}

export class ToolLoopDetector {
  private history: Array<{ key: string; names: string }> = [];
  private warnedPattern: string | null = null;
  private waitingKey: string | null = null;
  private waitingCount = 0;
  private successfulRounds = 0;
  private failures = new Map<string, TrackedFailure>();

  // Checking this also reserves one permitted recovery attempt.
  blockedReason(call: ToolInvocation): string | null {
    const failure = this.failures.get(callSignature(call));
    if (!failure || failure.count < STOP_SINGLE_REPETITIONS) return null;
    if (!failure.probing && this.successfulRounds >= failure.retryAfter) {
      failure.probing = true;
      return null;
    }
    return `${call.function.name} with these arguments failed identically at least ${STOP_SINGLE_REPETITIONS} times. This call is temporarily blocked; change the arguments or use a different tool.`;
  }

  private observeFailures(calls: readonly ToolInvocation[], outcomes: readonly ToolOutcome[]): ToolLoopNotice | null {
    let notice: ToolLoopNotice | null = null;
    const succeeded = new Set(calls.flatMap((call, i) =>
      failureSignature(outcomes[i]) === null ? [callSignature(call)] : []));

    for (let i = 0; i < calls.length; i++) {
      const key = callSignature(calls[i]);
      const error = failureSignature(outcomes[i]);
      if (succeeded.has(key)) {
        this.failures.delete(key);
        continue;
      }
      if (error === null) continue;

      const previous = this.failures.get(key);
      const same = previous?.result === error;
      const count = same ? Math.min(previous.count + 1, STOP_SINGLE_REPETITIONS) : 1;
      let retrySpacing = same ? previous.retrySpacing : 1;
      let retryAfter = same ? previous.retryAfter : 0;
      if (count === STOP_SINGLE_REPETITIONS) {
        if (same && previous.probing) {
          // One failed recovery probe must not reopen the call every round.
          retrySpacing = Math.min(retrySpacing * 2, MAX_RETRY_SPACING);
          retryAfter = this.successfulRounds + retrySpacing;
        } else if (!same || previous.count < STOP_SINGLE_REPETITIONS) {
          // Another successful tool result gives one chance to recheck the error.
          retryAfter = this.successfulRounds + 1;
        }
      }

      this.failures.delete(key);
      this.failures.set(key, { result: error, count, retrySpacing, retryAfter, probing: false });
      if (this.failures.size > MAX_FAILURE_HISTORY) this.failures.delete(this.failures.keys().next().value!);
      if (count === WARN_REPETITIONS || (count === STOP_SINGLE_REPETITIONS && previous?.count !== count)) {
        notice = {
          action: 'warn',
          reason: `${calls[i].function.name} returned the same result ${count} times with unchanged arguments. ${count === STOP_SINGLE_REPETITIONS ? 'Further identical calls will be throttled, with recovery attempts after successful other tool calls; change the arguments or use a different tool.' : 'Do not retry it unchanged.'}`,
        };
      }
    }
    if (outcomes.some(outcome => failureSignature(outcome) === null
      && (!isWaiting(outcome) || isReportedProgress(outcome)))) {
      this.successfulRounds++;
    }
    return notice;
  }

  observe(calls: readonly ToolInvocation[], outcomes: readonly ToolOutcome[]): ToolLoopNotice | null {
    if (calls.length === 0 || calls.length !== outcomes.length) return null;

    // New arguments or changed error details are activity the round guard must see.
    const changedFailure = calls.some((call, i) => {
      const result = failureSignature(outcomes[i]);
      const previous = this.failures.get(callSignature(call));
      return result !== null && (previous === undefined || previous.result !== result);
    });
    const failureNotice = this.observeFailures(calls, outcomes);
    // A failed call is handled by its own guard and must not cause the
    // round-level guard to disable unrelated tools.
    const active = calls.map((call, i) => ({ call, outcome: outcomes[i] }))
      .filter(item => failureSignature(item.outcome) === null);
    const changed = changedFailure || active.some(item => isReportedProgress(item.outcome));
    const waiting = active.length > 0 && active.every(item => isWaiting(item.outcome));
    if (changed) {
      this.history = [];
      this.warnedPattern = null;
      this.waitingKey = null;
      this.waitingCount = 0;
      return failureNotice;
    }
    if (active.length === 0) {
      this.history = [];
      this.warnedPattern = null;
      this.waitingKey = null;
      this.waitingCount = 0;
      return failureNotice;
    }

    const entries = active.map(({ call, outcome }) => {
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
        : failureNotice;
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
      return failureNotice;
    }
    this.warnedPattern = null;
    return failureNotice;
  }
}
