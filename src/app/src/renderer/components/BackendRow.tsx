import React from 'react';
import { BackendInfo } from '../utils/systemData';

const STATUS_INDICATOR_CLASS: Record<BackendInfo['state'], string> = {
  installed: 'available',
  installable: 'not-downloaded',
  update_required: 'update-required',
  action_required: 'update-required',
  unsupported: 'unsupported',
};

export interface BackendRowProps {
  recipeName: string;
  backendName: string;
  info: BackendInfo;
  isInstalling: boolean;
  /** Size label to display (e.g. "123 MB"), or null to hide */
  sizeLabel?: string | null;
  /** Status message override — falls back to info.message */
  statusMessage?: string;
  /** Show actions on hover only (BackendManager) vs always visible (ModelManager) */
  hoverActions?: boolean;
  isHovered?: boolean;
  onMouseEnter?: () => void;
  onMouseLeave?: () => void;
  /** Called to install/update the backend */
  onInstall?: (recipe: string, backend: string) => void;
  /** Called to uninstall the backend */
  onUninstall?: (recipe: string, backend: string) => void;
  /** Called to copy or handle the action string */
  onCopyAction?: (recipe: string, backend: string, action?: string) => void;
  /** Called to open the release URL */
  onOpenReleaseUrl?: (url: string) => void;
  /** Extra CSS class on the outer div */
  className?: string;
}

const DownloadIcon = () => (
  <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
    <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
    <polyline points="7 10 12 15 17 10" />
    <line x1="12" y1="15" x2="12" y2="3" />
  </svg>
);

const UninstallIcon = () => (
  <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
    <polyline points="3 6 5 6 21 6" />
    <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
  </svg>
);

const CopyIcon = () => (
  <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
    <rect x="9" y="9" width="13" height="13" rx="2" ry="2" />
    <path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1" />
  </svg>
);

const HelpIcon = () => (
  <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
    <circle cx="12" cy="12" r="10" />
    <path d="M9.09 9a3 3 0 0 1 5.83 1c0 2-3 3-3 3" />
    <line x1="12" y1="17" x2="12.01" y2="17" />
  </svg>
);

const BackendRow: React.FC<BackendRowProps> = ({
  recipeName,
  backendName,
  info,
  isInstalling,
  sizeLabel,
  statusMessage,
  hoverActions = true,
  isHovered = false,
  onMouseEnter,
  onMouseLeave,
  onInstall,
  onUninstall,
  onCopyAction,
  onOpenReleaseUrl,
  className,
}) => {
  const showActions = hoverActions ? isHovered : true;
  const message = statusMessage ?? info.message;
  const canInstall = info.state === 'installable' || info.state === 'update_required';
  const hasAction = canInstall || info.state === 'action_required';
  const canUninstall = info.state === 'installed' && info.can_uninstall !== false && backendName !== 'system';
  const isUpdate = info.state === 'update_required';

  return (
    <div
      className={`model-item backend-row-item ${className || ''}`}
      onMouseEnter={onMouseEnter}
      onMouseLeave={onMouseLeave}
    >
      <div className="model-item-content">
        <div className="model-info-left backend-row-main">
          <div className="backend-row-head">
            <span className="model-name backend-name">
              <span className={`model-status-indicator ${STATUS_INDICATOR_CLASS[info.state]}`}>●</span>
              {backendName}
            </span>
            {backendName !== 'system' && (
              <div className="backend-inline-meta">
                {onOpenReleaseUrl && info.release_url ? (
                  <button
                    className="backend-version-link"
                    onClick={() => onOpenReleaseUrl(info.release_url!)}
                    title="Open backend release page"
                  >
                    {info.version || 'Not installed'}
                  </button>
                ) : (
                  <span className="backend-version">{info.version || info.state}</span>
                )}
                {sizeLabel && (
                  <>
                    <span className="backend-meta-separator">•</span>
                    <span className="backend-size">{sizeLabel}</span>
                  </>
                )}
              </div>
            )}
          </div>
          {message && <div className="backend-status-message">{message}</div>}
        </div>
        {showActions && (
          <span className={`model-actions ${hoverActions ? '' : 'backend-setup-actions'}`}>
            {canInstall && onInstall && (
              <button
                className={`model-action-btn ${isUpdate ? 'update-btn' : 'download-btn'}`}
                title={isUpdate ? 'Update backend' : 'Install backend'}
                disabled={isInstalling}
                onClick={() => onInstall(recipeName, backendName)}
              >
                {isInstalling ? '…' : <DownloadIcon />}
              </button>
            )}
            {canUninstall && onUninstall && (
              <button
                className="model-action-btn delete-btn"
                title="Uninstall backend"
                onClick={() => onUninstall(recipeName, backendName)}
              >
                <UninstallIcon />
              </button>
            )}
            {hasAction && info.action && onCopyAction && (
              <button
                className="model-action-btn"
                title={info.action}
                onClick={() => onCopyAction(recipeName, backendName, info.action)}
              >
                {info.state === 'action_required' ? <HelpIcon /> : <CopyIcon />}
              </button>
            )}
          </span>
        )}
      </div>
    </div>
  );
};

export { STATUS_INDICATOR_CLASS };
export default BackendRow;
