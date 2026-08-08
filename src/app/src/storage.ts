const STORAGE_PREFIX = 'lemonade:';
const MIGRATION_KEY = 'lemonade_storage_migrated_v2';
const PREVIOUS_MIGRATION_KEY = 'lemonade_storage_migrated_v1';
const MIGRATION_CONFLICTS_KEY = `${STORAGE_PREFIX}storage_migration_conflicts_v1`;

const LEGACY_KEYS: Record<string, string> = {
  lemonade_conversations: 'conversations',
  lemonade_active_conversation: 'active_conversation',
  lemonade_persist_conversations: 'persist_conversations',
  lemonade_use_tools: 'use_tools',
  lemonade_custom_models: 'custom_models',
};

const OBSOLETE_LEGACY_CONFIGURATION_KEYS = [
  'lemonade_user_presets',
  'lemonade_applied_presets',
  `${STORAGE_PREFIX}user_presets`,
  `${STORAGE_PREFIX}applied_presets`,
  `${STORAGE_PREFIX}backend_presets`,
  `${STORAGE_PREFIX}running_presets`,
] as const;

let migrationComplete = false;

function storageAvailable(): boolean {
  return typeof localStorage !== 'undefined';
}

function stableJson(value: unknown): string {
  return JSON.stringify(value);
}

