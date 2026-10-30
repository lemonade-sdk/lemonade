import { storageKey } from '../../storage';

export const CHAT_HISTORY_PREFERENCE_EVENT = 'lemonade:chat-history-preference-changed';

const CHAT_HISTORY_PREFERENCE_KEY = 'persist_conversations';

function preferenceKey(): string {
  return storageKey(CHAT_HISTORY_PREFERENCE_KEY);
}

export function loadChatHistoryPreference(): boolean {
  try {
    const stored = localStorage.getItem(preferenceKey());
    return stored === null || stored === 'true';
  } catch {
    return false;
  }
}

export function tryWriteChatHistoryPreference(persist: boolean): boolean {
  try {
    localStorage.setItem(preferenceKey(), persist ? 'true' : 'false');
    return true;
  } catch {
    return false;
  }
}

export function saveChatHistoryPreference(persist: boolean): void {
  tryWriteChatHistoryPreference(persist);
  try {
    window.dispatchEvent(new CustomEvent<boolean>(CHAT_HISTORY_PREFERENCE_EVENT, { detail: persist }));
  } catch {
    // Browser events are best effort.
  }
}

// Legacy entries have no id. Deriving one from their content keeps it stable
// across loads without writing storage, so a delete made while saving is off
// still matches the stored entry.
export function storedConversationId(entry: unknown): string | undefined {
  if (!entry || typeof entry !== 'object') return undefined;
  const id = (entry as { id?: unknown }).id;
  if (typeof id === 'string') return id;
  const text = JSON.stringify(entry);
  let hash = 5381;
  for (let i = 0; i < text.length; i += 1) hash = ((hash << 5) + hash + text.charCodeAt(i)) | 0;
  return `legacy-${(hash >>> 0).toString(36)}`;
}

// Match legacy entries by content and occurrence, without writing during opt-out hydration.
// Identical ID-less entries still need different identities so deleting one keeps the other.
export function storedConversationIds(entries: readonly unknown[]): (string | undefined)[] {
  const occupied = new Set<string>();
  for (const entry of entries) {
    if (entry && typeof entry === 'object') {
      const id = (entry as { id?: unknown }).id;
      if (typeof id === 'string') occupied.add(id);
    }
  }
  const nextOccurrence = new Map<string, number>();
  return entries.map(entry => {
    const id = storedConversationId(entry);
    if (id === undefined) return undefined;
    if (entry && typeof entry === 'object' && typeof (entry as { id?: unknown }).id === 'string') return id;
    let occurrence = nextOccurrence.get(id) ?? 0;
    let candidate = occurrence === 0 ? id : `${id}-${occurrence}`;
    while (occupied.has(candidate)) {
      occurrence += 1;
      candidate = `${id}-${occurrence}`;
    }
    nextOccurrence.set(id, occurrence + 1);
    occupied.add(candidate);
    return candidate;
  });
}

export function deletedConversationIds(previous: ReadonlySet<string>, current: ReadonlySet<string>): Set<string> {
  return new Set([...previous].filter(id => !current.has(id)));
}

// Only explicit deletions leave storage: another tab may have saved chats this tab never loaded.
export function removeDeletedConversations(stored: unknown[], deletedIds: ReadonlySet<string>): unknown[] {
  const ids = storedConversationIds(stored);
  if (!ids.some(id => id !== undefined && deletedIds.has(id))) return stored;
  return stored.flatMap((entry, index) => {
    const id = ids[index];
    if (id !== undefined && deletedIds.has(id)) return [];
    // Once a duplicate is removed, preserve the survivor's occurrence ID across reloads.
    if (id !== undefined && id !== storedConversationId(entry) && entry && typeof entry === 'object' && !Array.isArray(entry)) {
      return [{ ...entry, id }];
    }
    return [entry];
  });
}

export function conversationVersion(conversation: { updatedAt: number; messages: unknown[] }): string {
  return `${conversation.updatedAt}:${conversation.messages.length}`;
}
