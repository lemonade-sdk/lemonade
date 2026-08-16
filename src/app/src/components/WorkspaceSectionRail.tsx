import React from 'react';
import { Icon, type IconName } from './Icon';
import WorkspaceMobileMenuButton from './WorkspaceMobileMenuButton';
import WorkspaceRailHeader from './WorkspaceRailHeader';
import { useWorkspaceMobileRail } from '../hooks/useWorkspaceMobileRail';
import { useI18n } from '../i18n';

export interface WorkspaceSectionDefinition<Section extends string> {
  id: Section;
  labelKey: string;
  descriptionKey: string;
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
  const { t } = useI18n('navigation');

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
          {sections.map(section => {
            const label = t(section.labelKey);
            const description = t(section.descriptionKey);
            return (
            <button
              key={section.id}
              type="button"
              className={activeSection === section.id ? 'is-active' : ''}
              aria-current={activeSection === section.id ? 'page' : undefined}
              aria-label={label}
              // aria-label keeps the name short and stable across the collapsed
              // state; the description reaches assistive tech as a description
              // rather than being swallowed by the label override.
              aria-describedby={collapsed ? undefined : `${panelId}-${section.id}-description`}
              title={collapsed ? label : undefined}
              onClick={() => {
                onSectionChange(section.id);
                mobileRail.close();
              }}
            >
              <span className="workspace-nav__icon"><Icon name={section.icon} size={15} aria-hidden="true" /></span>
              <span className="workspace-nav__copy">
                <strong>{label}</strong>
                <small id={`${panelId}-${section.id}-description`}>{description}</small>
              </span>
              <Icon className="workspace-nav__chevron" name="chevron-right" size={13} aria-hidden="true" />
            </button>
            );
          })}
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
