import React, { Suspense, useEffect, useRef, useState } from 'react';
import Dashboard from './Dashboard';
import WorkspaceSectionRail from './WorkspaceSectionRail';
import { WORKSPACE_NAVIGATION, type DashboardSection } from '../features/navigation/workspaceNavigation';
import {
  InspectViewPreloaded as InspectView,
  LogViewerPreloaded as LogViewer,
  preloadMonitorSecondarySurfaces,
} from '../interactionPreload';

interface MonitorViewProps {
  activeSection: DashboardSection;
  isActive: boolean;
  onSectionChange: (section: DashboardSection) => void;
}

export async function preloadMonitorSections(): Promise<void> {
  await preloadMonitorSecondarySurfaces();
}

const MonitorSectionFallback = () => (
  <div className="view-loading" role="status" aria-label="Loading monitor section" aria-live="polite">
    <span className="spinner" aria-hidden="true" />
  </div>
);

export default function MonitorView({
  activeSection,
  isActive,
  onSectionChange,
}: MonitorViewProps) {
  const [railCollapsed, setRailCollapsed] = useState(false);
  const mountedSectionsRef = useRef<Set<DashboardSection>>(new Set([activeSection]));
  mountedSectionsRef.current.add(activeSection);

  useEffect(() => {
    if (!isActive) return;
    // Do not wait for a click on Telemetry/Logs. As soon as Monitor itself is
    // active, warm those modules without mounting them. Shared preload promises
    // make this a no-op when the global post-start warm-up already finished.
    void preloadMonitorSections().catch(() => undefined);
  }, [isActive]);

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
        {mountedSectionsRef.current.has('performance') && (
          <div className="monitor-section" hidden={activeSection !== 'performance'}>
            <Dashboard isActive={isActive && activeSection === 'performance'} />
          </div>
        )}
        {mountedSectionsRef.current.has('telemetry') && (
          <div className="monitor-section" hidden={activeSection !== 'telemetry'}>
            <Suspense fallback={<MonitorSectionFallback />}>
              <InspectView embedded />
            </Suspense>
          </div>
        )}
        {mountedSectionsRef.current.has('logs') && (
          <div className="monitor-section" hidden={activeSection !== 'logs'}>
            <Suspense fallback={<MonitorSectionFallback />}>
              <LogViewer embedded />
            </Suspense>
          </div>
        )}
      </div>
    </section>
  );
}
