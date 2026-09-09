import { storageKey } from '../../storage';

export const CHAT_HISTORY_PREFERENCE_EVENT = 'lemonade:chat-history-preference-changed';

const CHAT_HISTORY_PREFERENCE_KEY = 'persist_conversations';

function preferenceKey(): string {
  return storageKey(CHAT_HISTORY_PREFERENCE_KEY);
}

export function loadChatHistoryPreference(): boolean {
  try {
    return localStorage.getItem(preferenceKey()) === 'true';
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
