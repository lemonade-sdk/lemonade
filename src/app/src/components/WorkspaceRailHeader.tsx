import React from 'react';
import { Icon } from './Icon';
import { useI18n } from '../i18n';
import type { IconName } from './Icon';

interface WorkspaceRailHeaderProps {
  title: string;
  sidebarLabel?: string;
  icon?: IconName;
  purpose: 'filter' | 'history' | 'navigation';
  collapsed: boolean;
  onToggle: () => void;
  onMobileClose?: () => void;
}

const WorkspaceRailHeader: React.FC<WorkspaceRailHeaderProps> = ({
  title,
  sidebarLabel = title,
  icon,
  purpose,
  collapsed,
  onToggle,
  onMobileClose,
}) => {
  const { t } = useI18n();
  const translatedSidebarLabel = t(sidebarLabel);
  return (
    <div className={`workspace-rail__header workspace-rail__header--${purpose}`}>
      <span className="workspace-rail__context" aria-hidden="true">
        <Icon name={icon ?? (purpose === 'filter' ? 'funnel' : purpose === 'history' ? 'clock' : 'layers')} size={12} />
        <strong className="workspace-rail__title">{t(title)}</strong>
      </span>
      <button
        type="button"
        className="workspace-rail__toggle"
        onClick={onToggle}
        aria-expanded={!collapsed}
        aria-label={`${t(collapsed ? 'Expand' : 'Collapse')} ${translatedSidebarLabel} ${t('sidebar')}`}
        title={`${t(collapsed ? 'Expand' : 'Collapse')} ${translatedSidebarLabel} ${t('sidebar')}`}
      >
        <Icon name={collapsed ? 'panel-left-open' : 'panel-left-close'} size={17} aria-hidden="true" />
      </button>
      {onMobileClose && (
        <button
          type="button"
          className="workspace-rail__mobile-close"
          onClick={onMobileClose}
          aria-label={`${t('Close panel')}: ${translatedSidebarLabel}`}
          title={t('Close panel')}
        >
          <Icon name="x" size={17} aria-hidden="true" />
        </button>
      )}
    </div>
  );
};

export default WorkspaceRailHeader;
