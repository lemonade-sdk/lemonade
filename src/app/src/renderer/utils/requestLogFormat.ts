export function looksLikeJsonString(value: string): boolean {
  const trimmed = value.trim();
  return trimmed.startsWith('{') || trimmed.startsWith('[');
}

export function tryParseJsonString(value: string): unknown {
  if (!looksLikeJsonString(value)) {
    return value;
  }
  try {
    return JSON.parse(value);
  } catch {
    return value;
  }
}

/** Recursively expand JSON-encoded strings so nested payloads pretty-print. */
export function normalizeJsonForDisplay(value: unknown): unknown {
  if (typeof value === 'string') {
    let parsed: unknown = tryParseJsonString(value);
    if (typeof parsed === 'string' && looksLikeJsonString(parsed)) {
      parsed = tryParseJsonString(parsed);
    }
    if (parsed !== value) {
      return normalizeJsonForDisplay(parsed);
    }
    return value;
  }
  if (Array.isArray(value)) {
    return value.map((item) => normalizeJsonForDisplay(item));
  }
  if (value && typeof value === 'object') {
    return Object.fromEntries(
      Object.entries(value).map(([key, item]) => [key, normalizeJsonForDisplay(item)]),
    );
  }
  return value;
}

export function formatJsonBlock(value: unknown): string {
  if (value === null || value === undefined) {
    return '—';
  }

  const normalized = normalizeJsonForDisplay(value);
  if (typeof normalized === 'string') {
    return normalized;
  }

  return JSON.stringify(normalized, null, 2);
}

export function payloadUsesCharCountsOnly(value: unknown): boolean {
  if (!value || typeof value !== 'object') {
    return false;
  }
  const record = value as Record<string, unknown>;
  if (record.prompt && typeof record.prompt === 'object' && record.prompt !== null) {
    return 'char_count' in (record.prompt as Record<string, unknown>);
  }
  if (record.messages && typeof record.messages === 'object' && record.messages !== null) {
    return 'char_count' in (record.messages as Record<string, unknown>);
  }
  if (record.content && typeof record.content === 'object' && record.content !== null) {
    return 'char_count' in (record.content as Record<string, unknown>);
  }
  return false;
}
