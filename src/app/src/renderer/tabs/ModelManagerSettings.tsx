import React from 'react';
import { AppSettings } from '../utils/appSettings';

interface ModelManagerSettingsProps {
  settings: AppSettings;
  onBooleanChangeFunc: (key: string | any, value: boolean) => void;
  onResetFunc: (key: any) => void;
}

const ModelManagerSettings: React.FC<ModelManagerSettingsProps> = ({
  settings,
  onBooleanChangeFunc,
  onResetFunc,
}) => {
  return (
    <div className="settings-section-container">
      <div className={`settings-section ${settings.modelAutoUpdate.useDefault ? 'settings-section-default' : ''}`}>
        <div className="settings-label-row">
          <span className="settings-label-text">Model Auto Update</span>
          <button
            type="button"
            className="settings-field-reset"
            onClick={() => onResetFunc('modelAutoUpdate')}
            disabled={settings.modelAutoUpdate.useDefault}
          >
            Reset
          </button>
        </div>
        <label className="settings-checkbox-label">
          <input
            type="checkbox"
            checked={settings.modelAutoUpdate.value}
            onChange={(e) => onBooleanChangeFunc('modelAutoUpdate', e.target.checked)}
            className="settings-checkbox"
          />
          <div className="settings-checkbox-content">
            <span className="settings-description">
              When enabled, newer model artifacts on Hugging Face are automatically loaded once available.
            </span>
          </div>
        </label>
      </div>
    </div>
  );
};

export default ModelManagerSettings;
