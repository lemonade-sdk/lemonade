/**
 * Decomposes a backend args string into typed fields and a freeform remainder,
 * so the configuration form can show every sampler value it will send instead
 * of one opaque command line.
 *
 * The tokeniser is a port of `parse_custom_args` / `build_custom_args_map` in
 * `src/cpp/include/lemon/utils/custom_args.h`. It is deliberately
 * bug-compatible with that implementation — including the way `keep_quotes`
 * wraps the whole accumulated token rather than just the quoted run — because
 * lemond parses the string this panel sends, and a client that disagreed about
 * where a token ends would show one thing and load another.
 */

export type ArgFieldKind = 'number' | 'text';

export interface ArgFieldSpec {
  flag: string;
  /** The flag without its dashes, for element ids. */
  id: string;
  label: string;
  kind: ArgFieldKind;
  min?: number;
  max?: number;
  step?: number;
  /** llama.cpp's own value, so stepping an empty field starts where the backend
      would have. Absent where the field holds something other than a number. */
  backendDefault?: string;
}

/** Every sampler lemond can put in `*_args`, offered on every model so a value
    it does not set still reads as an explicit "default" rather than an absence.
    Ranges follow `llama-server --help`; the defaults are its documented ones. */
export const SAMPLER_ARG_FIELDS: ArgFieldSpec[] = [
  { flag: '--temp', id: 'temp', label: 'Temperature', kind: 'number', min: 0, max: 2, step: 0.05, backendDefault: '0.80' },
  { flag: '--top-k', id: 'top-k', label: 'Top-k', kind: 'number', min: 0, max: 200, step: 1, backendDefault: '40' },
  { flag: '--top-p', id: 'top-p', label: 'Top-p', kind: 'number', min: 0, max: 1, step: 0.05, backendDefault: '0.95' },
  { flag: '--min-p', id: 'min-p', label: 'Min-p', kind: 'number', min: 0, max: 1, step: 0.01, backendDefault: '0.05' },
  { flag: '--repeat-penalty', id: 'repeat-penalty', label: 'Repeat penalty', kind: 'number', min: 0, max: 2, step: 0.05, backendDefault: '1.00' },
  { flag: '--presence-penalty', id: 'presence-penalty', label: 'Presence penalty', kind: 'number', min: -2, max: 2, step: 0.05, backendDefault: '0.00' },
  { flag: '--chat-template-kwargs', id: 'chat-template-kwargs', label: 'Chat template kwargs', kind: 'text' },
];

const SAMPLER_FLAGS = new Set(SAMPLER_ARG_FIELDS.map(field => field.flag));

export function parseCustomArgs(customArgs: string): string[] {
  const result: string[] = [];
  if (!customArgs) return result;

  let current = '';
  let inQuotes = false;
  let quoteChar = '';

  for (const c of customArgs) {
    if (!inQuotes && (c === '"' || c === "'")) {
      inQuotes = true;
      quoteChar = c;
    } else if (inQuotes && c === quoteChar) {
      inQuotes = false;
      current = quoteChar + current + quoteChar;
      quoteChar = '';
    } else if (!inQuotes && c === ' ') {
      if (current) {
        result.push(current);
        current = '';
      }
    } else {
      current += c;
    }
  }

  if (current) result.push(current);
  return result;
}

/** A complete negative number is a value, not a flag. */
function isNegativeNumber(token: string): boolean {
  return /^-\d+(\.\d*)?([eE][-+]?\d+)?$/.test(token) || /^-\.\d+([eE][-+]?\d+)?$/.test(token);
}

export type ArgsMap = Map<string, string[][]>;

export function buildArgsMap(tokens: string[]): ArgsMap {
  const result: ArgsMap = new Map();
  let lastFlag = '';

  for (const token of tokens) {
    if (token.startsWith('-') && !isNegativeNumber(token)) {
      const occurrences = result.get(token) || [];
      occurrences.push([]);
      result.set(token, occurrences);
      lastFlag = token;
    } else if (lastFlag) {
      const occurrences = result.get(lastFlag);
      occurrences?.[occurrences.length - 1].push(token);
    }
  }

  return result;
}

/** Mirrors `map_to_args_string`: flags in sorted order, one run per occurrence. */
export function argsMapToString(map: ArgsMap): string {
  const parts: string[] = [];
  for (const flag of [...map.keys()].sort()) {
    for (const values of map.get(flag) || []) {
      parts.push([flag, ...values].join(' '));
    }
  }
  return parts.join(' ');
}

export interface SplitArgs {
  /** Flag -> single value, for the flags a typed field can represent. */
  fields: Record<string, string>;
  /** Everything else, as a command line the freeform box can hold. */
  rest: string;
  /** Sampler flags the freeform text holds, which therefore own themselves. */
  claimedByRest: Set<string>;
}

/**
 * A sampler flag becomes a field only when it appears once with a single value.
 * Anything less tidy stays in the freeform box, which then owns that flag
 * outright — the two halves must never both emit it, or the last one wins at
 * load time and the field would be showing a value that never applies.
 */
export function splitSamplerArgs(args: string): SplitArgs {
  const tokens = parseCustomArgs(args);
  const fields: Record<string, string> = {};
  const claimedByRest = new Set<string>();
  const extracted = new Set<string>();

  for (const [flag, occurrences] of buildArgsMap(tokens)) {
    if (!SAMPLER_FLAGS.has(flag)) continue;
    if (occurrences.length === 1 && occurrences[0].length === 1) {
      fields[flag] = occurrences[0][0];
      extracted.add(flag);
    } else {
      claimedByRest.add(flag);
    }
  }

  // The remainder keeps the order it was written in. Rebuilding it from the
  // sorted map would rewrite the freeform field under the cursor of whoever is
  // typing in it.
  const rest: string[] = [];
  let dropping = false;
  for (const token of tokens) {
    if (token.startsWith('-') && !isNegativeNumber(token)) dropping = extracted.has(token);
    if (!dropping) rest.push(token);
  }

  return { fields, rest: rest.join(' '), claimedByRest };
}

/** Field order is stable and the remainder is appended verbatim, so a value
    typed into either half survives the trip back through `splitSamplerArgs`. */
export function composeSamplerArgs(fields: Record<string, string>, rest: string): string {
  const parts: string[] = [];
  for (const spec of SAMPLER_ARG_FIELDS) {
    const value = (fields[spec.flag] || '').trim();
    if (value) parts.push(`${spec.flag} ${value}`);
  }
  const tail = rest.trim();
  if (tail) parts.push(tail);
  return parts.join(' ');
}
