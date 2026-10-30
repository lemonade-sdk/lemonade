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

export function saveChatHistoryPreference(persist: boolean): void {
  try {
    localStorage.setItem(preferenceKey(), persist ? 'true' : 'false');
  } catch {
    // Browser storage is best effort.
  }
  try {
    window.dispatchEvent(new CustomEvent<boolean>(CHAT_HISTORY_PREFERENCE_EVENT, { detail: persist }));
  } catch {
    // Browser events are best effort.
  }
}

function storedConversationId(entry: unknown): string | undefined {
  const id = entry && typeof entry === 'object' ? (entry as { id?: unknown }).id : undefined;
  return typeof id === 'string' ? id : undefined;
}

// A generated id that is not written back changes on every load, so a delete
// made while saving is off could never match the stored entry.
export function assignMissingConversationIds(stored: unknown[], generateId: () => string): unknown[] | null {
  let changed = false;
  const next = stored.map(entry => {
    if (!entry || typeof entry !== 'object' || storedConversationId(entry) !== undefined) return entry;
    changed = true;
    return { ...entry, id: generateId() };
  });
  return changed ? next : null;
}

export function deletedConversationIds(previous: ReadonlySet<string>, current: ReadonlySet<string>): Set<string> {
  return new Set([...previous].filter(id => !current.has(id)));
}

// Only explicit deletions leave storage: another tab may have saved chats this tab never loaded.
export function removeDeletedConversations(stored: unknown[], deletedIds: ReadonlySet<string>): unknown[] {
  return stored.filter(entry => {
    const id = storedConversationId(entry);
    return id === undefined || !deletedIds.has(id);
  });
}

export function conversationVersion(conversation: { updatedAt: number; messages: unknown[] }): string {
  return `${conversation.updatedAt}:${conversation.messages.length}`;
}
