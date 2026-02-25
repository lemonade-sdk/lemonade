import React, { useEffect, useRef, useState } from 'react';
import { SystemCheck } from './utils/systemData';

interface SystemChecksModalProps {
  isOpen: boolean;
  onClose: (permanent: boolean) => void;
  checks: SystemCheck[];
}

const SystemChecksModal: React.FC<SystemChecksModalProps> = ({ isOpen, onClose, checks }) => {
  const cardRef = useRef<HTMLDivElement>(null);
  const [dontShowAgain, setDontShowAgain] = useState(false);

  useEffect(() => {
    if (!isOpen) return;

    const handleClickOutside = (event: MouseEvent) => {
      if (cardRef.current && !cardRef.current.contains(event.target as Node)) {
        onClose(dontShowAgain);
      }
    };

    const handleEscape = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        onClose(dontShowAgain);
      }
    };

    document.addEventListener('mousedown', handleClickOutside);
    document.addEventListener('keydown', handleEscape);

    return () => {
      document.removeEventListener('mousedown', handleClickOutside);
      document.removeEventListener('keydown', handleEscape);
    };
  }, [isOpen, onClose, dontShowAgain]);

  if (!isOpen) return null;

  const getSeverityIcon = (severity: string) => {
    switch (severity) {
      case 'error':
        return (
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="#ff4444" strokeWidth="2">
            <circle cx="12" cy="12" r="10" />
            <line x1="15" y1="9" x2="9" y2="15" />
            <line x1="9" y1="9" x2="15" y2="15" />
          </svg>
        );
      case 'warning':
        return (
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="#ffaa00" strokeWidth="2">
            <path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z" />
            <line x1="12" y1="9" x2="12" y2="13" />
            <line x1="12" y1="17" x2="12.01" y2="17" />
          </svg>
        );
      default:
        return (
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="#4488ff" strokeWidth="2">
            <circle cx="12" cy="12" r="10" />
            <line x1="12" y1="16" x2="12" y2="12" />
            <line x1="12" y1="8" x2="12.01" y2="8" />
          </svg>
        );
    }
  };

  const handleOpenUrl = (url: string) => {
    if (window?.api?.openExternal) {
      window.api.openExternal(url);
    } else {
      window.open(url, '_blank');
    }
  };

  return (
    <div className="modal-overlay">
      <div ref={cardRef} className="modal-card system-checks-modal">
        <div className="modal-header">
          <h2>System Checks</h2>
          <button className="modal-close-btn" onClick={() => onClose(dontShowAgain)}>
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
              <line x1="18" y1="6" x2="6" y2="18" />
              <line x1="6" y1="6" x2="18" y2="18" />
            </svg>
          </button>
        </div>
        <div className="modal-content">
          {checks.length === 0 ? (
            <div className="system-check-empty">
              <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                <circle cx="12" cy="12" r="10" />
                <polyline points="9 12 12 15 16 10" />
              </svg>
              <p>All system checks passed!</p>
            </div>
          ) : (
            <div className="system-checks-list">
              {checks.map((check) => (
                <div key={check.id} className={`system-check-item severity-${check.severity}`}>
                  <div className="check-header">
                    <div className="check-icon">{getSeverityIcon(check.severity)}</div>
                    <div className="check-info">
                      <h3>{check.title}</h3>
                      <span className="check-platform">{check.platform}</span>
                    </div>
                  </div>
                  <p className="check-message">{check.message}</p>
                  {check.fix_url && (
                    <button
                      className="check-fix-btn"
                      onClick={() => handleOpenUrl(check.fix_url!)}
                    >
                      View Fix Instructions
                      <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                        <path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6" />
                        <polyline points="15 3 21 3 21 9" />
                        <line x1="10" y1="14" x2="21" y2="3" />
                      </svg>
                    </button>
                  )}
                </div>
              ))}
            </div>
          )}
        </div>
        <div className="modal-footer">
          <label className="dont-show-again-label">
            <input
              type="checkbox"
              checked={dontShowAgain}
              onChange={(e) => setDontShowAgain(e.target.checked)}
            />
            <span>Don't show this again</span>
          </label>
          <button className="btn-secondary" onClick={() => onClose(dontShowAgain)}>
            Close
          </button>
        </div>
      </div>
    </div>
  );
};

export default SystemChecksModal;
