import React, { useState, useEffect, useRef } from 'react';
import { useModels } from '../../hooks/useModels';
import { Modality } from '../../hooks/useInferenceState';
import { useSystem } from '../../hooks/useSystem';
import {
  generationParamDefault,
  generationParams,
  GenerationParamMetadata,
} from '../../utils/generationParams';
import { ModelsData } from '../../utils/modelData';
import { AppSettings } from '../../utils/appSettings';
import { readWavFileAsBase64, WAV_FILE_ACCEPT } from '../../utils/wav';
import { useTTS } from '../../hooks/useTTS';
import { voiceOptions } from '../../tabs/TTSSettings';
import { PLAYING } from '../../AudioButton';
import MarkdownMessage from '../../MarkdownMessage';
import { SendIcon, StopIcon } from '../Icons';
import ModelSelector from '../ModelSelector';
import EmptyState from '../EmptyState';
import TypingIndicator from '../TypingIndicator';
import Combobox from '../Combobox';

type VoiceMode = 'plain' | 'describe' | 'clone';

const speechDefaultsForModel = (
  params: GenerationParamMetadata[],
  declared: object,
): Record<string, number> => {
  const defaults: Record<string, number> = {};
  for (const param of params) {
    const value = generationParamDefault(param, declared);
    if (typeof value === 'number') defaults[param.name] = value;
  }
  return defaults;
};

interface TTSPanelProps {
  isBusy: boolean;
  isPreFlight: boolean;
  isInferring: boolean;
  activeModality: Modality | null;
  runPreFlight: (modality: Modality, options: { modelName: string; modelsData: ModelsData; onError: (msg: string) => void }) => Promise<boolean>;
  reset: () => void;
  showError: (msg: string) => void;
  appSettings: AppSettings | null;
}

