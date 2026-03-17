import React, { useEffect, useRef, useState } from 'react';

interface KeepFilesOption {
  label: string;
  defaultChecked?: boolean;
}

interface ConfirmDialogProps {
  isOpen: boolean;
  title: string;
  message: string;
  confirmText?: string;
  cancelText?: string;
  danger?: boolean;
  keepFilesOption?: KeepFilesOption;
  keepFiles?: boolean;
  onKeepFilesChange?: (keepFiles: boolean) => void;
  onConfirm: () => void;
  onCancel: () => void;
}

const ConfirmDialog: React.FC<ConfirmDialogProps> = ({
  isOpen,
  title,
  message,
  confirmText = 'Confirm',
  cancelText = 'Cancel',
  danger = false,
  keepFilesOption,
  keepFiles,
  onKeepFilesChange,
  onConfirm,
  onCancel
}) => {
  const dialogRef = useRef<HTMLDivElement>(null);
  const confirmButtonRef = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    if (isOpen && confirmButtonRef.current) {
      // Focus the confirm button when dialog opens
      confirmButtonRef.current.focus();
    }
  }, [isOpen]);

  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if (!isOpen) return;

      if (e.key === 'Escape') {
        onCancel();
      } else if (e.key === 'Enter') {
        onConfirm();
      }
    };

    document.addEventListener('keydown', handleKeyDown);
    return () => document.removeEventListener('keydown', handleKeyDown);
  }, [isOpen, onConfirm, onCancel]);

  if (!isOpen) return null;

  const handleOverlayClick = (e: React.MouseEvent) => {
    if (e.target === e.currentTarget) {
      onCancel();
    }
  };

  return (
    <div className="confirm-dialog-overlay" onClick={handleOverlayClick}>
      <div className="confirm-dialog" ref={dialogRef}>
        <h3 className="confirm-dialog-title">{title}</h3>
        <p className="confirm-dialog-message">{message}</p>
        {keepFilesOption && (
          <label className="confirm-dialog-checkbox">
            <input
              type="checkbox"
              checked={keepFiles ?? false}
              onChange={(e) => onKeepFilesChange?.(e.target.checked)}
            />
            <span>{keepFilesOption.label}</span>
          </label>
        )}
        <div className="confirm-dialog-actions">
          <button
            className="confirm-dialog-btn confirm-dialog-btn-cancel"
            onClick={onCancel}
          >
            {cancelText}
          </button>
          <button
            ref={confirmButtonRef}
            className={`confirm-dialog-btn ${danger ? 'confirm-dialog-btn-danger' : 'confirm-dialog-btn-confirm'}`}
            onClick={onConfirm}
          >
            {confirmText}
          </button>
        </div>
      </div>
    </div>
  );
};

// Hook for managing confirm dialogs
interface ConfirmOptions {
  title: string;
  message: string;
  confirmText?: string;
  cancelText?: string;
  danger?: boolean;
  keepFilesOption?: KeepFilesOption;
}

export interface ConfirmResult {
  confirmed: boolean;
  keepFiles: boolean;
}

export const useConfirmDialog = () => {
  const [isOpen, setIsOpen] = useState(false);
  const [options, setOptions] = useState<ConfirmOptions>({
    title: '',
    message: '',
  });
  const resolveRef = useRef<((value: ConfirmResult) => void) | null>(null);
  const [keepFiles, setKeepFiles] = useState(false);

  const confirm = (opts: ConfirmOptions): Promise<ConfirmResult> => {
    return new Promise((resolve) => {
      setKeepFiles(opts.keepFilesOption?.defaultChecked ?? false);
      setOptions(opts);
      setIsOpen(true);
      resolveRef.current = resolve;
    });
  };

  const handleConfirm = () => {
    setIsOpen(false);
    resolveRef.current?.({ confirmed: true, keepFiles });
  };

  const handleCancel = () => {
    setIsOpen(false);
    resolveRef.current?.({ confirmed: false, keepFiles: false });
  };

  const ConfirmDialogComponent = () => (
    <ConfirmDialog
      isOpen={isOpen}
      title={options.title}
      message={options.message}
      confirmText={options.confirmText}
      cancelText={options.cancelText}
      danger={options.danger}
      keepFilesOption={options.keepFilesOption}
      keepFiles={keepFiles}
      onKeepFilesChange={(v) => setKeepFiles(v)}
      onConfirm={handleConfirm}
      onCancel={handleCancel}
    />
  );

  return {
    confirm,
    ConfirmDialog: ConfirmDialogComponent
  };
};

export default ConfirmDialog;
