import React from 'react';
import { AppSettings, DEFAULT_TTS_SETTINGS } from '../utils/appSettings';
import Combobox from '../components/Combobox';

interface TTSSettingsProps {
  settings: AppSettings,
  onValueChangeFunc: (key: string | any, value: string | boolean) => void;
  onResetFunc: (key: any) => void
}

export const voiceOptions: string[] = [
  'ash',
  'ballad',
  'coral',
  'echo',
  'fable',
  'nova',
  'onyx',
  'sage',
  'shimmer',
  'verse',
  'marin',
  'cedar',
  'alloy'
];

const TTSSettings: React.FC<TTSSettingsProps> = ({ settings, onValueChangeFunc, onResetFunc }) => {
  const [userVoice, setUserVoice] = React.useState<string>(settings.tts['userVoice'].value);
  const [assistantVoice, setAssistantVoice] = React.useState<string>(settings.tts['assistantVoice'].value);

  const updateUserVoice = (val: string): void => {
    setUserVoice(val);
    onValueChangeFunc('userVoice', val);
  }

  const updateAssistantVoice = (val: string): void => {
    setAssistantVoice(val);
    onValueChangeFunc('assistantVoice', val);
  }

  const resetUserVoice = (): void => {
    onResetFunc('userVoice');
    setUserVoice(DEFAULT_TTS_SETTINGS.userVoice.value);
  }

  const resetAssistantVoice = (): void => {
    onResetFunc('assistantVoice');
    setAssistantVoice(DEFAULT_TTS_SETTINGS.assistantVoice.value);
  }

  return (
    <div className="settings-section-container">
      <div className={`settings-section ${settings.tts.model.useDefault ? "settings-section-default" : ""}`}>
        <div className="settings-label-row">
          <label className="settings-label">
            <span className="settings-label-text">TTS Model</span>
            <span className="settings-description">Use the selected model for TTS conversion.</span>
          </label>
          <button type="button" className="settings-field-reset" onClick={() => onResetFunc('model')} disabled={settings.tts.model.useDefault}>
            Reset
          </button>
        </div>
        <input type="text" value={settings.tts["model"].value} onChange={(e) => onValueChangeFunc('model', e.target.value)} className="settings-text-input" />
      </div>
      <div className="settings-section">
        <div className="settings-label-row">
          <label className="settings-label">
            <span className="settings-label-text">User Voice</span>
            <span className="settings-description">Use the selected voice for TTS conversion of user messages.</span>
          </label>
          <button type="button" className="settings-field-reset" onClick={resetUserVoice} disabled={settings.tts.userVoice.useDefault}>
            Reset
          </button>
        </div>
        <Combobox defaultValue={userVoice} useDefault={settings.tts.userVoice.useDefault} onChangeFunc={updateUserVoice} optionsList={voiceOptions} placeholder='Select a voice...' />
      </div>
      <div className="settings-section">
        <div className="settings-label-row">
          <label className="settings-label">
            <span className="settings-label-text">Assistant Voice</span>
            <span className="settings-description">Use the selected voice for TTS conversion of assistant messages.</span>
          </label>
          <button type="button" className="settings-field-reset" onClick={resetAssistantVoice} disabled={settings.tts.assistantVoice.useDefault}>
            Reset
          </button>
        </div>
        <Combobox defaultValue={assistantVoice} useDefault={settings.tts.assistantVoice.useDefault} onChangeFunc={updateAssistantVoice} optionsList={voiceOptions} placeholder='Select a voice...' />
      </div>
      <div className={`settings-section ${settings.tts.enableTTS.useDefault ? 'settings-section-default' : ''}`}>
        <div className="settings-label-row">
          <span className="settings-label-text">Enable TTS</span>
          <button type="button" className="settings-field-reset" onClick={() => onResetFunc('enableTTS')} disabled={settings.tts.enableTTS.useDefault}>
            Reset
          </button>
        </div>
        <label className="settings-checkbox-label">
          <input
            type="checkbox"
            checked={settings.tts.enableTTS.value}
            onChange={(e) => onValueChangeFunc('enableTTS', e.target.checked)}
            className="settings-checkbox"
          />
          <div className="settings-checkbox-content">
            <span className="settings-description">
              Enables TTS conversion for all messages.
            </span>
          </div>
        </label>
      </div>
      <div className={`settings-section ${settings.tts.enableUserTTS.useDefault ? 'settings-section-default' : ''}`}>
        <div className="settings-label-row">
          <span className="settings-label-text">Enable User TTS</span>
          <button type="button" className="settings-field-reset" onClick={() => onResetFunc('enableUserTTS')} disabled={settings.tts.enableUserTTS.useDefault}>
            Reset
          </button>
        </div>
        <label className="settings-checkbox-label">
          <input
            type="checkbox"
            checked={settings.tts.enableUserTTS.value}
            onChange={(e) => onValueChangeFunc('enableUserTTS', e.target.checked)}
            className="settings-checkbox"
          />
          <div className="settings-checkbox-content">
            <span className="settings-description">
              Enables TTS conversion for user messages.
            </span>
          </div>
        </label>
      </div>
    </div>
  );
}

export default TTSSettings;
