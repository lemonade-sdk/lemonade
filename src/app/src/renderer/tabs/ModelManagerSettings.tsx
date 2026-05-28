import React from 'react';
import { BooleanSetting } from '../utils/appSettings';

interface ModelManagerSettingsProps {
  modelAutoUpdate: BooleanSetting;
  onBooleanChangeFunc: (value: boolean) => void;
  onResetFunc: () => void;
}

const ModelManagerSettings: React.FC<ModelManagerSettingsProps> = ({
  modelAutoUpdate,
  onBooleanChangeFunc,
  onResetFunc,
}) => {
  return (
    <div className="settings-section-container">
      <div className={`settings-section ${modelAutoUpdate.useDefault ? 'settings-section-default' : ''}`}>
        <div className="settings-label-row">
          <span className="settings-label-text">Model Auto Update</span>
          <button
            type="button"
            className="settings-field-reset"
            onClick={onResetFunc}
            disabled={modelAutoUpdate.useDefault}
          >
            Reset
          </button>
        </div>
        <label className="settings-checkbox-label">
          <input
            type="checkbox"
            checked={modelAutoUpdate.value}
            onChange={(e) => onBooleanChangeFunc(e.target.checked)}
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
