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

export function retainSavedConversations(stored: unknown[], openIds: ReadonlySet<string>): unknown[] {
  return stored.filter(entry => {
    const id = entry && typeof entry === 'object' ? (entry as { id?: unknown }).id : undefined;
    return typeof id !== 'string' || openIds.has(id);
  });
}

export function conversationVersion(conversation: { updatedAt: number; messages: unknown[] }): string {
  return `${conversation.updatedAt}:${conversation.messages.length}`;
}
