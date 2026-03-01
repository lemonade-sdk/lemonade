import React, { useState, useRef, useCallback } from 'react';
import { useModels } from '../../hooks/useModels';
import { Modality } from '../../hooks/useInferenceState';
import { ModelsData } from '../../utils/modelData';
import { serverFetch } from '../../utils/serverConfig';
import { useLiveTranscription } from '../../hooks/useLiveTranscription';
import { MicrophoneIcon } from '../Icons';
import InferenceControls from '../InferenceControls';
import ModelSelector from '../ModelSelector';
import EmptyState from '../EmptyState';
import TypingIndicator from '../TypingIndicator';

interface TranscriptionPanelProps {
  isBusy: boolean;
  isInferring: boolean;
  activeModality: Modality | null;
  runPreFlight: (modality: Modality, options: { modelName: string; modelsData: ModelsData; onError: (msg: string) => void }) => Promise<boolean>;
  reset: () => void;
  showError: (msg: string) => void;
}

const TranscriptionPanel: React.FC<TranscriptionPanelProps> = ({
  isBusy, isInferring, activeModality,
  runPreFlight, reset, showError,
}) => {
  const { selectedModel, modelsData } = useModels();
  const [transcriptionFile, setTranscriptionFile] = useState<File | null>(null);
  const [transcriptionHistory, setTranscriptionHistory] = useState<Array<{ filename: string; text: string }>>([]);
  const audioFileInputRef = useRef<HTMLInputElement>(null);
  const messagesEndRef = useRef<HTMLDivElement>(null);

  // Reusable live transcription hook (shared with OmniPanel)
  const mic = useLiveTranscription({
    modelName: selectedModel,
    modelsData,
    runPreFlight,
    onError: showError,
  });

  const handleMicStart = useCallback(async () => {
    await mic.start();
  }, [mic]);

  const handleMicStop = useCallback(() => {
    const finalText = mic.stop();
    if (finalText) {
      setTranscriptionHistory(h => [...h, { filename: 'Live Recording', text: finalText }]);
    }
    reset();
  }, [mic, reset]);

  const handleAudioFileSelect = (event: React.ChangeEvent<HTMLInputElement>) => {
    const files = event.target.files;
    if (!files || files.length === 0) return;
    setTranscriptionFile(files[0]);
  };

  const handleTranscription = async () => {
    if (!transcriptionFile || isBusy) return;

    const ready = await runPreFlight('transcription', {
      modelName: selectedModel,
      modelsData,
      onError: showError,
    });
    if (!ready) return;

    const currentFile = transcriptionFile;

    try {
      const formData = new FormData();
      formData.append('file', currentFile);
      formData.append('model', selectedModel);

      const response = await serverFetch('/audio/transcriptions', {
        method: 'POST',
        body: formData,
      });

      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);

      const data = await response.json();
      if (data.text) {
        setTranscriptionHistory(prev => [...prev, { filename: currentFile.name, text: data.text }]);
      } else {
        throw new Error('Unexpected response format');
      }

      setTranscriptionFile(null);
      if (audioFileInputRef.current) audioFileInputRef.current.value = '';
    } catch (error: any) {
      console.error('Failed to transcribe:', error);
      showError(`Failed to transcribe: ${error.message || 'Unknown error'}`);
    } finally {
      reset();
    }
  };

  const MicIcon = () => (
    <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
      <path
        d="M12 1C11.2044 1 10.4413 1.31607 9.87868 1.87868C9.31607 2.44129 9 3.20435 9 4V12C9 12.7956 9.31607 13.5587 9.87868 14.1213C10.4413 14.6839 11.2044 15 12 15C12.7956 15 13.5587 14.6839 14.1213 14.1213C14.6839 13.5587 15 12.7956 15 12V4C15 3.20435 14.6839 2.44129 14.1213 1.87868C13.5587 1.31607 12.7956 1 12 1Z"
        stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"
      />
      <path
        d="M19 10V12C19 13.8565 18.2625 15.637 16.9497 16.9497C15.637 18.2625 13.8565 19 12 19C10.1435 19 8.36301 18.2625 7.05025 16.9497C5.7375 15.637 5 13.8565 5 12V10"
        stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"
      />
      <path d="M12 19V23" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"/>
      <path d="M8 23H16" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"/>
    </svg>
  );

  return (
    <>
      <div className="chat-messages">
        {transcriptionHistory.length === 0 && !mic.isRecording && <EmptyState title="Lemonade Transcriber" />}

        {transcriptionHistory.map((item, index) => (
          <div key={index} className="transcription-history-item">
            <div className="transcription-file-info">
              <div className="transcription-label">
                {item.filename === 'Live Recording' ? (
                  <MicIcon />
                ) : (
                  <svg width="16" height="16" viewBox="0 0 24 24" fill="none" style={{ marginRight: '8px' }}>
                    <path
                      d="M12 15V3M12 15L8 11M12 15L16 11M2 17L2.621 19.485C2.725 19.871 2.777 20.064 2.873 20.213C2.958 20.345 3.073 20.454 3.209 20.531C3.364 20.618 3.558 20.658 3.947 20.737L11.053 22.147C11.442 22.226 11.636 22.266 11.791 22.179C11.927 22.102 12.042 21.993 12.127 21.861C12.223 21.712 12.275 21.519 12.379 21.133L13 18.5M22 17L21.379 19.485C21.275 19.871 21.223 20.064 21.127 20.213C21.042 20.345 20.927 20.454 20.791 20.531C20.636 20.618 20.442 20.658 20.053 20.737L12.947 22.147C12.558 22.226 12.364 22.266 12.209 22.179C12.073 22.102 11.958 21.993 11.873 21.861C11.777 21.712 11.725 21.519 11.621 21.133L11 18.5"
                      stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"
                    />
                  </svg>
                )}
                {item.filename}
              </div>
            </div>
            <div className="transcription-result-container">
              <div className="transcription-result-header"><h4>Transcription</h4></div>
              <div className="transcription-result">{item.text}</div>
            </div>
          </div>
        ))}

        {mic.isRecording && (
          <div className="transcription-history-item">
            <div className="transcription-file-info">
              <div className="transcription-label">
                <MicIcon />
                Live Recording
              </div>
            </div>
            <div className="transcription-result-container">
              <div className="transcription-result-header"><h4>Transcription</h4></div>
              <div className="transcription-result">
                {mic.transcript || (mic.isRecording ? 'Listening... Start speaking to see transcription.' : '')}
              </div>
            </div>
          </div>
        )}

        {isBusy && activeModality === 'transcription' && !mic.isRecording && (
          <div className="chat-message assistant-message">
            <TypingIndicator />
          </div>
        )}
        <div ref={messagesEndRef} />
      </div>

      <div className="chat-input-container">
        <div className="chat-input-wrapper">
          <input
            ref={audioFileInputRef}
            type="file"
            accept="audio/*"
            onChange={handleAudioFileSelect}
            style={{ display: 'none' }}
          />

          <div className="transcription-file-display">
            {mic.isRecording ? (
              <div className="live-status">
                {!mic.isConnected ? (
                  <span className="status-indicator">
                    <span className="dot connecting" />
                    Connecting...
                  </span>
                ) : (
                  <div className="level-meter">
                    <div
                      className="level-fill"
                      style={{
                        width: `${mic.audioLevel * 100}%`,
                        backgroundColor: mic.audioLevel > 0.7 ? '#f44336' : mic.audioLevel > 0.3 ? '#4caf50' : '#555',
                      }}
                    />
                  </div>
                )}
              </div>
            ) : transcriptionFile ? (
              <div className="transcription-file-info-display">
                <span className="file-name">{transcriptionFile.name}</span>
                <span className="file-size-indicator">
                  {(transcriptionFile.size / 1024 / 1024).toFixed(2)} MB
                </span>
              </div>
            ) : (
              <span className="transcription-placeholder">No audio file selected</span>
            )}
          </div>

          <InferenceControls
            isBusy={isBusy}
            isInferring={isInferring}
            stoppable={false}
            onSend={handleTranscription}
            sendDisabled={!transcriptionFile || isBusy || mic.isRecording}
            modelSelector={<ModelSelector disabled={isBusy || mic.isRecording} />}
            leftControls={
              <>
                <button
                  className="audio-file-button"
                  onClick={() => audioFileInputRef.current?.click()}
                  disabled={isBusy || mic.isRecording}
                  title="Choose audio file"
                >
                  <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
                    <path
                      d="M21 15V19C21 20.1 20.1 21 19 21H5C3.9 21 3 20.1 3 19V15M17 8L12 3M12 3L7 8M12 3V15"
                      stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"
                    />
                  </svg>
                </button>
                <button
                  className={`audio-file-button${mic.isRecording ? ' recording' : ''}`}
                  onClick={mic.isRecording ? handleMicStop : handleMicStart}
                  disabled={isBusy && !mic.isRecording}
                  title={mic.isRecording ? 'Stop recording' : 'Start live recording'}
                >
                  <MicrophoneIcon active={mic.isRecording} />
                </button>
              </>
            }
          />
        </div>
      </div>

      {mic.error && (
        <div className="transcription-error">{mic.error}</div>
      )}
    </>
  );
};

export default TranscriptionPanel;
