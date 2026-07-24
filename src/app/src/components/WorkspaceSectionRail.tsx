import React from 'react';
import { Icon, type IconName } from './Icon';
import WorkspaceMobileMenuButton from './WorkspaceMobileMenuButton';
import WorkspaceRailHeader from './WorkspaceRailHeader';
import { useWorkspaceMobileRail } from '../hooks/useWorkspaceMobileRail';

export interface WorkspaceSectionDefinition<Section extends string> {
  id: Section;
  label: string;
  description: string;
  icon: IconName;
}

interface WorkspaceSectionRailProps<Section extends string> {
  sections: readonly WorkspaceSectionDefinition<Section>[];
  activeSection: Section;
  onSectionChange: (section: Section) => void;
  collapsed: boolean;
  onCollapsedChange: (collapsed: boolean) => void;
  panelId: string;
  railLabel: string;
  navigationLabel: string;
  railClassName?: string;
  navClassName?: string;
  headerTitle: string;
  sidebarLabel: string;
  headerIcon?: IconName;
  mobileMenuLabel: string;
  footer?: React.ReactNode;
}

export default function WorkspaceSectionRail<Section extends string>({
  sections,
  activeSection,
  onSectionChange,
  collapsed,
  onCollapsedChange,
  panelId,
  railLabel,
  navigationLabel,
  railClassName = '',
  navClassName = '',
  headerTitle,
  sidebarLabel,
  headerIcon,
  mobileMenuLabel,
  footer,
}: WorkspaceSectionRailProps<Section>) {
  const mobileRail = useWorkspaceMobileRail();

  return (
    <>
      {mobileRail.isOpen && <div className="workspace-mobile-rail-backdrop" onClick={mobileRail.close} aria-hidden="true" />}
      <aside
        ref={mobileRail.panelRef}
        id={panelId}
        className={`workspace-rail mobile-context-panel${railClassName ? ` ${railClassName}` : ''}${collapsed && !mobileRail.isOpen ? ' is-collapsed' : ''}${mobileRail.isOpen ? ' is-mobile-open' : ''}`}
        aria-label={railLabel}
        role={mobileRail.isOpen ? 'dialog' : undefined}
        aria-modal={mobileRail.isOpen ? true : undefined}
      >
        <WorkspaceRailHeader
          title={headerTitle}
          sidebarLabel={sidebarLabel}
          icon={headerIcon}
          purpose="navigation"
          collapsed={collapsed && !mobileRail.isOpen}
          onToggle={() => onCollapsedChange(!collapsed)}
          onMobileClose={mobileRail.isOpen ? mobileRail.close : undefined}
        />
        <nav className={`workspace-nav${navClassName ? ` ${navClassName}` : ''}`} aria-label={navigationLabel}>
          {sections.map(section => (
            <button
              key={section.id}
              type="button"
              className={activeSection === section.id ? 'is-active' : ''}
              aria-current={activeSection === section.id ? 'page' : undefined}
              aria-label={section.label}
              title={collapsed ? section.label : undefined}
              onClick={() => {
                onSectionChange(section.id);
                mobileRail.close();
              }}
            >
              <span className="workspace-nav__icon"><Icon name={section.icon} size={15} aria-hidden="true" /></span>
              <span className="workspace-nav__copy">
                <strong>{section.label}</strong>
                <small>{section.description}</small>
              </span>
              <Icon className="workspace-nav__chevron" name="chevron-right" size={13} aria-hidden="true" />
            </button>
          ))}
        </nav>
        {footer}
      </aside>
      <WorkspaceMobileMenuButton
        menuLabel={mobileMenuLabel}
        panelId={panelId}
        expanded={mobileRail.isOpen}
        onClick={mobileRail.toggle}
        triggerRef={mobileRail.triggerRef}
      />
    </>
  );
}
