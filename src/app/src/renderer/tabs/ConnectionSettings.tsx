import React from 'react';
import { AppSettings } from '../utils/appSettings';

interface  ConnectionSettingsProps {
  settings: AppSettings,
  onValueChangeFunc: (key: string, value: string) => void;
  onResetFunc: (key: any) => void
}

const ConnectionSettings: React.FC<ConnectionSettingsProps> = ({settings, onValueChangeFunc, onResetFunc}) => {
  return (
    <div className="settings-section-container">
      <div className={`settings-section ${settings.baseURL.useDefault ? "settings-section-default" : ""}`}>
        <div className="settings-label-row">
          <label className="settings-label">
            <span className="settings-label-text">Base URL</span>
            <span className="settings-description">Connect the app to a server at the specified URL.</span>
          </label>
          <button type="button" className="settings-field-reset" onClick={() => onResetFunc('baseURL')} disabled={settings.baseURL.useDefault}>
            Reset
          </button>
        </div>
        <input type="text" value={settings["baseURL"].value} placeholder="http://localhost:8000/" onChange={(e) => onValueChangeFunc('baseURL', e.target.value)} className="settings-text-input"/>
      </div>
      <div className={`settings-section ${settings.apiKey.useDefault ? "settings-section-default" : ""}`}>
        <div className="settings-label-row">
          <label className="settings-label">
            <span className="settings-label-text">API Key</span>
            <span className="settings-description">If present, API Key will be required to execute any request.</span>
          </label>
          <button type="button" className="settings-field-reset" onClick={() => onResetFunc('apiKey')} disabled={settings.apiKey.useDefault}>
            Reset
          </button>
        </div>
        <input type="text" value={settings['apiKey'].value} onChange={(e) => onValueChangeFunc('apiKey', e.target.value)} className="settings-text-input"/>
      </div>
    </div>
  );
}

export default ConnectionSettings;
