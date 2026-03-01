import React, { useState, useRef, useEffect, useCallback } from 'react';
import MarkdownMessage from '../../MarkdownMessage';
import { useOmniChat } from '../../hooks/useOmniChat';
import { useModels } from '../../hooks/useModels';
import { useLiveTranscription } from '../../hooks/useLiveTranscription';
import { adjustTextareaHeight } from '../../utils/textareaUtils';
import { SendIcon } from '../Icons';
import { AppSettings } from '../../utils/appSettings';
import { Modality } from '../../hooks/useInferenceState';
import { ModelsData } from '../../utils/modelData';

interface OmniPanelProps {
  isBusy: boolean;
  isPreFlight: boolean;
  isInferring: boolean;
  activeModality: Modality | null;
  runPreFlight: (modality: Modality, options: { modelName: string; modelsData: ModelsData; onError: (msg: string) => void }) => Promise<boolean>;
  reset: () => void;
  showError: (msg: string) => void;
  appSettings: AppSettings | null;
}

const DEFAULT_WHISPER_MODEL = 'Whisper-Large-v3-Turbo';

const OmniPanel: React.FC<OmniPanelProps> = ({
  isBusy, runPreFlight, showError,
}) => {
  const { selectedModel, modelsData } = useModels();
  const { messages, sendMessage, isProcessing, currentStep, clearMessages } = useOmniChat();
  const [inputValue, setInputValue] = useState('');

  const messagesEndRef = useRef<HTMLDivElement>(null);
  const messagesContainerRef = useRef<HTMLDivElement>(null);
  const inputTextareaRef = useRef<HTMLTextAreaElement>(null);

  // Reuse the same live transcription logic from TranscriptionPanel
  const mic = useLiveTranscription({
    modelName: DEFAULT_WHISPER_MODEL,
    modelsData,
    runPreFlight,
    onError: showError,
  });

  // Show live transcript preview in the input field while recording
  useEffect(() => {
    if (mic.isRecording && mic.transcript) {
      setInputValue(mic.transcript);
    }
  }, [mic.isRecording, mic.transcript]);

  // Scroll to bottom when new messages arrive
  useEffect(() => {
    messagesEndRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [messages, currentStep]);

  const handleSend = useCallback(async () => {
    const text = inputValue.trim();
    if (!text || isProcessing || !selectedModel) return;

    setInputValue('');
    if (inputTextareaRef.current) {
      inputTextareaRef.current.style.height = 'auto';
    }

    try {
      await sendMessage(text, selectedModel);
    } catch (error: any) {
      showError(error.message);
    }
  }, [inputValue, isProcessing, selectedModel, sendMessage, showError]);

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      handleSend();
    }
  };

  const handleMicToggle = useCallback(async () => {
    if (mic.isRecording) {
      const transcript = mic.stop();
      if (transcript) {
        setInputValue(transcript);
      }
    } else {
      setInputValue('');
      await mic.start();
    }
  }, [mic]);

  return (
    <div className="omni-panel">
      <div className="omni-gradient-bg" />

      <div className="omni-response-area" ref={messagesContainerRef}>
        {messages.length === 0 && (
          <div className="omni-empty-state">
            <h2>Omni Mode</h2>
            <p>Ask me to generate images, describe photos, transcribe audio, or read text aloud.</p>
          </div>
        )}

        {messages.map((msg, idx) => (
          <div key={idx} className={`omni-message omni-message-${msg.role}`}>
            {msg.role === 'user' ? (
              <div className="omni-user-bubble">{msg.content}</div>
            ) : (
              <div className="omni-assistant-response">
                {/* Step indicators */}
                {msg.omniSteps && msg.omniSteps.length > 0 && (
                  <div className="omni-steps-summary">
                    {(() => {
                      const tools = new Map<string, boolean>();
                      for (const step of msg.omniSteps) {
                        for (const r of step.results) {
                          const prev = tools.get(r.tool_name);
                          tools.set(r.tool_name, prev === undefined ? r.success : prev && r.success);
                        }
                      }
                      return Array.from(tools.entries()).map(([name, success]) => (
                        <span key={name} className={`omni-tool-badge ${success ? 'success' : 'error'}`}>
                          {name.replace(/_/g, ' ')}
                        </span>
                      ));
                    })()}
                  </div>
                )}

                {/* Generated images */}
                {msg.images && msg.images.length > 0 && (
                  <div className="omni-images">
                    {msg.images.map((img, imgIdx) => (
                      <img
                        key={imgIdx}
                        src={`data:image/png;base64,${img}`}
                        alt="Generated"
                        className="omni-generated-image"
                      />
                    ))}
                  </div>
                )}

                {/* Audio playback */}
                {msg.audioData && msg.audioData.length > 0 && (
                  <div className="omni-audio-players">
                    {msg.audioData.map((audio, audioIdx) => (
                      <audio
                        key={audioIdx}
                        controls
                        src={`data:audio/${audio.format};base64,${audio.base64}`}
                        className="omni-audio-player"
                      />
                    ))}
                  </div>
                )}

                {/* Text response */}
                {msg.content && <MarkdownMessage content={msg.content} />}
              </div>
            )}
          </div>
        ))}

        {/* Current step indicator */}
        {currentStep && (
          <div className="omni-step-indicator">
            <div className="omni-step-spinner" />
            <span>{currentStep}</span>
          </div>
        )}

        {isProcessing && !currentStep && (
          <div className="omni-step-indicator">
            <div className="omni-step-spinner" />
            <span>Thinking...</span>
          </div>
        )}

        <div ref={messagesEndRef} />
      </div>

      <div className="omni-input-area">
        <div className="omni-input-container">
          <textarea
            ref={inputTextareaRef}
            className="omni-textarea"
            placeholder="Ask me anything..."
            value={inputValue}
            onChange={(e) => {
              setInputValue(e.target.value);
              adjustTextareaHeight(e.target);
            }}
            onKeyDown={handleKeyDown}
            rows={1}
            disabled={isProcessing || mic.isRecording}
          />
          <div className="omni-input-buttons">
            <button
              className={`omni-mic-button ${mic.isRecording ? 'recording' : ''}`}
              onClick={handleMicToggle}
              disabled={isProcessing}
              title={mic.isRecording ? 'Stop recording' : 'Start voice input'}
            >
              <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                <path d="M12 1a3 3 0 0 0-3 3v8a3 3 0 0 0 6 0V4a3 3 0 0 0-3-3z" />
                <path d="M19 10v2a7 7 0 0 1-14 0v-2" />
                <line x1="12" y1="19" x2="12" y2="23" />
                <line x1="8" y1="23" x2="16" y2="23" />
              </svg>
            </button>
            <button
              className="omni-send-button"
              onClick={handleSend}
              disabled={isProcessing || !inputValue.trim() || mic.isRecording}
              title="Send message"
            >
              <SendIcon />
            </button>
          </div>
        </div>
      </div>
    </div>
  );
};

export default OmniPanel;
