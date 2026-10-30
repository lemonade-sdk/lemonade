import React, { Suspense, useEffect, useRef, useState } from 'react';
import Monitor from './Monitor';
import api from '../api';
import WorkspaceSectionRail from './WorkspaceSectionRail';
import { WORKSPACE_NAVIGATION, type MonitorSection } from '../features/navigation/workspaceNavigation';
import {
  InspectViewPreloaded as InspectView,
  LogViewerPreloaded as LogViewer,
  preloadMonitorSecondarySurfaces,
} from '../interactionPreload';
import { useI18n } from '../i18n';

interface MonitorViewProps {
  activeSection: MonitorSection;
  isActive: boolean;
  onSectionChange: (section: MonitorSection) => void;
}

export async function preloadMonitorSections(): Promise<void> {
  await preloadMonitorSecondarySurfaces();
}

const MonitorSectionFallback = () => {
  const { t } = useI18n();
  return (
    <div className="view-loading" role="status" aria-label={t('Loading monitor section')} aria-live="polite">
      <span className="spinner" aria-hidden="true" />
    </div>
  );
};

export default function MonitorView({
  activeSection,
  isActive,
  onSectionChange,
}: MonitorViewProps) {
  const [railCollapsed, setRailCollapsed] = useState(false);
  const { t } = useI18n();
  const mountedSectionsRef = useRef<Set<MonitorSection>>(new Set([activeSection]));
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
      data-view="monitor"
    >
      <WorkspaceSectionRail
        sections={WORKSPACE_NAVIGATION.monitor.sections}
        activeSection={activeSection}
        onSectionChange={onSectionChange}
        collapsed={railCollapsed}
        onCollapsedChange={setRailCollapsed}
        panelId="monitor-views-panel"
        railLabel={t('Monitor navigation')}
        navigationLabel={t('Monitor sections')}
        railClassName="monitor-rail"
        navClassName="monitor-nav"
        headerTitle={t('Views')}
        sidebarLabel={t('monitor navigation')}
        mobileMenuLabel={t('Open monitor views')}
      />

      <div className="monitor-content">
        {mountedSectionsRef.current.has('performance') && (
          <div className="monitor-section" hidden={activeSection !== 'performance'}>
            <Monitor key={api.baseUrl} isActive={isActive && activeSection === 'performance'} />
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
