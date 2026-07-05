function legacyCopy(text: string): void {
  const ta = document.createElement('textarea');
  ta.value = String(text);
  ta.setAttribute('readonly', '');
  ta.style.position = 'fixed';
  ta.style.left = '-9999px';
  document.body.appendChild(ta);
  try {
    ta.select();
    if (!document.execCommand('copy')) {
      throw new Error('Legacy clipboard copy failed');
    }
  } finally {
    document.body.removeChild(ta);
  }
}

export async function writeClipboard(text: string): Promise<void> {
  if (window.api?.writeClipboard) {
    return window.api.writeClipboard(text);
  }
  if (navigator.clipboard) {
    try {
      await navigator.clipboard.writeText(text);
      return;
    } catch {
      // Ignore clipboard errors and fall back to legacy method
    }
  }
  legacyCopy(text);
}
