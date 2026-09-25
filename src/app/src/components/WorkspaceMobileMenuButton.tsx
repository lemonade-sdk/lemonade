import React from 'react';
import { Icon } from './Icon';
import { useI18n } from '../i18n';

interface WorkspaceMobileMenuButtonProps {
  menuLabel: string;
  panelId: string;
  expanded: boolean;
  onClick: () => void;
  triggerRef?: React.Ref<HTMLButtonElement>;
}

const WorkspaceMobileMenuButton: React.FC<WorkspaceMobileMenuButtonProps> = ({
  menuLabel,
  panelId,
  expanded,
  onClick,
  triggerRef,
}) => {
  const { t } = useI18n();
  return (
    <button
      ref={triggerRef}
      type="button"
      className="workspace-mobile-menu-button"
      aria-label={t(menuLabel)}
      aria-expanded={expanded}
      aria-controls={panelId}
      onClick={onClick}
    >
      <Icon name="menu" size={18} aria-hidden="true" />
    </button>
  );
};

export default WorkspaceMobileMenuButton;
