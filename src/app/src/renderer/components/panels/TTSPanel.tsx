import React, { useState, useRef } from 'react';
import { useModels } from '../../hooks/useModels';
import { Modality } from '../../hooks/useInferenceState';
import { ModelsData } from '../../utils/modelData';
import { AppSettings } from '../../utils/appSettings';
import { useTTS } from '../../hooks/useTTS';
import { MessageContent } from '../../utils/chatTypes';
import { voiceOptions } from '../../tabs/TTSSettings';
import { PLAYING } from '../../AudioButton';
import AudioButton from '../../AudioButton';
import MarkdownMessage from '../../MarkdownMessage';
import { SendIcon, StopIcon } from '../Icons';
import ModelSelector from '../ModelSelector';
import EmptyState from '../EmptyState';
import TypingIndicator from '../TypingIndicator';

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
  const tts = useTTS(appSettings, modelsData);

  const [inputValue, setInputValue] = useState('');
  const [ttsMessageHistory, setTTSMessageHistory] = useState<MessageContent[]>([]);
  const [editingIndex, setEditingIndex] = useState<number | null>(null);
  const [editingValue, setEditingValue] = useState('');

  const inputTextareaRef = useRef<HTMLTextAreaElement>(null);
  const editTextareaRef = useRef<HTMLTextAreaElement>(null);
  const messagesEndRef = useRef<HTMLDivElement>(null);
  const messagesContainerRef = useRef<HTMLDivElement>(null);

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

  const handleMessageToSpeech = async () => {
    if (!inputValue.trim() || isBusy) return;

    const textToSpeechModel = appSettings?.tts.model.value;
    if (!textToSpeechModel) return;

    const ready = await runPreFlight('speech', {
      modelName: textToSpeechModel,
      modelsData,
      onError: showError,
    });
    if (!ready) return;

    try {
      await tts.doTextToSpeech(inputValue, tts.currentVoice);
      setTTSMessageHistory(prev => [...prev, inputValue]);
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
    const message = ttsMessageHistory[index];
    setEditingIndex(index);
    if (typeof message === 'string') {
      setEditingValue(message);
    } else {
      const textContent = message.find(item => item.type === 'text');
      setEditingValue(textContent ? textContent.text : '');
    }
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

  const submitAudioMessageEdit = () => {
    if (!editingValue.trim() || editingIndex === null || isBusy) return;
    const updated = [...ttsMessageHistory];
    updated[editingIndex] = editingValue;
    setTTSMessageHistory(updated);
    setEditingIndex(null);
    setEditingValue('');
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

  const renderMessageContent = (content: MessageContent) => {
    if (typeof content === 'string') {
      return <MarkdownMessage content={content} isComplete={false} />;
    }
    return (
      <div className="message-content-array">
        {content.map((item, index) => {
          if (item.type === 'text') {
            return <MarkdownMessage key={index} content={item.text} isComplete={false} />;
          } else if (item.type === 'image_url') {
            return <img key={index} src={item.image_url.url} alt="Uploaded" className="message-image" />;
          }
          return null;
        })}
      </div>
    );
  };

  return (
    <>
      <div className="chat-messages" ref={messagesContainerRef}>
        {ttsMessageHistory.length === 0 && <EmptyState title="Lemonade Text to Speech" />}
        {ttsMessageHistory.map((message, index) => (
          <div key={index} className="chat-message user-message">
            <AudioButton
              textMessage={message}
              buttonIndex={index}
              onClickFunction={tts.handleAudioButtonClick}
              buttonContext={{ buttonId: tts.pressedAudioButton, audioState: tts.audioState }}
            />
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
              <div
                onClick={(e) => !isBusy && handleEditAudioMessage(index, e)}
                style={{ cursor: !isBusy ? 'pointer' : 'default' }}
              >
                {renderMessageContent(message)}
              </div>
            )}
          </div>
        ))}

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
          <select
            className="form-input form-select"
            value={tts.currentVoice}
            onChange={(e) => tts.setVoice(e.target.value)}
          >
            {voiceOptions.map((voice: string, index: number) => {
              const label = (voice === '') ? 'Select a voice...' : voice;
              return <option key={index} value={voice} disabled={voice === ''}>{label}</option>;
            })}
          </select>
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
              <ModelSelector disabled={isBusy} />
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
              <button className="chat-send-button" onClick={handleMessageToSpeech} disabled={!inputValue.trim()} title="Send">
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