function isObject(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

function mergeConversations(existing: unknown[], incoming: unknown[], scope: string): { value: unknown[]; remap: Record<string, string> } {
  const merged = [...existing];
  const usedIds = new Set(
    merged
      .filter(isObject)
      .map(item => typeof item.id === 'string' ? item.id : '')
      .filter(Boolean),
  );
  const remap: Record<string, string> = {};

  incoming.forEach(item => {
    if (!isObject(item) || typeof item.id !== 'string') {
      if (!merged.some(existingItem => stableJson(existingItem) === stableJson(item))) merged.push(item);
      return;
    }
    const same = merged.find(existingItem => isObject(existingItem) && existingItem.id === item.id);
    if (!same) {
      merged.push(item);
      usedIds.add(item.id);
      return;
    }
    if (stableJson(same) === stableJson(item)) return;

    const base = `${item.id}-migrated-${scope.replace(/[^a-zA-Z0-9]+/g, '-')}`;
    let nextId = base;
    let suffix = 2;
    while (usedIds.has(nextId)) nextId = `${base}-${suffix++}`;
    usedIds.add(nextId);
    remap[item.id] = nextId;
    merged.push({ ...item, id: nextId });
  });

  return { value: merged, remap };
}

function mergeJson(existing: unknown, incoming: unknown, key: string, scope: string, conflicts: Record<string, string>): unknown {
  if (stableJson(existing) === stableJson(incoming)) return existing;

  if (Array.isArray(existing) && Array.isArray(incoming)) {
    if (key === 'conversations') return mergeConversations(existing, incoming, scope).value;
    return [...existing, ...incoming.filter(item => !existing.some(existingItem => stableJson(existingItem) === stableJson(item)))];
  }

  if (isObject(existing) && isObject(incoming)) {
    const merged = { ...existing };
    for (const [childKey, childValue] of Object.entries(incoming)) {
      if (!(childKey in merged)) {
        merged[childKey] = childValue;
        continue;
      }
      merged[childKey] = mergeJson(merged[childKey], childValue, childKey, scope, conflicts);
    }
    return merged;
  }

  conflicts[`${scope}:${key}`] = typeof incoming === 'string' ? incoming : stableJson(incoming);
  return existing;
}

function rememberConflicts(store: Storage, conflicts: Record<string, string>): boolean {
  if (Object.keys(conflicts).length === 0) return true;
  try {
    const existing = JSON.parse(store.getItem(MIGRATION_CONFLICTS_KEY) || '{}');
    const archive = isObject(existing) ? existing : {};
    Object.assign(archive, conflicts);
    store.setItem(MIGRATION_CONFLICTS_KEY, JSON.stringify(archive));
    return true;
  } catch {
    return false;
  }
}

function copyIfMissing(store: Storage, source: string, target: string): boolean {
  if (store.getItem(target) !== null) return true;
  const value = store.getItem(source);
  if (value === null) return true;
  try {
    store.setItem(target, value);
    return true;
  } catch {
    return false;
  }
}

function removeObsoleteLegacyConfiguration(store: Storage): void {
  for (const key of OBSOLETE_LEGACY_CONFIGURATION_KEYS) store.removeItem(key);
}

function migrateScopedValue(
  store: Storage,
  source: string,
  target: string,
  key: string,
  scope: string,
  conversationRemaps: Record<string, Record<string, string>>,
): boolean {
  const incoming = store.getItem(source);
  if (incoming === null) return true;
  const existing = store.getItem(target);
  if (existing === null) {
    try {
      if (key === 'active_conversation') {
        const remapped = conversationRemaps[scope]?.[incoming] || incoming;
        store.setItem(target, remapped);
      } else {
        store.setItem(target, incoming);
      }
      return true;
    } catch {
      return false;
    }
  }

  const remappedIncoming = key === 'active_conversation'
    ? conversationRemaps[scope]?.[incoming] || incoming
    : incoming;
  let merged = existing;
  const conflicts: Record<string, string> = {};
  try {
    const existingJson = JSON.parse(existing);
    const incomingJson = JSON.parse(remappedIncoming);
    if (key === 'conversations' && isObject(existingJson) && isObject(incomingJson)
      && Array.isArray(existingJson.conversations) && Array.isArray(incomingJson.conversations)) {
      const result = mergeConversations(existingJson.conversations, incomingJson.conversations, scope);
      conversationRemaps[scope] = result.remap;
      merged = JSON.stringify({ ...existingJson, ...incomingJson, version: 3, conversations: result.value });
    } else {
      merged = JSON.stringify(mergeJson(existingJson, incomingJson, key, scope, conflicts));
    }
  } catch {
    if (existing !== remappedIncoming) conflicts[`${scope}:${key}`] = remappedIncoming;
  }

  try {
    if (merged !== existing) store.setItem(target, merged);
  } catch {
    return false;
  }
  return rememberConflicts(store, conflicts);
}

export function migrateClientStorage(): void {
  if (migrationComplete || !storageAvailable()) return;
  migrationComplete = true;

  try {
    removeObsoleteLegacyConfiguration(localStorage);
    if (localStorage.getItem(MIGRATION_KEY) === 'true') return;

    for (const [legacyKey, key] of Object.entries(LEGACY_KEYS)) {
      if (!copyIfMissing(localStorage, legacyKey, `${STORAGE_PREFIX}${key}`)) return;
    }

    const scopedKeys = Object.keys(localStorage)
      .map(key => ({ key, match: key.match(/^lemonade:(?:guest:shared|user:[^:]+):(.+)$/) }))
      .filter((entry): entry is { key: string; match: RegExpMatchArray } => !!entry.match);
    const scopedByScope = new Map<string, { key: string; name: string }[]>();
    for (const { key, match } of scopedKeys) {
      const scope = key.slice(STORAGE_PREFIX.length, key.lastIndexOf(':'));
      const name = match[1];
      const entries = scopedByScope.get(scope) || [];
      entries.push({ key, name });
      scopedByScope.set(scope, entries);
    }
    const conversationRemaps: Record<string, Record<string, string>> = {};
    for (const [scope, entries] of [...scopedByScope.entries()].sort(([a], [b]) => a.localeCompare(b))) {
      entries.sort((a, b) => (a.name === 'conversations' ? -1 : b.name === 'conversations' ? 1 : a.name.localeCompare(b.name)));
      for (const { key, name } of entries) {
        const target = `${STORAGE_PREFIX}${name}`;
        if (migrateScopedValue(localStorage, key, target, name, scope, conversationRemaps)) {
          localStorage.removeItem(key);
        }
      }
    }

    localStorage.removeItem('lemonade_accounts_v1');
    localStorage.removeItem('lemonade_account_session_v1');
    localStorage.removeItem(PREVIOUS_MIGRATION_KEY);
    localStorage.setItem(MIGRATION_KEY, 'true');
  } catch {
    // Browser storage is best-effort; callers can continue with in-memory defaults.
  }
}

export function storageKey(key: string): string {
  migrateClientStorage();
  return `${STORAGE_PREFIX}${key}`;
}

export function clearClientStorage(): void {
  if (!storageAvailable()) return;
  try {
    Object.keys(localStorage)
      .filter(key => key.startsWith(STORAGE_PREFIX)
        || key === MIGRATION_KEY
        || key === PREVIOUS_MIGRATION_KEY
        || key in LEGACY_KEYS
        || OBSOLETE_LEGACY_CONFIGURATION_KEYS.includes(key as typeof OBSOLETE_LEGACY_CONFIGURATION_KEYS[number])
        || key === 'lemonade_theme'
        || key === 'lemonade_current_view')
      .forEach(key => localStorage.removeItem(key));
    Object.keys(sessionStorage)
      .filter(key => key.startsWith(STORAGE_PREFIX)
        || key in LEGACY_KEYS
        || OBSOLETE_LEGACY_CONFIGURATION_KEYS.includes(key as typeof OBSOLETE_LEGACY_CONFIGURATION_KEYS[number]))
      .forEach(key => sessionStorage.removeItem(key));
  } catch {
    // Ignore unavailable browser storage.
  }
}
