import React from 'react';
import { AppSettings, DEFAULT_TTS_SETTINGS } from '../utils/appSettings';
import Combobox from '../components/Combobox';
import { useModels } from '../hooks/useModels';
import { getTtsVoiceMode } from '../utils/modelData';
import { readWavFileAsBase64, WAV_FILE_ACCEPT } from '../utils/wav';

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
  const { downloadedModels, modelsData } = useModels();
  const [userVoice, setUserVoice] = React.useState<string>(settings.tts['userVoice'].value);
  const [assistantVoice, setAssistantVoice] = React.useState<string>(settings.tts['assistantVoice'].value);
  const userSampleRef = React.useRef<HTMLInputElement>(null);
  const assistantSampleRef = React.useRef<HTMLInputElement>(null);

  const ttsModelOptions = React.useMemo(() => {
    const ids = downloadedModels
      .filter(m => m.info?.labels?.includes('tts') && getTtsVoiceMode(m.info) !== 'design')
      .map(m => m.id);
    const current = settings.tts.model.value;
    return current && !ids.includes(current) ? [current, ...ids] : ids;
  }, [downloadedModels, settings.tts.model.value]);

  const voiceMode = getTtsVoiceMode(modelsData?.[settings.tts.model.value]);

  const [sampleError, setSampleError] = React.useState<string>('');

  const pickSample = (key: 'userVoiceSample' | 'assistantVoiceSample') => (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (!file) return;
    readWavFileAsBase64(file)
      .then((b64) => { setSampleError(''); onValueChangeFunc(key, b64); })
      .catch((err) => setSampleError(err.message));
    e.target.value = '';
  };

  const renderVoiceSample = (
    key: 'userVoiceSample' | 'assistantVoiceSample',
    ref: React.RefObject<HTMLInputElement | null>,
  ) => (
    <div className="tts-clone-row">
      <input ref={ref} type="file" accept={WAV_FILE_ACCEPT} onChange={pickSample(key)} style={{ display: 'none' }} />
      <button type="button" className="tts-clone-upload" onClick={() => ref.current?.click()}>
        {settings.tts[key].value ? 'Change voice sample' : 'Upload voice sample'}
      </button>
      {settings.tts[key].value && (
        <>
          <span className="tts-clone-file">Voice sample loaded</span>
          <button type="button" className="tts-clone-upload" onClick={() => onValueChangeFunc(key, '')}>
            Clear
          </button>
        </>
      )}
      {sampleError && <span className="tts-clone-error">{sampleError}</span>}
    </div>
  );

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
            <span className="settings-description">Default model used to read messages aloud (speaker buttons).</span>
          </label>
          <button type="button" className="settings-field-reset" onClick={() => onResetFunc('model')} disabled={settings.tts.model.useDefault}>
            Reset
          </button>
        </div>
        <Combobox defaultValue={settings.tts.model.value} optionsList={ttsModelOptions} onChangeFunc={(val) => onValueChangeFunc('model', val)} placeholder='Select a TTS model...' />
      </div>
      <div className="settings-section">
        <div className="settings-label-row">
          <label className="settings-label">
            <span className="settings-label-text">User Voice</span>
            <span className="settings-description">
              {voiceMode === 'clone'
                ? 'Voice sample to clone for user messages. Samples (max 10 MB WAV) are stored locally in the app settings; use Clear to remove them.'
                : 'Use the selected voice for TTS conversion of user messages.'}
            </span>
          </label>
          <button type="button" className="settings-field-reset" onClick={resetUserVoice} disabled={settings.tts.userVoice.useDefault}>
            Reset
          </button>
        </div>
        {voiceMode === 'clone'
          ? renderVoiceSample('userVoiceSample', userSampleRef)
          : <Combobox defaultValue={userVoice} useDefault={settings.tts.userVoice.useDefault} onChangeFunc={updateUserVoice} optionsList={voiceOptions} placeholder='Select a voice...' />}
      </div>
      <div className="settings-section">
        <div className="settings-label-row">
          <label className="settings-label">
            <span className="settings-label-text">Assistant Voice</span>
            <span className="settings-description">
              {voiceMode === 'clone'
                ? 'Voice sample to clone for assistant messages. Samples (max 10 MB WAV) are stored locally in the app settings; use Clear to remove them.'
                : 'Use the selected voice for TTS conversion of assistant messages.'}
            </span>
          </label>
          <button type="button" className="settings-field-reset" onClick={resetAssistantVoice} disabled={settings.tts.assistantVoice.useDefault}>
            Reset
          </button>
        </div>
        {voiceMode === 'clone'
          ? renderVoiceSample('assistantVoiceSample', assistantSampleRef)
          : <Combobox defaultValue={assistantVoice} useDefault={settings.tts.assistantVoice.useDefault} onChangeFunc={updateAssistantVoice} optionsList={voiceOptions} placeholder='Select a voice...' />}
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
