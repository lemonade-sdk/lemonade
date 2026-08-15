import { useState } from 'react';
import { inspectStore } from '../inspectStore';
import { copyTextToClipboard } from '../clipboard';

export interface CopyToClipboardOptions {
  successToast?: boolean;
  failureToast?: boolean;
}

export function useCopyToClipboard(defaultLabel = 'text', duration = 2000) {
  const [isCopied, setIsCopied] = useState(false);

  const copy = async (text: string, label?: string, options: CopyToClipboardOptions = {}) => {
    try {
      await copyTextToClipboard(text);
      if (options.successToast !== false) {
        inspectStore.showToast({ code: 'copied', label: label || defaultLabel });
      }
      setIsCopied(true);
      setTimeout(() => setIsCopied(false), duration);
      return true;
    } catch (err) {
      console.error('Failed to copy to clipboard', err);
      if (options.failureToast !== false) inspectStore.showToast({ code: 'copy_failed' });
      return false;
    }
  };

  return { isCopied, copy };
}
