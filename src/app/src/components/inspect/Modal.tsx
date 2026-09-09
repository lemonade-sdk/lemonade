import React, { useEffect, useId, useRef } from 'react';
import { Icon } from '../Icon';

interface ModalProps {
  isOpen: boolean;
  onClose: () => void;
  title: string;
  children: React.ReactNode;
  maxWidth?: string;
  ariaLabelledBy?: string;
  className?: string;
}

export default function Modal({
  isOpen,
  onClose,
  title,
  children,
  maxWidth = '600px',
  ariaLabelledBy,
  className,
}: ModalProps) {
  const containerRef = useRef<HTMLDivElement>(null);
  const titleRef = useRef<HTMLHeadingElement>(null);
  const previousFocusRef = useRef<HTMLElement | null>(null);
  const generatedTitleId = useId();
  const titleId = ariaLabelledBy || `inspect-modal-title-${generatedTitleId.replace(/:/g, '')}`;

  useEffect(() => {
    if (!isOpen) return;
    previousFocusRef.current = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    titleRef.current?.focus();
    return () => {
      const previous = previousFocusRef.current;
      previousFocusRef.current = null;
      if (previous?.isConnected) previous.focus();
    };
  }, [isOpen]);

  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && isOpen) {
        onClose();
      }
      if (e.key === 'Tab' && isOpen && containerRef.current) {
        const focusable = containerRef.current.querySelectorAll<HTMLElement>(
          'a[href], area[href], input:not([disabled]), select:not([disabled]), textarea:not([disabled]), button:not([disabled]), [tabIndex="0"]'
        );
        if (focusable.length > 0) {
          const first = focusable[0];
          const last = focusable[focusable.length - 1];
          if (e.shiftKey) {
            // The dialog title receives initial focus but is deliberately not
            // in the normal tab order. Shift+Tab from that title must still
            // stay inside the modal rather than escaping to the page behind it.
            if (document.activeElement === first || document.activeElement === titleRef.current) {
              last.focus();
              e.preventDefault();
            }
          } else {
            if (document.activeElement === last) {
              first.focus();
              e.preventDefault();
            } else if (document.activeElement === titleRef.current) {
              first.focus();
              e.preventDefault();
            }
          }
        }
      }
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [isOpen, onClose]);

  if (!isOpen) return null;

  return (
    <div
      ref={containerRef}
      className="inspect-modal-overlay"
      onClick={onClose}
      role="dialog"
      aria-modal="true"
      aria-labelledby={titleId}
    >
      <div
        className={`inspect-modal-content flex-col${className ? ` ${className}` : ''}`}
        onClick={(e) => e.stopPropagation()}
        style={{ maxWidth }}
      >
        <div className="inspect-modal-header">
          <h4 id={titleId} ref={titleRef} tabIndex={-1}>
            {title}
          </h4>
          <button
            type="button"
            className="close-modal-btn"
            onClick={onClose}
            aria-label="Close modal"
          >
            <Icon name="x" size={14} />
          </button>
        </div>
        {children}
      </div>
    </div>
  );
}
