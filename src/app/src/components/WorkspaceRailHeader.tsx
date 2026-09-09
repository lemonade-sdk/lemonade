import React from 'react';
import { Icon } from './Icon';
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
}) => (
  <div className={`workspace-rail__header workspace-rail__header--${purpose}`}>
    <span className="workspace-rail__context" aria-hidden="true">
      <Icon name={icon ?? (purpose === 'filter' ? 'funnel' : purpose === 'history' ? 'clock' : 'layers')} size={12} />
      <strong className="workspace-rail__title">{title}</strong>
    </span>
    <button
      type="button"
      className="workspace-rail__toggle"
      onClick={onToggle}
      aria-expanded={!collapsed}
      aria-label={`${collapsed ? 'Expand' : 'Collapse'} ${sidebarLabel} sidebar`}
      title={`${collapsed ? 'Expand' : 'Collapse'} ${sidebarLabel} sidebar`}
    >
      <Icon name={collapsed ? 'panel-left-open' : 'panel-left-close'} size={17} aria-hidden="true" />
    </button>
    {onMobileClose && (
      <button
        type="button"
        className="workspace-rail__mobile-close"
        onClick={onMobileClose}
        aria-label={`Close ${sidebarLabel} panel`}
        title="Close panel"
      >
        <Icon name="x" size={17} aria-hidden="true" />
      </button>
    )}
  </div>
);

export default WorkspaceRailHeader;
