import React, { useState, useRef } from 'react';
import { useModels } from '../../hooks/useModels';
import { Modality } from '../../hooks/useInferenceState';
import { ModelsData } from '../../utils/modelData';
import { AppSettings } from '../../utils/appSettings';
import { ensureModelReady, DownloadAbortError } from '../../utils/backendInstaller';
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

const VOICE_DESIGN_PHRASE = 'Hello there. This is a short sample of the voice you described.';

const blobUrlToBase64 = async (url: string): Promise<string> => {
  const blob = await (await fetch(url)).blob();
  return await new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onloadend = () => {
      const s = reader.result as string;
      const comma = s.indexOf(',');
      resolve(comma >= 0 ? s.slice(comma + 1) : s);
    };
    reader.onerror = reject;
    reader.readAsDataURL(blob);
  });
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
  const { selectedModel, modelsData, downloadedModels } = useModels();
  const tts = useTTS(appSettings, modelsData);

  interface TTSClip {
    text: string;
    audioUrl: string;
    model: string;
    voice: string;
    referenceWavB64?: string;
  }

  const [inputValue, setInputValue] = useState('');
  const [ttsMessageHistory, setTTSMessageHistory] = useState<TTSClip[]>([]);
  const [editingIndex, setEditingIndex] = useState<number | null>(null);
  const [editingValue, setEditingValue] = useState('');
  const [mossMode, setMossMode] = useState<'plain' | 'describe' | 'clone'>('plain');
  const [voiceDescription, setVoiceDescription] = useState('');
  const [cloneWav, setCloneWav] = useState<{ b64: string; name: string } | null>(null);
  const [designingVoice, setDesigningVoice] = useState(false);

  const inputTextareaRef = useRef<HTMLTextAreaElement>(null);
  const editTextareaRef = useRef<HTMLTextAreaElement>(null);
  const messagesEndRef = useRef<HTMLDivElement>(null);
  const messagesContainerRef = useRef<HTMLDivElement>(null);
  const sampleInputRef = useRef<HTMLInputElement>(null);

  const selectedIsTts = (modelsData?.[selectedModel || '']?.labels || []).includes('tts');
  const ttsModel = (selectedIsTts ? selectedModel : '') || appSettings?.tts.model.value || '';
  const isOpenMoss = (modelsData?.[ttsModel]?.recipe || '') === 'openmoss';
  const selectedIsVoiceDesign = (modelsData?.[ttsModel]?.labels || []).includes('voice-design');

  const voiceDesignModel = selectedIsVoiceDesign ? ttsModel
    : (downloadedModels.find(m => m.info?.labels?.includes('voice-design'))?.id || '');
  const cloneModel = (isOpenMoss && !selectedIsVoiceDesign) ? ttsModel
    : (downloadedModels.find(m => m.info?.recipe === 'openmoss'
        && !m.info?.labels?.includes('voice-design'))?.id || '');

  const cloneMissing  = isOpenMoss && mossMode === 'clone' && (!cloneWav || !cloneModel);
  const describeMissing = isOpenMoss && mossMode === 'describe' && !voiceDesignModel;

  const busy = isBusy || designingVoice;

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
    text: string, model: string, voice: string, referenceWavB64?: string,
  ): Promise<boolean> => {
    const ready = await runPreFlight('speech', { modelName: model, modelsData, onError: showError });
    if (!ready) return false;
    const audioUrl = await tts.synthesizeSpeech(text, voice, { model, referenceWavB64 });
    setTTSMessageHistory(prev => [...prev, { text, audioUrl, model, voice, referenceWavB64 }]);
    return true;
  };

  const handleMessageToSpeech = async () => {
    if (!inputValue.trim() || isBusy || designingVoice || cloneMissing || describeMissing) return;
    const text = inputValue;

    try {
      if (isOpenMoss && mossMode === 'describe' && cloneModel) {
        let referenceWavB64: string;
        try {
          setDesigningVoice(true);
          await ensureModelReady(voiceDesignModel, modelsData);
          const refUrl = await tts.synthesizeSpeech(VOICE_DESIGN_PHRASE, voiceDescription.trim(), { model: voiceDesignModel });
          referenceWavB64 = await blobUrlToBase64(refUrl);
        } catch (e: any) {
          if (e instanceof DownloadAbortError) return;
          throw e;
        } finally {
          setDesigningVoice(false);
        }
        await synthAndRecord(text, cloneModel, '', referenceWavB64);
      } else if (isOpenMoss && mossMode === 'describe') {
        await synthAndRecord(text, voiceDesignModel, voiceDescription.trim());
      } else if (isOpenMoss && mossMode === 'clone') {
        await synthAndRecord(text, cloneModel, voiceDescription.trim(), cloneWav?.b64);
      } else if (isOpenMoss) {
        await synthAndRecord(text, ttsModel, voiceDescription.trim());
      } else {
        await synthAndRecord(text, ttsModel, tts.currentVoice);
      }
    } catch (error: any) {
      console.error('Failed to process message:', error);
      showError(`Failed to process message: ${error.message || 'Unknown error'}`);
      tts.stopAudio();
    } finally {
      setDesigningVoice(false);
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
      const audioUrl = await tts.synthesizeSpeech(newText, clip.voice, { model: clip.model, referenceWavB64: clip.referenceWavB64 });
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

        {designingVoice && (
          <div className="model-loading-indicator">
            <span className="model-loading-text">Designing voice from description...</span>
          </div>
        )}

        {isInferring && activeModality === 'speech' && (
          <div className="model-loading-indicator">
            <span className="model-loading-text">Converting text to speech...</span>
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
          {isOpenMoss ? (
            <div className="tts-openmoss-controls">
              <div className="tts-mode-toggle">
                <button
                  className={`toggle-button${mossMode === 'plain' ? ' active' : ''}`}
                  onClick={() => setMossMode('plain')}
                  disabled={busy}
                >Plain</button>
                <button
                  className={`toggle-button${mossMode === 'describe' ? ' active' : ''}`}
                  onClick={() => setMossMode('describe')}
                  disabled={busy}
                >Describe</button>
                <button
                  className={`toggle-button${mossMode === 'clone' ? ' active' : ''}`}
                  onClick={() => setMossMode('clone')}
                  disabled={busy}
                >Clone</button>
              </div>
              {mossMode === 'plain' ? (
                <input
                  className="form-input"
                  value={voiceDescription}
                  onChange={(e) => setVoiceDescription(e.target.value)}
                  placeholder="Optional style instruction (e.g. cheerful, whispering)"
                  disabled={busy}
                />
              ) : mossMode === 'describe' ? (
                <input
                  className="form-input"
                  value={voiceDescription}
                  onChange={(e) => setVoiceDescription(e.target.value)}
                  placeholder={describeMissing
                    ? 'Install MOSS-VoiceGen to design a voice from a description'
                    : 'Describe the voice (e.g. warm low female, British accent)'}
                  disabled={busy || describeMissing}
                />
              ) : (
                <div className="tts-clone-row">
                  <input ref={sampleInputRef} type="file" accept={WAV_FILE_ACCEPT} onChange={handlePickSample} style={{ display: 'none' }} />
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
          ) : (
            <Combobox defaultValue={tts.currentVoice} optionsList={voiceOptions} onChangeFunc={tts.setVoice} position='top' placeholder='Select a voice...'/>
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
            ) : (isBusy || designingVoice) ? (
              <button className="chat-send-button" disabled title="Processing...">
                <TypingIndicator size="small" />
              </button>
            ) : (
              <button className="chat-send-button" onClick={handleMessageToSpeech} disabled={!inputValue.trim() || cloneMissing || describeMissing} title={cloneMissing ? 'Upload a voice sample to clone' : describeMissing ? 'Install MOSS-VoiceGen for described voices' : 'Send'}>
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
