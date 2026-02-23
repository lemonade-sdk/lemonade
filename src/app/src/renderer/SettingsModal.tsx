import React, { useState, useEffect, ReactElement } from 'react';
import {
  AppSettings,
  BASE_SETTING_VALUES,
  NumericSettingKey,
  clampNumericSettingValue,
  createDefaultSettings,
  mergeWithDefaultSettings,
  DEFAULT_TTS_SETTINGS,
} from './utils/appSettings';
import ConnectionSettings from './tabs/ConnectionSettings';
import TTSSettings from './tabs/TTSSettings';
import LLMChatSettings from './tabs/LLMChatSettings';
import Tabs from './Tabs';

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

const tabs = [
  { id: 'connection_settings', label: 'Connection' },
  { id: 'llm_chat_settings', label: 'LLM Chat' },
  { id: 'tts_settings', label: 'TTS' },
  { id: 'help_settings', label: 'Help' }
];

const SettingsModal: React.FC<SettingsModalProps> = ({ isOpen, onClose }) => {
  const [settings, setSettings] = useState<AppSettings>(createDefaultSettings());
  const [isLoading, setIsLoading] = useState(false);
  const [isSaving, setIsSaving] = useState(false);
  const [version, setVersion] = useState<string>('Unknown');

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
    if (window.api?.getVersion) {
      window.api.getVersion().then((v) => {
        if (isMounted && v) {
          setVersion(v);
        }
      }).catch((error) => {
        console.error('Failed to load app version:', error);
      });
    }

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

  const handleTTSSettingChange = (key: string, value: string | boolean) => {
    if(value !== '') {
      setSettings((prev) => ({
        ...prev,
        tts: {
          ...prev.tts,
          [key]: { value, useDefault: false }
        }
      }));
    }
  };

  const handleTextInputChange = (key: string, value: string) => {
    setSettings((prev) => ({
      ...prev,
      [key]: {
        value,
        useDefault: false,
      },
    }));
  };

  const handleResetField = (key: NumericSettingKey | 'enableThinking' | 'collapseThinkingByDefault' | 'baseURL' | 'apiKey' | 'model' | 'userVoice' | 'assistantVoice') => {
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

      if (key === 'model') {
        return {
          ...prev,
          tts: {
            ...prev.tts,
            model: DEFAULT_TTS_SETTINGS.model
          },
        };
      }

      if (key === 'userVoice') {
        return {
          ...prev,
          tts: {
            ...prev.tts,
            userVoice: DEFAULT_TTS_SETTINGS.userVoice
          },
        };
      }

      if (key === 'assistantVoice') {
        return {
          ...prev,
          tts: {
            ...prev.tts,
            assistantVoice: DEFAULT_TTS_SETTINGS.assistantVoice
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

  const openExternal = (url: string) => {
    if (window.api?.openExternal) {
      window.api.openExternal(url);
      return;
    }
    window.open(url, '_blank', 'noopener,noreferrer');
  };

  const renderHelpSettings = () => (
    <div className="settings-section-container">
      <div className="settings-section">
        <div className="settings-label-row">
          <label className="settings-label">
            <span className="settings-label-text">Resources</span>
            <span className="settings-description">Documentation, release notes, and project home.</span>
          </label>
        </div>
        <div className="settings-help-actions">
          <button type="button" className="settings-link-button" onClick={() => openExternal('https://lemonade-server.ai/docs/')}>
            Open Documentation
          </button>
          <button type="button" className="settings-link-button" onClick={() => openExternal('https://github.com/lemonade-sdk/lemonade/releases')}>
            Open Release Notes
          </button>
          <button type="button" className="settings-link-button" onClick={() => openExternal('https://github.com/lemonade-sdk/lemonade')}>
            Open GitHub Repo
          </button>
        </div>
      </div>
      <div className="settings-section">
        <div className="settings-label-row">
          <label className="settings-label">
            <span className="settings-label-text">About</span>
            <span className="settings-description">Lemonade local AI control center.</span>
          </label>
        </div>
        <div className="settings-help-about">
          <div className="settings-help-about-row">
            <span className="settings-help-about-key">Version</span>
            <span className="settings-help-about-value">{version}</span>
          </div>
        </div>
      </div>
      <div className="settings-section">
        <div className="settings-label-row">
          <label className="settings-label">
            <span className="settings-label-text">Keyboard Shortcuts</span>
            <span className="settings-description">Quick access keys for the main workspace.</span>
          </label>
        </div>
        <div className="settings-help-shortcuts">
          <div className="settings-help-shortcut-row"><span>Toggle Left Panel</span><code>Ctrl+Shift+M</code></div>
          <div className="settings-help-shortcut-row"><span>Focus Models</span><code>Ctrl+Shift+P</code></div>
          <div className="settings-help-shortcut-row"><span>Toggle Chat</span><code>Ctrl+Shift+H</code></div>
          <div className="settings-help-shortcut-row"><span>Toggle Logs</span><code>Ctrl+Shift+L</code></div>
        </div>
      </div>
    </div>
  );

  const getSettingContext = (id: string): ReactElement => {
    switch(id) {
      case 'connection_settings':
        return <ConnectionSettings
          settings={settings}
          onValueChangeFunc={handleTextInputChange}
          onResetFunc={handleResetField}
        />;
      case 'llm_chat_settings':
        return <LLMChatSettings
          settings={settings}
          numericSettingsConfig={numericSettingsConfig}
          onBooleanChangeFunc={handleBooleanChange}
          onNumericChangeFunc={handleNumericChange}
          onResetFunc={handleResetField}
        />;
      case 'tts_settings':
        return <TTSSettings
        settings={settings}
        onValueChangeFunc={handleTTSSettingChange}
        onResetFunc={handleResetField}
        />;
      case 'help_settings':
        return renderHelpSettings();
      default:
        return <div></div>;
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
            <div className="settings-tabs-wrapper">
              <Tabs>
                <Tabs.Labels items={tabs.map(({ id, label }) => ({ id, label }))} />
                <Tabs.Contents items={tabs.map(({ id }) => ({id, content: getSettingContext(id)}))} />
              </Tabs>
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
