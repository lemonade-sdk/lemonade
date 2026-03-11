import React, { useState, useCallback } from 'react';

import { useSystem } from '../hooks/useSystem';
import { useBackendInstall } from '../hooks/useBackendInstall';
import { useConfirmDialog } from '../ConfirmDialog';
import { RECIPE_DISPLAY_NAMES } from '../utils/recipeNames';
import BackendRow from './BackendRow';

interface ConnectedBackendRowProps {
  recipe: string;
  backend: string;
  showError: (msg: string) => void;
  showSuccess: (msg: string) => void;
  /**
   * 'full'   — hover-to-show actions, uninstall, release link (BackendManager)
   * 'banner' — always-visible actions, no uninstall (ModelManager)
   */
  variant?: 'full' | 'banner';
  /** Override the default status message */
  statusMessage?: string;
  /** Size label (e.g. "123 MB"), or null to hide */
  sizeLabel?: string | null;
  /** Handler to open release page externally (full variant only) */
  onOpenReleaseUrl?: (url: string) => void;
}

const ConnectedBackendRow: React.FC<ConnectedBackendRowProps> = ({
  recipe,
  backend,
  showError,
  showSuccess,
  variant = 'full',
  statusMessage,
  sizeLabel,
  onOpenReleaseUrl,
}) => {
  const { systemInfo } = useSystem();
  const { handleInstall, handleUninstall, handleCopyAction, isInstalling } = useBackendInstall({ showError, showSuccess });
  const { confirm, ConfirmDialog } = useConfirmDialog();
  const [isHovered, setIsHovered] = useState(false);

  const backendInfo = systemInfo?.recipes?.[recipe]?.backends?.[backend];
  if (!backendInfo) return null;

  const isFull = variant === 'full';
  const info = statusMessage ? { ...backendInfo, message: statusMessage } : backendInfo;

  const onConfirmUninstall = useCallback(async (r: string, b: string) => {
    const confirmed = await confirm({
      title: 'Uninstall Backend',
      message: `Are you sure you want to uninstall ${RECIPE_DISPLAY_NAMES[r] || r} ${b}?`,
      confirmText: 'Uninstall',
      cancelText: 'Cancel',
      danger: true,
    });
    if (confirmed) await handleUninstall(r, b);
  }, [confirm, handleUninstall]);

  return (
    <>
      <ConfirmDialog />
      <BackendRow
        recipeName={recipe}
        backendName={backend}
        info={info}
        isInstalling={isInstalling(recipe, backend)}
        sizeLabel={sizeLabel}
        hoverActions={isFull}
        isHovered={isFull ? isHovered : undefined}
        onMouseEnter={isFull ? () => setIsHovered(true) : undefined}
        onMouseLeave={isFull ? () => setIsHovered(false) : undefined}
        onInstall={handleInstall}
        onUninstall={isFull ? onConfirmUninstall : undefined}
        onCopyAction={handleCopyAction}
        onOpenReleaseUrl={isFull ? onOpenReleaseUrl : undefined}
        className={isFull ? undefined : 'backend-setup-banner'}
      />
    </>
  );
};

export default ConnectedBackendRow;
