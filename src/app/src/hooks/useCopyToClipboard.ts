import { useState } from 'react';
import { inspectStore } from '../inspectStore';

export function useCopyToClipboard(defaultLabel = 'text', duration = 2000) {
  const [isCopied, setIsCopied] = useState(false);

  const copy = async (text: string, label?: string) => {
    try {
      await navigator.clipboard.writeText(text);
      inspectStore.showToast(`Copied ${label || defaultLabel}`);
      setIsCopied(true);
      setTimeout(() => setIsCopied(false), duration);
      return true;
    } catch (err) {
      console.error('Failed to copy to clipboard', err);
      return false;
    }
  };

  return { isCopied, copy };
}
