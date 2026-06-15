function legacyCopy(text: string): Promise<void> {
  const ta = document.createElement('textarea');
  ta.value = text;
  ta.style.position = 'fixed';
  ta.style.opacity = '0';
  ta.style.pointerEvents = 'none';
  document.body.appendChild(ta);
  ta.select();
  document.execCommand('copy');
  document.body.removeChild(ta);
  return Promise.resolve();
}

export async function writeClipboard(text: string): Promise<void> {
  if (window.api?.writeClipboard) {
    return window.api.writeClipboard(text);
  }
  try {
    return await navigator.clipboard.writeText(text);
  } catch {
    legacyCopy(text);
  }
}