const TTSPanel: React.FC<TTSPanelProps> = ({
  isBusy, isPreFlight, isInferring, activeModality,
  runPreFlight, reset, showError, appSettings,
}) => {
  const { selectedModel, modelsData } = useModels();
  const { systemInfo, ensureSystemInfoLoaded } = useSystem();
  const tts = useTTS(appSettings, modelsData);

  interface TTSClip {
    text: string;
    audioUrl: string;
    model: string;
    voice: string;
    referenceWavB64?: string;
    extra?: Record<string, unknown>;
  }

  const [inputValue, setInputValue] = useState('');
  const [ttsMessageHistory, setTTSMessageHistory] = useState<TTSClip[]>([]);
  const [editingIndex, setEditingIndex] = useState<number | null>(null);
  const [editingValue, setEditingValue] = useState('');
  const [voiceMode, setVoiceMode] = useState<VoiceMode>('plain');
  const [voiceDescription, setVoiceDescription] = useState('');
  const [cloneWav, setCloneWav] = useState<{ b64: string; name: string } | null>(null);
  const [speechParams, setSpeechParams] = useState<Record<string, number>>({});

  const inputTextareaRef = useRef<HTMLTextAreaElement>(null);
  const editTextareaRef = useRef<HTMLTextAreaElement>(null);
  const messagesEndRef = useRef<HTMLDivElement>(null);
  const messagesContainerRef = useRef<HTMLDivElement>(null);
  const sampleInputRef = useRef<HTMLInputElement>(null);

  const selectedIsTts = (modelsData?.[selectedModel || '']?.labels || []).includes('tts');
  const ttsModel = (selectedIsTts ? selectedModel : '') || appSettings?.tts.model.value || '';
  const ttsRecipe = modelsData?.[ttsModel]?.recipe || '';
  const ttsParams = React.useMemo(
    () => generationParams(systemInfo, ttsRecipe, 'tts'),
    [systemInfo, ttsRecipe],
  );
  const cloneSampleParam = ttsParams.find(param => (
    param.typeName === 'AUDIO_B64' && param.exclusiveGroup
  ));
  const voiceDesignParam = ttsParams.find(param => (
    param.typeName === 'TEXT'
    && param.exclusiveGroup
    && param.exclusiveGroup === cloneSampleParam?.exclusiveGroup
  ));
  const supportsVoiceClone = Boolean(cloneSampleParam);
  const supportsVoiceDesign = Boolean(
    voiceDesignParam
    && supportsVoiceClone
    && modelsData?.[ttsModel]?.checkpoints?.voicegen,
  );
  const advancedSpeechParams = ttsParams.filter(param => (
    param.group === 'advanced' && (param.typeName === 'NUMBER' || param.typeName === 'INT')
  ));

  const cloneMissing = supportsVoiceClone && voiceMode === 'clone' && !cloneWav;
  const describeMissing = supportsVoiceDesign && voiceMode === 'describe' && !voiceDescription.trim();

  const busy = isBusy;

  const speechDefaultsKey = JSON.stringify(modelsData?.[ttsModel]?.speech_defaults || {});
  const speechParamsKey = JSON.stringify(advancedSpeechParams);
  useEffect(() => {
    void ensureSystemInfoLoaded();
  }, [ensureSystemInfoLoaded]);

  useEffect(() => {
    if ((voiceMode === 'describe' && !supportsVoiceDesign)
        || (voiceMode === 'clone' && !supportsVoiceClone)) {
      setVoiceMode('plain');
    }
  }, [voiceMode, supportsVoiceClone, supportsVoiceDesign]);

  useEffect(() => {
    const declared = modelsData?.[ttsModel]?.speech_defaults || {};
    setSpeechParams(speechDefaultsForModel(advancedSpeechParams, declared));
  }, [ttsModel, speechDefaultsKey, speechParamsKey]);

  const adjustTextareaHeight = (textarea: HTMLTextAreaElement) => {
    textarea.style.height = 'auto';
    const maxHeight = 200;
    const newHeight = Math.min(textarea.scrollHeight, maxHeight);
    textarea.style.height = newHeight + 'px';
    textarea.style.overflowY = textarea.scrollHeight > maxHeight ? 'auto' : 'hidden';
  };

  const handleInputChange = (e: React.ChangeEvent<HTMLTextAreaElement>) => {
    setInputValue(e.target.value);
    adjustTextareaHeight(e.target);
  };

  const handlePickSample = (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (!file) return;
    readWavFileAsBase64(file)
      .then((b64) => setCloneWav({ b64, name: file.name }))
      .catch((err) => showError(err.message));
    e.target.value = '';
  };

  const synthAndRecord = async (
    text: string, model: string, voice: string,
    referenceWavB64?: string, extra?: Record<string, unknown>,
  ): Promise<boolean> => {
    const ready = await runPreFlight('speech', { modelName: model, modelsData, onError: showError });
    if (!ready) return false;
    const audioUrl = await tts.synthesizeSpeech(text, voice, { model, referenceWavB64, extra });
    setTTSMessageHistory(prev => [...prev, { text, audioUrl, model, voice, referenceWavB64, extra }]);
    return true;
  };

  const handleMessageToSpeech = async () => {
    if (!inputValue.trim() || isBusy || cloneMissing || describeMissing) return;
    const text = inputValue;

    try {
      const extra: Record<string, unknown> = { ...speechParams };
      const styleNote = voiceDescription.trim();
      if (supportsVoiceDesign && voiceDesignParam && voiceMode === 'describe') {
        extra[voiceDesignParam.name] = styleNote;
        await synthAndRecord(text, ttsModel, '', undefined, extra);
      } else if (supportsVoiceClone && voiceMode === 'clone') {
        await synthAndRecord(text, ttsModel, styleNote, cloneWav?.b64, extra);
      } else {
        await synthAndRecord(
          text,
          ttsModel,
          supportsVoiceClone ? styleNote : tts.currentVoice,
          undefined,
          extra,
        );
      }
    } catch (error: any) {
      console.error('Failed to process message:', error);
      showError(`Failed to process message: ${error.message || 'Unknown error'}`);
      tts.stopAudio();
    } finally {
      reset();
      setInputValue('');
    }
  };

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      handleMessageToSpeech();
    }
  };

  const handleEditAudioMessage = (index: number, e: React.MouseEvent) => {
    if (isBusy) return;
    e.stopPropagation();
    setEditingIndex(index);
    setEditingValue(ttsMessageHistory[index].text);
  };

  const handleEditInputChange = (e: React.ChangeEvent<HTMLTextAreaElement>) => {
    setEditingValue(e.target.value);
    e.target.style.height = 'auto';
    e.target.style.height = e.target.scrollHeight + 'px';
  };

  const cancelEdit = () => {
    setEditingIndex(null);
    setEditingValue('');
  };

  const submitAudioMessageEdit = async () => {
    if (!editingValue.trim() || editingIndex === null || isBusy) return;
    const index = editingIndex;
    const clip = ttsMessageHistory[index];
    const newText = editingValue;
    setEditingIndex(null);
    setEditingValue('');

    const ready = await runPreFlight('speech', { modelName: clip.model, modelsData, onError: showError });
    if (!ready) return;
    try {
      const audioUrl = await tts.synthesizeSpeech(newText, clip.voice, { model: clip.model, referenceWavB64: clip.referenceWavB64, extra: clip.extra });
      setTTSMessageHistory(prev => prev.map((c, i) => i === index ? { ...c, text: newText, audioUrl } : c));
    } catch (error: any) {
      console.error('Failed to regenerate message:', error);
      showError(`Failed to regenerate message: ${error.message || 'Unknown error'}`);
    } finally {
      reset();
    }
  };

  const handleAudioEditKeyPress = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      submitAudioMessageEdit();
    } else if (e.key === 'Escape') {
      e.preventDefault();
      cancelEdit();
    }
  };

  const handleEditContainerClick = (e: React.MouseEvent) => {
    e.stopPropagation();
  };

  const renderMessageContent = (content: string) => (
    <MarkdownMessage content={content} isComplete={false} />
  );

  return (
    <>
      <div className="chat-messages" ref={messagesContainerRef}>
        {ttsMessageHistory.length === 0 && <EmptyState title="Lemonade Text to Speech" />}
        {ttsMessageHistory.map((clip, index) => (
          <div key={index} className="chat-message user-message tts-clip-message">
            {editingIndex === index ? (
              <div className="edit-message-wrapper" onClick={handleEditContainerClick}>
                <div className="edit-message-content">
                  <textarea
                    ref={editTextareaRef}
                    className="edit-message-input"
                    value={editingValue}
                    onChange={handleEditInputChange}
                    onKeyDown={handleAudioEditKeyPress}
                    autoFocus
                    rows={1}
                  />
                  <div className="edit-message-controls">
                    <button
                      className="edit-send-button"
                      onClick={submitAudioMessageEdit}
                      disabled={!editingValue.trim()}
                      title="Send edited message"
                    >
                      <SendIcon />
                    </button>
                  </div>
                </div>
              </div>
            ) : (
              <>
                <div
                  onClick={(e) => !isBusy && handleEditAudioMessage(index, e)}
                  style={{ cursor: !isBusy ? 'pointer' : 'default' }}
                  title="Click to edit and regenerate"
                >
                  {renderMessageContent(clip.text)}
                </div>
                <audio
                  className="tts-clip-player"
                  src={clip.audioUrl}
                  controls
                  autoPlay={index === ttsMessageHistory.length - 1}
                />
              </>
            )}
          </div>
        ))}

        {isInferring && activeModality === 'speech' && (
          <div className="model-loading-indicator">
            <span className="model-loading-text">
              {supportsVoiceDesign && voiceMode === 'describe'
                ? 'Designing the voice, then converting text to speech...'
                : 'Converting text to speech...'}
            </span>
          </div>
        )}

        {isPreFlight && activeModality === 'speech' && (
          <div className="model-loading-indicator">
            <span className="model-loading-text">Loading tts model...</span>
          </div>
        )}
        <div ref={messagesEndRef} />
      </div>

      <div className="chat-input-container">
        <div className="chat-input-voice-selector">
          {supportsVoiceClone ? (
            <div className="tts-generation-panel">
              <div className="tts-generation-controls">
                <div className="tts-mode-toggle">
                  <button
                    className={`toggle-button${voiceMode === 'plain' ? ' active' : ''}`}
                    onClick={() => setVoiceMode('plain')}
                    disabled={busy}
                  >Plain</button>
                  {supportsVoiceDesign && (
                    <button
                      className={`toggle-button${voiceMode === 'describe' ? ' active' : ''}`}
                      onClick={() => setVoiceMode('describe')}
                      disabled={busy}
                    >Describe</button>
                  )}
                  <button
                    className={`toggle-button${voiceMode === 'clone' ? ' active' : ''}`}
                    onClick={() => setVoiceMode('clone')}
                    disabled={busy}
                  >Clone</button>
                </div>
                {voiceMode !== 'clone' ? (
                  <input
                    className="form-input"
                    value={voiceDescription}
                    onChange={(e) => setVoiceDescription(e.target.value)}
                    placeholder={voiceMode === 'describe'
                      ? 'Describe the voice (e.g. warm low female, British accent)'
                      : 'Optional style instruction (e.g. cheerful, whispering)'}
                    disabled={busy}
                  />
                ) : (
                  <div className="tts-clone-row">
                    <input
                      ref={sampleInputRef}
                      type="file"
                      accept={cloneSampleParam?.accept || WAV_FILE_ACCEPT}
                      onChange={handlePickSample}
                      style={{ display: 'none' }}
                    />
                    <button className="tts-clone-upload" onClick={() => sampleInputRef.current?.click()} disabled={busy}>
                      {cloneWav ? 'Change sample' : 'Upload voice sample'}
                    </button>
                    {cloneWav && (
                      <span className="tts-clone-file">
                        <span className="tts-clone-name" title={cloneWav.name}>{cloneWav.name}</span>
                        <button className="tts-clone-remove" onClick={() => setCloneWav(null)} disabled={busy} title="Remove sample">×</button>
                      </span>
                    )}
                    <input
                      className="form-input"
                      value={voiceDescription}
                      onChange={(e) => setVoiceDescription(e.target.value)}
                      placeholder="Optional style note"
                      disabled={busy}
                    />
                  </div>
                )}
              </div>
            </div>
          ) : (
            <Combobox defaultValue={tts.currentVoice} optionsList={voiceOptions} onChangeFunc={tts.setVoice} position='top' placeholder='Select a voice...'/>
          )}
          {advancedSpeechParams.length > 0 && (
            <details className="tts-speech-params">
              <summary>Generation parameters</summary>
              <div className="tts-speech-params-grid">
                {advancedSpeechParams.map(param => (
                  <label key={param.name} className="tts-speech-param" title={param.help || undefined}>
                    <span>{param.label}</span>
                    <input
                      type="number"
                      min={param.min ?? undefined}
                      max={param.max ?? undefined}
                      step={param.step ?? undefined}
                      value={speechParams[param.name] ?? ''}
                      onChange={(e) => {
                        const raw = e.target.value;
                        setSpeechParams(previous => {
                          const next = { ...previous };
                          if (raw === '') delete next[param.name];
                          else next[param.name] = Number(raw);
                          return next;
                        });
                      }}
                      disabled={busy}
                    />
                  </label>
                ))}
              </div>
            </details>
          )}
        </div>
        <div className="chat-input-wrapper">
          <textarea
            ref={inputTextareaRef}
            className="chat-input"
            value={inputValue}
            onChange={handleInputChange}
            onKeyDown={handleKeyDown}
            placeholder="Type your message..."
            rows={1}
          />
          <div className="chat-controls">
            <div className="chat-controls-left">
              <ModelSelector disabled={busy} filterLabel="tts" effectiveModel={ttsModel} />
            </div>
            {(tts.audioState == PLAYING) ? (
              <button className="chat-stop-button" onClick={tts.stopAudio} title="Stop audio">
                <StopIcon />
              </button>
            ) : isBusy ? (
              <button className="chat-send-button" disabled title="Processing...">
                <TypingIndicator size="small" />
              </button>
            ) : (
              <button className="chat-send-button" onClick={handleMessageToSpeech} disabled={!inputValue.trim() || cloneMissing || describeMissing} title={cloneMissing ? 'Upload a voice sample to clone' : describeMissing ? 'Describe the voice you want' : 'Send'}>
                <SendIcon />
              </button>
            )}
          </div>
        </div>
      </div>
    </>
  );
};

export default TTSPanel;
