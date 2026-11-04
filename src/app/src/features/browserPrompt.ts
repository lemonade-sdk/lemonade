export const BROWSER_PROMPT_DRAFT_KEY = 'lemonade:pending_browser_prompt';

export function saveBrowserPromptDraft(text: string): boolean {
  const hasDraft = !!text.trim();
  let stored = false;
  try {
    if (hasDraft) sessionStorage.setItem(BROWSER_PROMPT_DRAFT_KEY, text);
    else sessionStorage.removeItem(BROWSER_PROMPT_DRAFT_KEY);
    stored = true;
  } catch { /* Fall back to the URL if tab storage is blocked. */ }

  // Keep unsent edits recoverable if storage fails, and always clear the URL
  // fallback after the draft is submitted or discarded.
  if (!stored || !hasDraft) {
    try {
      const url = new URL(window.location.href);
      if (!stored && hasDraft) url.searchParams.set('q', text);
      else url.searchParams.delete('q');
      if (url.href !== window.location.href) {
        window.history.replaceState(window.history.state, '', url.toString());
      }
    } catch { /* History access can also be unavailable. */ }
  }
  return stored;
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
    const text = url.searchParams.get('q')?.trim() || '';
    const stored = saveBrowserPromptDraft(text);
    if (stored || !text) url.searchParams.delete('q');
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
