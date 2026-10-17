/**
 * Copy text from browser and Tauri-rendered UI contexts.
 *
 * The modern Clipboard API is only exposed in secure browser contexts and may
 * reject when clipboard permission is denied. The selection-based fallback is
 * intentionally synchronous so it still runs inside the user's click gesture
 * on plain-HTTP LAN deployments.
 */
export async function copyTextToClipboard(text: string): Promise<void> {
  if (!text) return;

  try {
    if (typeof navigator !== 'undefined' && navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(text);
      return;
    }
  } catch {
    // Fall through to the user-gesture-compatible path below.
  }

  if (typeof document === 'undefined' || !document.body) {
    throw new Error('Clipboard is unavailable');
  }

  const textarea = document.createElement('textarea');
  textarea.value = text;
  textarea.setAttribute('readonly', 'true');
  textarea.setAttribute('aria-hidden', 'true');
  textarea.style.position = 'fixed';
  textarea.style.left = '-9999px';
  textarea.style.top = '0';
  textarea.style.opacity = '0';
  textarea.style.pointerEvents = 'none';

  const activeElement = typeof HTMLElement !== 'undefined' && document.activeElement instanceof HTMLElement
    ? document.activeElement
    : null;
  const selection = document.getSelection();
  const savedRanges: Range[] = [];
  if (selection) {
    for (let i = 0; i < selection.rangeCount; i += 1) {
      savedRanges.push(selection.getRangeAt(i).cloneRange());
    }
  }

  document.body.appendChild(textarea);
  textarea.focus({ preventScroll: true });
  textarea.select();
  textarea.setSelectionRange(0, textarea.value.length);

  let copied = false;
  try {
    copied = typeof document.execCommand === 'function' && document.execCommand('copy');
  } finally {
    textarea.remove();
    if (selection) {
      selection.removeAllRanges();
      savedRanges.forEach(range => selection.addRange(range));
    }
    activeElement?.focus({ preventScroll: true });
  }

  if (!copied) {
    throw new Error('Clipboard copy failed');
  }
}
