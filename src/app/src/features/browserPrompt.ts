export const BROWSER_PROMPT_DRAFT_KEY = 'lemonade:pending_browser_prompt';

export function saveBrowserPromptDraft(text: string): void {
  try {
    if (text.trim()) sessionStorage.setItem(BROWSER_PROMPT_DRAFT_KEY, text);
    else sessionStorage.removeItem(BROWSER_PROMPT_DRAFT_KEY);
  } catch { /* Session storage is best-effort for a tab-local draft. */ }
}

export function readBrowserPromptDraft(): string {
  try {
    const params = new URLSearchParams(window.location.search);
    if (params.has('q')) return params.get('q')?.trim() || '';
    return sessionStorage.getItem(BROWSER_PROMPT_DRAFT_KEY) || '';
  } catch { return ''; }
}

// This must run before React mounts so a fast first response cannot recreate
// a pending draft after it has already been submitted.
export function captureBrowserPromptFromUrl(): void {
  if (typeof window === 'undefined') return;
  try {
    const url = new URL(window.location.href);
    if (!url.searchParams.has('q')) return;
    saveBrowserPromptDraft(url.searchParams.get('q')?.trim() || '');
    url.searchParams.delete('q');
    url.searchParams.delete('view');
    url.searchParams.delete('logs');
    url.hash = '#/chat';
    window.history.replaceState(window.history.state, '', url.toString());
  } catch { /* Keep the original URL if browser history is unavailable. */ }
}

export function captureBrowserDraftSend(
  overrideText: string | undefined,
  fromBrowserPrompt: boolean,
  pendingPrompt: string,
  wasHandled: boolean,
): boolean {
  return fromBrowserPrompt || (overrideText === undefined && !!pendingPrompt && !wasHandled);
}

export function hasBrowserPromptContext(messages: readonly { fromBrowserPrompt?: boolean }[]): boolean {
  return messages.some(message => message.fromBrowserPrompt === true);
}
