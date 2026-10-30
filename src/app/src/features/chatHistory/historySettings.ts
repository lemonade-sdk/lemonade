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
