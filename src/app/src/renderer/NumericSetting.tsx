import React from 'react';
import { AppSettings, NUMERIC_SETTING_LIMITS, NumericSettingKey } from './utils/appSettings';

interface  NumericSettingProps {
  settings: AppSettings;
  numKey: NumericSettingKey;
  label: string;
  description: string;
  onChangeFunc: (key: NumericSettingKey, rawValue: number) => void;
  onResetFunc: (key: any) => void
}

const NumericSetting: React.FC<NumericSettingProps> = ({settings, numKey, label, description, onChangeFunc, onResetFunc}) => {
  const limits = NUMERIC_SETTING_LIMITS[numKey];
  const isDefault = settings[numKey].useDefault;

  return (
    <div key={numKey} className={`settings-section ${isDefault ? 'settings-section-default' : ''}`}>
      <div className="settings-label-row">
        <label className="settings-label">
          <span className="settings-label-text">{label}</span>
          <span className="settings-description">{description}</span>
        </label>
        <button type="button" className="settings-field-reset" onClick={() => onResetFunc(numKey)} disabled={isDefault}>
          Reset
        </button>
      </div>
      <div className="settings-input-group">
        <input type="range"
          min={limits.min}
          max={limits.max}
          step={limits.step}
          value={settings[numKey].value}
          onChange={(e) => onChangeFunc(numKey, parseFloat(e.target.value))}
          className={`settings-slider ${isDefault ? 'slider-auto' : ''}`}
        />
        <input type="text"
          value={isDefault ? 'auto' : settings[numKey].value}
          onChange={(e) => {
            if (e.target.value === 'auto' || e.target.value === '') {
              return;
            }
            const parsed = parseFloat(e.target.value);
            if (Number.isNaN(parsed)) {
              return;
            }
            onChangeFunc(numKey, parsed);
          }}
          onFocus={(e) => {
            if (isDefault) {
              onChangeFunc(numKey, settings[numKey].value);
              // Select all text after a brief delay to allow the value to update
              setTimeout(() => e.target.select(), 0);
            }
          }}
          className="settings-number-input"
          placeholder="auto"
        />
      </div>
    </div>
  )
}

export default NumericSetting;
