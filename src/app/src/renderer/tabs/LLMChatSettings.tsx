import React from 'react';
import { AppSettings, NumericSettingKey } from '../utils/appSettings';
import NumericSetting from '../NumericSetting';

interface NumericSettingConfig {
  key: NumericSettingKey;
  label: string;
  description: string
}

interface  LLMChatSettingsProps {
  settings: AppSettings,
  numericSettingsConfig: NumericSettingConfig[];
  onBooleanChangeFunc: (key: string | any, value: boolean) => void;
  onNumericChangeFunc: (key: NumericSettingKey, rawValue: number) => void;
  onResetFunc: (key: any) => void
}

const LLMChatSettings: React.FC<LLMChatSettingsProps> = ({settings, numericSettingsConfig, onBooleanChangeFunc, onNumericChangeFunc, onResetFunc}) => {
  return (
    <div>
      { numericSettingsConfig.map((config: NumericSettingConfig) => {
          return <NumericSetting
          settings={settings}
          key={config.key}
          numKey={config.key}
          label={config.label}
          description={config.description}
          onChangeFunc={onNumericChangeFunc}
          onResetFunc={onResetFunc}
          />
        })
      }
      <div className="settings-section-container">
        <div className={`settings-section ${settings.enableThinking.useDefault ? 'settings-section-default' : ''}`}>
          <div className="settings-label-row">
            <span className="settings-label-text">Enable Thinking</span>
            <button type="button" className="settings-field-reset" onClick={() => onResetFunc('enableThinking')} disabled={settings.enableThinking.useDefault}>
              Reset
            </button>
          </div>
          <label className="settings-checkbox-label">
            <input
              type="checkbox"
              checked={settings.enableThinking.value}
              onChange={(e) => onBooleanChangeFunc('enableThinking', e.target.checked)}
              className="settings-checkbox"
            />
            <div className="settings-checkbox-content">
              <span className="settings-description">Determines whether hybrid reasoning models, such as Qwen3, will use thinking.</span>
            </div>
          </label>
        </div>
        <div className={`settings-section ${settings.collapseThinkingByDefault.useDefault ? 'settings-section-default' : ''}`}>
          <div className="settings-label-row">
            <span className="settings-label-text">Collapse Thinking by Default</span>
            <button type="button" className="settings-field-reset" onClick={() => onResetFunc('collapseThinkingByDefault')} disabled={settings.collapseThinkingByDefault.useDefault}>
              Reset
            </button>
          </div>
          <label className="settings-checkbox-label">
            <input
              type="checkbox"
              checked={settings.collapseThinkingByDefault.value}
              onChange={(e) => onBooleanChangeFunc('collapseThinkingByDefault', e.target.checked)}
              className="settings-checkbox"
            />
            <div className="settings-checkbox-content">
              <span className="settings-description">
                When enabled, thinking sections will be collapsed by default instead of automatically expanded.
              </span>
            </div>
          </label>
        </div>
      </div>
    </div>
  );
}

export default LLMChatSettings;
