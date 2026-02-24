import React, { ReactElement, useEffect, useState } from 'react';
import { ChevronRight } from 'lucide-react';
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

interface SettingsPanelProps {
  isVisible: boolean;
  searchQuery?: string;
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

const SettingsPanel: React.FC<SettingsPanelProps> = ({ isVisible, searchQuery = '' }) => {
  const [settings, setSettings] = useState<AppSettings>(createDefaultSettings());
  const [isLoading, setIsLoading] = useState(false);
  const [isSaving, setIsSaving] = useState(false);
  const [expandedCategories, setExpandedCategories] = useState<Set<string>>(
    new Set(['connection_settings', 'llm_chat_settings', 'tts_settings'])
  );

  useEffect(() => {
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
  }, []);

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
    if (value !== '') {
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
      return;
    }

    setIsSaving(true);
    try {
      const saved = await window.api.saveSettings(settings);
      setSettings(mergeWithDefaultSettings(saved));
    } catch (error) {
      console.error('Failed to save settings:', error);
      alert('Failed to save settings. Please try again.');
    } finally {
      setIsSaving(false);
    }
  };

  const toggleCategory = (id: string) => {
    setExpandedCategories((previous) => {
      const next = new Set(previous);
      if (next.has(id)) {
        next.delete(id);
      } else {
        next.add(id);
      }
      return next;
    });
  };

  const normalizedSearchQuery = searchQuery.trim().toLowerCase();
  const settingCategories: Array<{
    id: string;
    label: string;
    keywords: string[];
    settingCount: number;
  }> = [
    {
      id: 'connection_settings',
      label: 'Connection',
      keywords: [
        'connection', 'base url', 'api key', 'server', 'endpoint', 'authentication', 'request'
      ],
      settingCount: 2,
    },
    {
      id: 'llm_chat_settings',
      label: 'LLM',
      keywords: [
        'llm', 'chat', 'temperature', 'top k', 'top p', 'repeat penalty', 'thinking', 'collapse thinking'
      ],
      settingCount: 6,
    },
    {
      id: 'tts_settings',
      label: 'Speech',
      keywords: [
        'speech', 'tts', 'text to speech', 'voice', 'model', 'user voice', 'assistant voice', 'enable tts'
      ],
      settingCount: 5,
    },
  ];
  const visibleCategories = settingCategories.filter((category) => {
    if (!normalizedSearchQuery) {
      return true;
    }
    if (category.label.toLowerCase().includes(normalizedSearchQuery)) {
      return true;
    }
    return category.keywords.some((keyword) => keyword.includes(normalizedSearchQuery));
  });

  const getSettingContext = (id: string): ReactElement => {
    switch (id) {
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
      default:
        return <div></div>;
    }
  };

  if (!isVisible) return null;

  return (
    <div className="settings-panel">
      {isLoading ? (
        <div className="settings-loading">Loading settings...</div>
      ) : (
        <div className="settings-content">
          <div className="settings-categories">
            {visibleCategories.map((category) => {
              const isExpanded = normalizedSearchQuery ? true : expandedCategories.has(category.id);
              return (
                <div key={category.id} className="model-category">
                  <div className="model-category-header" onClick={() => toggleCategory(category.id)}>
                    <span className={`category-chevron ${isExpanded ? 'expanded' : ''}`}>
                      <ChevronRight size={11} strokeWidth={2.1} />
                    </span>
                    <span className="category-label">{category.label}</span>
                    <span className="category-count">({category.settingCount})</span>
                  </div>
                  {isExpanded && (
                    <div className="settings-category-content">
                      {getSettingContext(category.id)}
                    </div>
                  )}
                </div>
              );
            })}
            {!visibleCategories.length && (
              <div className="left-panel-empty-state">No settings match your search.</div>
            )}
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
          {isSaving ? 'Saving...' : 'Save'}
        </button>
      </div>
    </div>
  );
};

export default SettingsPanel;
