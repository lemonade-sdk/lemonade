import React, { useState } from 'react';
import { type AccountSession } from '../features/accounts/accountStore';
import Dashboard from './Dashboard';
import { Icon } from './Icon';
import InspectView from './InspectView';
import LogViewer from './LogViewer';
import WorkspaceMobileMenuButton from './WorkspaceMobileMenuButton';
import WorkspaceRailHeader from './WorkspaceRailHeader';
import { useWorkspaceMobileRail } from '../hooks/useWorkspaceMobileRail';

export type MonitorSection = 'overview' | 'requests' | 'logs';

interface MonitorViewProps {
  accountSession: AccountSession;
  activeSection: MonitorSection;
  isActive: boolean;
  onSectionChange: (section: MonitorSection) => void;
}

const sections: Array<{
  id: MonitorSection;
  label: string;
  description: string;
  icon: Parameters<typeof Icon>[0]['name'];
}> = [
  { id: 'overview', label: 'Overview', description: 'Health and throughput', icon: 'gauge' },
  { id: 'requests', label: 'Requests', description: 'Traces, replay and tuning', icon: 'search-check' },
  { id: 'logs', label: 'Logs', description: 'Live server output', icon: 'logs' },
];

export default function MonitorView({
  accountSession,
  activeSection,
  isActive,
  onSectionChange,
}: MonitorViewProps) {
  const [railCollapsed, setRailCollapsed] = useState(false);
  const mobileRail = useWorkspaceMobileRail();

  return (
    <section
      className={`monitor-workspace${railCollapsed ? ' workspace--rail-collapsed' : ''}`}
      data-view="monitor"
    >
      {mobileRail.isOpen && <div className="workspace-mobile-rail-backdrop" onClick={mobileRail.close} aria-hidden="true" />}
      <aside
        ref={mobileRail.panelRef}
        id="monitor-views-panel"
        className={`workspace-rail mobile-context-panel monitor-rail${railCollapsed && !mobileRail.isOpen ? ' is-collapsed' : ''}${mobileRail.isOpen ? ' is-mobile-open' : ''}`}
        aria-label="Monitor navigation"
        role={mobileRail.isOpen ? 'dialog' : undefined}
        aria-modal={mobileRail.isOpen ? true : undefined}
      >
        <WorkspaceRailHeader
          title="Views"
          sidebarLabel="monitor navigation"
          purpose="navigation"
          collapsed={railCollapsed && !mobileRail.isOpen}
          onToggle={() => setRailCollapsed(value => !value)}
          onMobileClose={mobileRail.isOpen ? mobileRail.close : undefined}
        />

        <nav className="workspace-nav monitor-nav" aria-label="Monitor sections">
          {sections.map(({ id, label, description, icon }) => (
            <button
              key={id}
              type="button"
              className={activeSection === id ? 'is-active' : ''}
              aria-current={activeSection === id ? 'page' : undefined}
              aria-label={label}
              title={railCollapsed ? label : undefined}
              onClick={() => { onSectionChange(id); mobileRail.close(); }}
            >
              <span className="workspace-nav__icon"><Icon name={icon} size={15} aria-hidden="true" /></span>
              <span className="workspace-nav__copy">
                <strong>{label}</strong>
                <small>{description}</small>
              </span>
              <Icon className="workspace-nav__chevron" name="chevron-right" size={13} aria-hidden="true" />
            </button>
          ))}
        </nav>
      </aside>

      <WorkspaceMobileMenuButton
        menuLabel="Open monitor views"
        panelId="monitor-views-panel"
        expanded={mobileRail.isOpen}
        onClick={mobileRail.toggle}
        triggerRef={mobileRail.triggerRef}
      />

      <div className="monitor-content">
        <div className="monitor-section" hidden={activeSection !== 'overview'}>
          <Dashboard isActive={isActive && activeSection === 'overview'} />
        </div>
        <div className="monitor-section" hidden={activeSection !== 'requests'}>
          <InspectView accountSession={accountSession} embedded />
        </div>
        <div className="monitor-section" hidden={activeSection !== 'logs'}>
          <LogViewer embedded />
        </div>
      </div>
    </section>
  );
}
