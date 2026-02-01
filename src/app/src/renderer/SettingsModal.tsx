import React, { useState, useEffect } from 'react';
import {
  AppSettings,
  BASE_SETTING_VALUES,
  NumericSettingKey,
  clampNumericSettingValue,
  createDefaultSettings,
  mergeWithDefaultSettings,
  NUMERIC_SETTING_LIMITS,
} from './utils/appSettings';

interface SettingsModalProps {
  isOpen: boolean;
  onClose: () => void;
}

const numericSettingsConfig: Array<{
  key: NumericSettingKey;
  label: string;
  description: string;
}> = [
  {
    key: 'temperature',
    label: 'Temperature',
    description: 'Controls randomness in responses (0 = deterministic, 2 = very random)',
  },
  {
    key: 'topK',
    label: 'Top K',
    description: 'Limits token selection to the K most likely tokens',
  },
  {
    key: 'topP',
    label: 'Top P',
    description: 'Nucleus sampling - considers tokens within cumulative probability P',
  },
  {
    key: 'repeatPenalty',
    label: 'Repeat Penalty',
    description: 'Penalty for repeating tokens (1 = no penalty, >1 = less repetition)',
  },
];

const SettingsModal: React.FC<SettingsModalProps> = ({ isOpen, onClose }) => {
  const [settings, setSettings] = useState<AppSettings>(createDefaultSettings());
  const [isLoading, setIsLoading] = useState(false);
  const [isSaving, setIsSaving] = useState(false);

  useEffect(() => {
    if (!isOpen) {
      return;
    }

    let isMounted = true;

    const fetchSettings = async () => {
      if (isMounted) {
        setIsLoading(true);
      }

      try {
        if (!window.api?.getSettings) {
          if (isMounted) {
            setSettings(createDefaultSettings());
          }
          return;
        }

        const stored = await window.api.getSettings();
        if (isMounted) {
          setSettings(mergeWithDefaultSettings(stored));
        }
      } catch (error) {
        console.error('Failed to load settings:', error);
        if (isMounted) {
          setSettings(createDefaultSettings());
        }
      } finally {
        if (isMounted) {
          setIsLoading(false);
        }
      }
    };

    fetchSettings();

    return () => {
      isMounted = false;
    };
  }, [isOpen]);

  const handleOverlayClick = (e: React.MouseEvent) => {
    if (e.target === e.currentTarget) {
      onClose();
    }
  };

  const handleNumericChange = (key: NumericSettingKey, rawValue: number) => {
    setSettings((prev) => ({
      ...prev,
      [key]: {
        value: clampNumericSettingValue(key, rawValue),
        useDefault: false,
      },
    }));
  };

  const handleBooleanChange = (key: 'enableThinking' | 'collapseThinkingByDefault', value: boolean) => {
    setSettings((prev) => ({
      ...prev,
      [key]: {
        value,
        useDefault: false,
      },
    }));
  };

  const handleTextInputChange = (key: 'baseURL' | 'apiKey', value: string) => {
    setSettings((prev) => ({
      ...prev,
      [key]: {
        value,
        useDefault: false,
      },
    }));
  };

  const handleResetField = (key: NumericSettingKey | 'enableThinking' | 'collapseThinkingByDefault' | 'baseURL' | 'apiKey') => {
    setSettings((prev) => {
      if (key === 'enableThinking') {
        return {
          ...prev,
          enableThinking: {
            value: BASE_SETTING_VALUES.enableThinking,
            useDefault: true,
          },
        };
      }

      if (key === 'collapseThinkingByDefault') {
        return {
          ...prev,
          collapseThinkingByDefault: {
            value: BASE_SETTING_VALUES.collapseThinkingByDefault,
            useDefault: true,
          },
        };
      }

      return {
        ...prev,
        [key]: {
          value: BASE_SETTING_VALUES[key],
          useDefault: true,
        },
      };
    });
  };

  const handleReset = () => {
    setSettings(createDefaultSettings());
  };

  const handleSave = async () => {
    if (!window.api?.saveSettings) {
      onClose();
      return;
    }

    setIsSaving(true);
    try {
      const saved = await window.api.saveSettings(settings);
      setSettings(mergeWithDefaultSettings(saved));
      onClose();
    } catch (error) {
      console.error('Failed to save settings:', error);
      alert('Failed to save settings. Please try again.');
    } finally {
      setIsSaving(false);
    }
  };

  if (!isOpen) return null;

  return (
    <div className="settings-overlay" onClick={handleOverlayClick}>
      <div className="settings-modal">
        <div className="settings-header">
          <h2>Settings</h2>
          <button className="settings-close-button" onClick={onClose} title="Close">
            <svg width="14" height="14" viewBox="0 0 14 14">
              <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/>
            </svg>
          </button>
        </div>

        {isLoading ? (
          <div className="settings-loading">Loading settings…</div>
        ) : (
          <div className="settings-content">
            <div className="settings-category-header">
              <h3>Connection</h3>
            </div>
            <div className={`settings-section ${settings.baseURL.useDefault ? "settings-section-default" : ""}`}>
              <div className="settings-label-row">
                <label className="settings-label">
                  <span className="settings-label-text">Base URL</span>
                  <span className="settings-description">Connect the app to a server at the specified URL.</span>
                </label>
                <button type="button" className="settings-field-reset" onClick={() => handleResetField('baseURL')} disabled={settings.baseURL.useDefault}>
                  Reset
                </button>
              </div>
              <input type="text" value={settings["baseURL"].value} placeholder="http://localhost:8000/" onChange={(e) => handleTextInputChange('baseURL', e.target.value)} className="settings-text-input"/>
            </div>
            <div className={`settings-section ${settings.apiKey.useDefault ? "settings-section-default" : ""}`}>
              <div className="settings-label-row">
                <label className="settings-label">
                  <span className="settings-label-text">API Key</span>
                  <span className="settings-description">If present, API Key will be required to execute any request.</span>
                </label>
                <button type="button" className="settings-field-reset" onClick={() => handleResetField('apiKey')} disabled={settings.apiKey.useDefault}>
                  Reset
                </button>
              </div>
              <input type="text" value={settings['apiKey'].value} onChange={(e) => handleTextInputChange('apiKey', e.target.value)} className="settings-text-input"/>
            </div>
            <div className="settings-category-header">
              <h3>LLM Chat</h3>
            </div>

            {numericSettingsConfig.map(({ key, label, description }) => {
              const limits = NUMERIC_SETTING_LIMITS[key];
              const isDefault = settings[key].useDefault;

              return (
                <div
                  key={key}
                  className={`settings-section ${isDefault ? 'settings-section-default' : ''}`}
                >
                  <div className="settings-label-row">
                    <label className="settings-label">
                      <span className="settings-label-text">{label}</span>
                      <span className="settings-description">{description}</span>
                    </label>
                    <button
                      type="button"
                      className="settings-field-reset"
                      onClick={() => handleResetField(key)}
                      disabled={isDefault}
                    >
                      Reset
                    </button>
                  </div>
                  <div className="settings-input-group">
                    <input
                      type="range"
                      min={limits.min}
                      max={limits.max}
                      step={limits.step}
                      value={settings[key].value}
                      onChange={(e) => handleNumericChange(key, parseFloat(e.target.value))}
                      className={`settings-slider ${isDefault ? 'slider-auto' : ''}`}
                    />
                    <input
                      type="text"
                      value={isDefault ? 'auto' : settings[key].value}
                      onChange={(e) => {
                        if (e.target.value === 'auto' || e.target.value === '') {
                          return;
                        }
                        const parsed = parseFloat(e.target.value);
                        if (Number.isNaN(parsed)) {
                          return;
                        }
                        handleNumericChange(key, parsed);
                      }}
                      onFocus={(e) => {
                        if (isDefault) {
                          handleNumericChange(key, settings[key].value);
                          // Select all text after a brief delay to allow the value to update
                          setTimeout(() => e.target.select(), 0);
                        }
                      }}
                      className="settings-number-input"
                      placeholder="auto"
                    />
                  </div>
                </div>
              );
            })}

            <div
              className={`settings-section ${
                settings.enableThinking.useDefault ? 'settings-section-default' : ''
              }`}
            >
              <div className="settings-label-row">
                <span className="settings-label-text">Enable Thinking</span>
                <button
                  type="button"
                  className="settings-field-reset"
                  onClick={() => handleResetField('enableThinking')}
                  disabled={settings.enableThinking.useDefault}
                >
                  Reset
                </button>
              </div>
              <label className="settings-checkbox-label">
                <input
                  type="checkbox"
                  checked={settings.enableThinking.value}
                  onChange={(e) => handleBooleanChange('enableThinking', e.target.checked)}
                  className="settings-checkbox"
                />
                <div className="settings-checkbox-content">
                  <span className="settings-description">
                    Determines whether hybrid reasoning models, such as Qwen3, will use thinking.
                  </span>
                </div>
              </label>
            </div>

            <div
              className={`settings-section ${
                settings.collapseThinkingByDefault.useDefault ? 'settings-section-default' : ''
              }`}
            >
              <div className="settings-label-row">
                <span className="settings-label-text">Collapse Thinking by Default</span>
                <button
                  type="button"
                  className="settings-field-reset"
                  onClick={() => handleResetField('collapseThinkingByDefault')}
                  disabled={settings.collapseThinkingByDefault.useDefault}
                >
                  Reset
                </button>
              </div>
              <label className="settings-checkbox-label">
                <input
                  type="checkbox"
                  checked={settings.collapseThinkingByDefault.value}
                  onChange={(e) => handleBooleanChange('collapseThinkingByDefault', e.target.checked)}
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
        )}

        <div className="settings-footer">
          <button
            className="settings-reset-button"
            onClick={handleReset}
            disabled={isSaving || isLoading}
          >
            Reset to Defaults
          </button>
          <button
            className="settings-save-button"
            onClick={handleSave}
            disabled={isSaving || isLoading}
          >
            {isSaving ? 'Saving…' : 'Save'}
          </button>
        </div>
      </div>
    </div>
  );
};

export default SettingsModal;
