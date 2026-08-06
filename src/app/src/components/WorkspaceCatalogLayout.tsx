import React, { useState } from 'react';
import { Icon, type IconName } from './Icon';
import WorkspaceMobileMenuButton from './WorkspaceMobileMenuButton';
import WorkspaceRailHeader from './WorkspaceRailHeader';
import { useWorkspaceMobileRail } from '../hooks/useWorkspaceMobileRail';

export type CatalogFilterDefinition<Id extends string> = {
  id: Id;
  label: string;
  description?: string;
  icon: IconName;
  count?: number;
};

interface WorkspaceCatalogLayoutProps<Id extends string> {
  view: string;
  className?: string;
  panelId: string;
  railTitle: string;
  railLabel: string;
  sidebarLabel: string;
  mobileMenuLabel: string;
  filters: readonly CatalogFilterDefinition<Id>[];
  activeFilter: Id;
  onFilterChange: (id: Id) => void;
  railFooter?: React.ReactNode;
  header: React.ReactNode;
  preContent?: React.ReactNode;
  overlay?: React.ReactNode;
  children: React.ReactNode;
}

export function WorkspaceCatalogLayout<Id extends string>({
  view,
  className = '',
  panelId,
  railTitle,
  railLabel,
  sidebarLabel,
  mobileMenuLabel,
  filters,
  activeFilter,
  onFilterChange,
  railFooter,
  header,
  preContent,
  overlay,
  children,
}: WorkspaceCatalogLayoutProps<Id>) {
  const mobileRail = useWorkspaceMobileRail();
  const [railCollapsed, setRailCollapsed] = useState(false);

  return (
    <section
      className={`workspace-catalog-layout${railCollapsed ? ' workspace--rail-collapsed' : ''}${className ? ` ${className}` : ''}`}
      data-view={view}
    >
      {mobileRail.isOpen && <div className="workspace-mobile-rail-backdrop" onClick={mobileRail.close} aria-hidden="true" />}
      <aside
        ref={mobileRail.panelRef}
        id={panelId}
        className={`workspace-rail mobile-context-panel${railCollapsed && !mobileRail.isOpen ? ' is-collapsed' : ''}${mobileRail.isOpen ? ' is-mobile-open' : ''}`}
        aria-label={railLabel}
        role={mobileRail.isOpen ? 'dialog' : undefined}
        aria-modal={mobileRail.isOpen ? true : undefined}
      >
        <WorkspaceRailHeader
          title={railTitle}
          sidebarLabel={sidebarLabel}
          purpose="filter"
          collapsed={railCollapsed && !mobileRail.isOpen}
          onToggle={() => setRailCollapsed(value => !value)}
          onMobileClose={mobileRail.isOpen ? mobileRail.close : undefined}
        />
        <nav className="workspace-filter-list" aria-label={railLabel}>
          {filters.map(filter => (
            <button
              key={filter.id}
              type="button"
              className={`workspace-filter-list__item${activeFilter === filter.id ? ' is-active' : ''}`}
              aria-current={activeFilter === filter.id ? 'true' : undefined}
              aria-label={filter.label}
              title={filter.description ? `${filter.label} — ${filter.description}` : filter.label}
              onClick={() => { onFilterChange(filter.id); mobileRail.close(); }}
            >
              <Icon className="workspace-filter-list__icon" name={filter.icon} size={14} />
              <span className="workspace-filter-list__label">{filter.label}</span>
              {filter.count !== undefined && <span className="workspace-filter-list__count">{filter.count}</span>}
            </button>
          ))}
        </nav>
        {railFooter && <div className="workspace-rail__footer">{railFooter}</div>}
      </aside>

      <WorkspaceMobileMenuButton
        menuLabel={mobileMenuLabel}
        panelId={panelId}
        expanded={mobileRail.isOpen}
        onClick={mobileRail.toggle}
        triggerRef={mobileRail.triggerRef}
      />

      <div className="workspace-pane workspace-catalog-layout__main">
        {header}
        {preContent}
        <div className="workspace-catalog-layout__content">
          {children}
        </div>
        {overlay}
      </div>
    </section>
  );
}

interface WorkspaceCatalogSectionProps {
  title?: React.ReactNode;
  description?: React.ReactNode;
  aside?: React.ReactNode;
  children: React.ReactNode;
}

export const WorkspaceCatalogSection: React.FC<WorkspaceCatalogSectionProps> = ({ title, description, aside, children }) => (
  <section className="workspace-catalog__section">
    {title && (
      <header className="workspace-catalog__heading">
        <div>
          <h2>{title}</h2>
          {description && <p>{description}</p>}
        </div>
        {aside && <span>{aside}</span>}
      </header>
    )}
    <div className="workspace-catalog__grid">{children}</div>
  </section>
);

export default WorkspaceCatalogLayout;
