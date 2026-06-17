import React from 'react';

export type CenterPanelTab = 'server-logs' | 'request-logs';

interface CenterPanelTabsProps {
  activeTab: CenterPanelTab;
  onTabChange: (tab: CenterPanelTab) => void;
}

const CenterPanelTabs: React.FC<CenterPanelTabsProps> = ({ activeTab, onTabChange }) => {
  return (
    <div className="center-panel-tabs">
      <button
        type="button"
        className={`center-panel-tab ${activeTab === 'server-logs' ? 'active' : ''}`}
        onClick={() => onTabChange('server-logs')}
      >
        Server Logs
      </button>
      <button
        type="button"
        className={`center-panel-tab ${activeTab === 'request-logs' ? 'active' : ''}`}
        onClick={() => onTabChange('request-logs')}
      >
        Request Logs
      </button>
    </div>
  );
};

export default CenterPanelTabs;
