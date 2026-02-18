import React from 'react';
import { AppSettings } from '../utils/appSettings';

interface  TTSSettingsProps {
  settings: AppSettings,
  onValueChangeFunc: (key: string | any, value: string | boolean) => void;
  onResetFunc: (key: any) => void
}

export const voiceOptions: string[] = [
  '',
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

const TTSSettings: React.FC<TTSSettingsProps> = ({settings, onValueChangeFunc, onResetFunc}) => {
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
      <div className={`settings-section ${settings.tts.userVoice.useDefault ? "settings-section-default" : ""}`}>
        <div className="settings-label-row">
          <label className="settings-label">
            <span className="settings-label-text">User Voice</span>
            <span className="settings-description">Use the selected voice for TTS conversion of user messages.</span>
          </label>
          <button type="button" className="settings-field-reset" onClick={() => onResetFunc('userVoice')} disabled={settings.tts.userVoice.useDefault}>
            Reset
          </button>
        </div>
        <select className="form-input form-select" value={settings.tts['userVoice'].value} onChange={(e) => onValueChangeFunc('userVoice', e.target.value)}>
          {
            voiceOptions.map((voice: string, index: number) => {
              const label = (voice === '') ? 'Select a voice...' : voice;
              return <option key={index} value={voice} disabled={(voice === '')}>{label}</option>;
            })
          }
        </select>
      </div>
      <div className={`settings-section ${settings.tts.assistantVoice.useDefault ? "settings-section-default" : ""}`}>
        <div className="settings-label-row">
          <label className="settings-label">
            <span className="settings-label-text">Assistant Voice</span>
            <span className="settings-description">Use the selected voice for TTS conversion of assistant messages.</span>
          </label>
          <button type="button" className="settings-field-reset" onClick={() => onResetFunc('assistantVoice')} disabled={settings.tts.assistantVoice.useDefault}>
            Reset
          </button>
        </div>
        <select className="form-input form-select" value={settings.tts['assistantVoice'].value} onChange={(e) => onValueChangeFunc('assistantVoice', e.target.value)}>
          {
            voiceOptions.map((voice: string, index: number) => {
              const label = (voice === '') ? 'Select a voice...' : voice;
              return <option key={index} value={voice} disabled={(voice === '')}>{label}</option>;
            })
          }
        </select>
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
