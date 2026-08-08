import React, { useState } from 'react';
import Dashboard from './Dashboard';
import InspectView from './InspectView';
import LogViewer from './LogViewer';
import WorkspaceSectionRail from './WorkspaceSectionRail';
import { WORKSPACE_NAVIGATION, type DashboardSection } from '../features/navigation/workspaceNavigation';

interface MonitorViewProps {
  activeSection: DashboardSection;
  isActive: boolean;
  onSectionChange: (section: DashboardSection) => void;
}

export default function MonitorView({
  activeSection,
  isActive,
  onSectionChange,
}: MonitorViewProps) {
  const [railCollapsed, setRailCollapsed] = useState(false);

  return (
    <section
      className={`monitor-workspace${railCollapsed ? ' workspace--rail-collapsed' : ''}`}
      data-view="dashboard"
    >
      <WorkspaceSectionRail
        sections={WORKSPACE_NAVIGATION.dashboard.sections}
        activeSection={activeSection}
        onSectionChange={onSectionChange}
        collapsed={railCollapsed}
        onCollapsedChange={setRailCollapsed}
        panelId="dashboard-views-panel"
        railLabel="Monitor navigation"
        navigationLabel="Monitor sections"
        railClassName="monitor-rail"
        navClassName="monitor-nav"
        headerTitle="Views"
        sidebarLabel="monitor navigation"
        mobileMenuLabel="Open monitor views"
      />

      <div className="monitor-content">
        <div className="monitor-section" hidden={activeSection !== 'performance'}>
          <Dashboard isActive={isActive && activeSection === 'performance'} />
        </div>
        <div className="monitor-section" hidden={activeSection !== 'telemetry'}>
          <InspectView embedded />
        </div>
        <div className="monitor-section" hidden={activeSection !== 'logs'}>
          <LogViewer embedded />
        </div>
      </div>
    </section>
  );
}
