import { useCallback, useEffect, useRef, useState } from 'react';
import { useFocusTrap } from './useFocusTrap';

export function useWorkspaceMobileRail() {
  const [isOpen, setIsOpen] = useState(false);
  const panelRef = useRef<HTMLElement>(null);
  const triggerRef = useRef<HTMLButtonElement>(null);

  useFocusTrap(panelRef, isOpen);

  const open = useCallback(() => setIsOpen(true), []);
  const close = useCallback(() => {
    setIsOpen(false);
    requestAnimationFrame(() => triggerRef.current?.focus());
  }, []);
  const toggle = useCallback(() => {
    if (isOpen) close();
    else setIsOpen(true);
  }, [close, isOpen]);

  useEffect(() => {
    if (!isOpen) return;
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') close();
    };
    document.addEventListener('keydown', onKeyDown);
    return () => document.removeEventListener('keydown', onKeyDown);
  }, [close, isOpen]);

  return { isOpen, panelRef, triggerRef, open, close, toggle };
}
