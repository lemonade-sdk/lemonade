export async function writeClipboard(text: string): Promise<void> {
  if (window.api?.writeClipboard) {
    await window.api.writeClipboard(text);
  } else {
    await navigator.clipboard.writeText(text);
  }
}
