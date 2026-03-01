import React, { useState, useRef, useEffect, useCallback, ReactNode } from 'react';
import MarkdownMessage from '../../MarkdownMessage';
import { useOmniChat, OmniMessage } from '../../hooks/useOmniChat';
import { useModels, DownloadedModel } from '../../hooks/useModels';
import { useLiveTranscription } from '../../hooks/useLiveTranscription';
import { useTTS } from '../../hooks/useTTS';
import { PAUSED } from '../../AudioButton';
import { adjustTextareaHeight } from '../../utils/textareaUtils';
import InferenceControls from '../InferenceControls';
import ModelSelector from '../ModelSelector';
import ImagePreviewList from '../ImagePreviewList';
import { ImageUploadIcon } from '../Icons';
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

// --- Feature 1: Tool detail renderer ---
function renderToolDetail(result: { tool_name: string; data: any; summary: string }): ReactNode {
  const { tool_name, data, summary } = result;
  if (!data) return <span className="omni-tool-detail-text">{summary}</span>;

  switch (tool_name) {
    case 'web_search':
      return (
        <div className="omni-tool-detail">
          <strong>Query:</strong> {data.query}
          {data.results?.length > 0 && (
            <ul>
              {data.results.map((r: any, i: number) => (
                <li key={i}>
                  <a href={r.url} target="_blank" rel="noopener noreferrer">{r.title}</a>
                  {r.snippet && <span className="omni-tool-snippet"> — {r.snippet}</span>}
                </li>
              ))}
            </ul>
          )}
        </div>
      );

    case 'read_file':
      return (
        <div className="omni-tool-detail">
          <strong>{data.path}</strong> ({data.size ? `${data.size} bytes` : ''})
          {data.content && (
            <pre>{data.content.length > 2000 ? data.content.slice(0, 2000) + '\n... (truncated)' : data.content}</pre>
          )}
        </div>
      );

    case 'write_file':
      return (
        <div className="omni-tool-detail">
          <strong>{data.path}</strong> — {data.bytes_written} bytes written
        </div>
      );

    case 'run_command':
      return (
        <div className="omni-tool-detail">
          <pre>$ {data.command}</pre>
          {data.stdout && <pre>{data.stdout}</pre>}
          {data.stderr && <pre className="stderr">{data.stderr}</pre>}
          {data.exit_code !== undefined && data.exit_code !== 0 && (
            <span className="stderr">Exit code: {data.exit_code}</span>
          )}
        </div>
      );

    case 'list_directory':
      return (
        <div className="omni-tool-detail">
          <strong>{data.path}</strong> ({data.count ?? data.entries?.length ?? 0} entries)
          {data.entries?.length > 0 && (
            <pre>{data.entries.map((e: any) => `${e.type === 'directory' ? '📁' : '📄'} ${e.name}${e.size ? ` (${e.size})` : ''}`).join('\n')}</pre>
          )}
        </div>
      );

    case 'generate_image':
    case 'edit_image':
    case 'describe_image':
    case 'analyze_image':
    case 'text_to_speech':
    case 'transcribe_audio':
      return <div className="omni-tool-detail"><span>{summary}</span></div>;

    default:
      return (
        <div className="omni-tool-detail">
          <pre>{JSON.stringify(data, null, 2)}</pre>
        </div>
      );
  }
}

const OmniPanel: React.FC<OmniPanelProps> = ({
  isBusy, isPreFlight, isInferring, activeModality,
  runPreFlight, reset, showError, appSettings,
}) => {
  const { selectedModel, modelsData } = useModels();
  const { messages, sendMessage, isProcessing, currentStep, clearMessages, abort } = useOmniChat();
  const [inputValue, setInputValue] = useState('');

  // Feature 1: Expandable tool step details
  const [expandedResults, setExpandedResults] = useState<Set<string>>(new Set());

  // Feature 2: Multimodal input
  const [uploadedImages, setUploadedImages] = useState<string[]>([]);
  const fileInputRef = useRef<HTMLInputElement>(null);

  // Feature 4: Voice loop
  const [voiceLoopActive, setVoiceLoopActive] = useState(false);
  const voiceLoopRef = useRef(false);
  const pendingSendRef = useRef(false);

  const messagesEndRef = useRef<HTMLDivElement>(null);
  const messagesContainerRef = useRef<HTMLDivElement>(null);
  const inputTextareaRef = useRef<HTMLTextAreaElement>(null);

  const { doTextToSpeech, stopAudio, audioState } = useTTS(appSettings, modelsData);

  const omniModelFilter = useCallback(
    (m: DownloadedModel) => m.info.labels?.includes('tool-calling') === true,
    []
  );

  // Keep voiceLoopRef in sync
  useEffect(() => { voiceLoopRef.current = voiceLoopActive; }, [voiceLoopActive]);

  // Reuse the same live transcription logic from TranscriptionPanel
  const mic = useLiveTranscription({
    modelName: DEFAULT_WHISPER_MODEL,
    modelsData,
    runPreFlight,
    onError: showError,
    onSpeechStopped: useCallback(() => {
      if (!voiceLoopRef.current) return;
      // VAD detected silence — auto-send
      pendingSendRef.current = true;
    }, []),
  });

  // Show live transcript preview in the input field while recording
  useEffect(() => {
    if (mic.isRecording && mic.transcript) {
      setInputValue(mic.transcript);
    }
  }, [mic.isRecording, mic.transcript]);

  // Voice loop: auto-send after VAD silence detected
  useEffect(() => {
    if (pendingSendRef.current && voiceLoopActive) {
      pendingSendRef.current = false;
      const transcript = mic.stop();
      if (transcript.trim()) {
        setInputValue('');
        // Direct send
        (async () => {
          try {
            const ready = await runPreFlight('llm', {
              modelName: selectedModel || '',
              modelsData,
              onError: showError,
            });
            if (!ready) return;
            await sendMessage(transcript.trim(), selectedModel || '');
          } catch (error: any) {
            showError(error.message);
          } finally {
            reset();
          }
        })();
      } else {
        // Empty transcript — restart listening
        mic.start();
      }
    }
  }, [mic.transcript, voiceLoopActive]);

  // Voice loop: speak response after processing completes
  const prevIsProcessingRef = useRef(false);
  useEffect(() => {
    if (prevIsProcessingRef.current && !isProcessing && voiceLoopActive && messages.length > 0) {
      const lastMsg = messages[messages.length - 1];
      if (lastMsg.role === 'assistant' && lastMsg.content) {
        const voice = appSettings?.tts?.assistantVoice?.value || '';
        doTextToSpeech(lastMsg.content, voice).catch(() => {
          // TTS failed, restart listening
          if (voiceLoopRef.current) mic.start();
        });
      } else {
        // No text to speak, restart listening
        if (voiceLoopRef.current) mic.start();
      }
    }
    prevIsProcessingRef.current = isProcessing;
  }, [isProcessing, voiceLoopActive]);

  // Voice loop: restart listening after TTS finishes
  useEffect(() => {
    if (voiceLoopActive && audioState === PAUSED && !isProcessing && !mic.isRecording) {
      // Small delay to avoid catching the tail end of speaker audio
      const timer = setTimeout(() => {
        if (voiceLoopRef.current && !mic.isRecording) {
          mic.start();
        }
      }, 300);
      return () => clearTimeout(timer);
    }
  }, [audioState, voiceLoopActive, isProcessing, mic.isRecording]);

  // Scroll to bottom when new messages arrive
  useEffect(() => {
    messagesEndRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [messages, currentStep]);

  // --- Feature 1: Toggle expanded result ---
  const toggleResult = useCallback((key: string) => {
    setExpandedResults(prev => {
      const next = new Set(prev);
      if (next.has(key)) next.delete(key);
      else next.add(key);
      return next;
    });
  }, []);

  // --- Feature 2: Image handlers ---
  const handleImageUpload = useCallback((event: React.ChangeEvent<HTMLInputElement>) => {
    const files = event.target.files;
    if (!files || files.length === 0) return;
    const file = files[0];
    const reader = new FileReader();
    reader.onload = (e) => {
      const result = e.target?.result;
      if (typeof result === 'string') setUploadedImages(prev => [...prev, result]);
    };
    reader.readAsDataURL(file);
    event.target.value = '';
  }, []);

  const handleImagePaste = useCallback((event: React.ClipboardEvent) => {
    const items = event.clipboardData.items;
    for (let i = 0; i < items.length; i++) {
      const item = items[i];
      if (item.type.indexOf('image') !== -1) {
        event.preventDefault();
        const file = item.getAsFile();
        if (!file) continue;
        const reader = new FileReader();
        reader.onload = (e) => {
          const result = e.target?.result;
          if (typeof result === 'string') setUploadedImages(prev => [...prev, result]);
        };
        reader.readAsDataURL(file);
        break;
      }
    }
  }, []);

  const handleImageRemove = useCallback((index: number) => {
    setUploadedImages(prev => prev.filter((_, i) => i !== index));
  }, []);

  const handleDrop = useCallback((event: React.DragEvent) => {
    event.preventDefault();
    const files = event.dataTransfer.files;
    if (!files || files.length === 0) return;
    const file = files[0];
    if (!file.type.startsWith('image/')) return;
    const reader = new FileReader();
    reader.onload = (e) => {
      const result = e.target?.result;
      if (typeof result === 'string') setUploadedImages(prev => [...prev, result]);
    };
    reader.readAsDataURL(file);
  }, []);

  // --- Feature 3: Image actions ---
  const saveImage = useCallback((base64: string, prompt: string) => {
    const link = document.createElement('a');
    link.href = `data:image/png;base64,${base64}`;
    link.download = `lemonade_${prompt.slice(0, 30).replace(/[^a-z0-9]/gi, '_')}_${Date.now()}.png`;
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
  }, []);

  const getImagePrompt = useCallback((msg: OmniMessage): string => {
    if (!msg.omniSteps) return 'image';
    for (const step of msg.omniSteps) {
      for (const tc of step.tool_calls || []) {
        if (tc.function?.name === 'generate_image' || tc.function?.name === 'edit_image') {
          try {
            const args = typeof tc.function.arguments === 'string'
              ? JSON.parse(tc.function.arguments) : tc.function.arguments;
            if (args?.prompt) return args.prompt;
          } catch { /* skip */ }
        }
      }
    }
    return 'image';
  }, []);

  const handleSend = useCallback(async () => {
    const text = inputValue.trim();
    if (!text || isBusy || isProcessing || !selectedModel) return;

    const images = uploadedImages.length > 0 ? [...uploadedImages] : undefined;
    setInputValue('');
    setUploadedImages([]);
    if (inputTextareaRef.current) {
      inputTextareaRef.current.style.height = 'auto';
    }

    try {
      const ready = await runPreFlight('llm', {
        modelName: selectedModel,
        modelsData,
        onError: showError,
      });
      if (!ready) return;

      await sendMessage(text, selectedModel, images);
    } catch (error: any) {
      showError(error.message);
    } finally {
      reset();
    }
  }, [inputValue, uploadedImages, isBusy, isProcessing, selectedModel, sendMessage, showError, runPreFlight, modelsData, reset]);

  const handleStop = useCallback(() => {
    abort();
    reset();
  }, [abort, reset]);

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

  // --- Feature 4: Voice loop toggle ---
  const handleVoiceLoopToggle = useCallback(async () => {
    if (voiceLoopActive) {
      setVoiceLoopActive(false);
      if (mic.isRecording) mic.stop();
      stopAudio();
    } else {
      setVoiceLoopActive(true);
      setInputValue('');
      await mic.start();
    }
  }, [voiceLoopActive, mic, stopAudio]);

  const getVoiceLoopStatus = (): string | null => {
    if (!voiceLoopActive) return null;
    if (mic.isRecording) return 'Listening...';
    if (isProcessing) return 'Thinking...';
    if (audioState === PAUSED) return null;
    return 'Speaking...';
  };

  const micButton = (
    <button
      className={`omni-mic-button ${mic.isRecording ? 'recording' : ''}`}
      onClick={handleMicToggle}
      disabled={isProcessing || isBusy || voiceLoopActive}
      title={mic.isRecording ? 'Stop recording' : 'Start voice input'}
    >
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
        <path d="M12 1a3 3 0 0 0-3 3v8a3 3 0 0 0 6 0V4a3 3 0 0 0-3-3z" />
        <path d="M19 10v2a7 7 0 0 1-14 0v-2" />
        <line x1="12" y1="19" x2="12" y2="23" />
        <line x1="8" y1="23" x2="16" y2="23" />
      </svg>
    </button>
  );

  const voiceLoopButton = (
    <button
      className={`omni-voice-loop-button ${voiceLoopActive ? 'active' : ''}`}
      onClick={handleVoiceLoopToggle}
      disabled={isBusy && !voiceLoopActive}
      title={voiceLoopActive ? 'Stop voice loop' : 'Start voice loop (continuous listen → send → speak)'}
    >
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
        <path d="M17 2l4 4-4 4" />
        <path d="M3 11v-1a4 4 0 0 1 4-4h14" />
        <path d="M7 22l-4-4 4-4" />
        <path d="M21 13v1a4 4 0 0 1-4 4H3" />
      </svg>
    </button>
  );

  const imageUploadButton = (
    <button
      className="omni-image-upload-button"
      onClick={() => fileInputRef.current?.click()}
      disabled={isProcessing || isBusy}
      title="Upload image"
    >
      <ImageUploadIcon />
    </button>
  );

  const leftControls = (
    <>
      {micButton}
      {voiceLoopButton}
      {imageUploadButton}
    </>
  );

  const voiceStatus = getVoiceLoopStatus();

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

        {messages.map((msg, msgIdx) => (
          <div key={msgIdx} className={`omni-message omni-message-${msg.role}`}>
            {msg.role === 'user' ? (
              <div className="omni-user-bubble">
                {msg.content}
                {/* Show attached images in user bubble */}
                {msg.attachedImages && msg.attachedImages.length > 0 && (
                  <div className="omni-user-attached-images">
                    {msg.attachedImages.map((img, i) => (
                      <img key={i} src={img} alt={`Attached ${i + 1}`} className="omni-user-thumbnail" />
                    ))}
                  </div>
                )}
              </div>
            ) : (
              <div className="omni-assistant-response">
                {/* Feature 1: Expandable step indicators */}
                {msg.omniSteps && msg.omniSteps.length > 0 && (
                  <div className="omni-steps-summary">
                    {msg.omniSteps.map((step, stepIdx) =>
                      step.results.map((r, resultIdx) => {
                        const key = `${msgIdx}-${stepIdx}-${resultIdx}`;
                        const isExpanded = expandedResults.has(key);
                        return (
                          <div key={key} className="omni-tool-result-group">
                            <span
                              className={`omni-tool-badge clickable ${r.success ? 'success' : 'error'}`}
                              onClick={() => toggleResult(key)}
                              title="Click to expand details"
                            >
                              {r.tool_name.replace(/_/g, ' ')}
                              <span className="omni-badge-chevron">{isExpanded ? ' ▾' : ' ▸'}</span>
                            </span>
                            {isExpanded && renderToolDetail(r)}
                          </div>
                        );
                      })
                    )}
                  </div>
                )}

                {/* Feature 3: Generated images with action buttons */}
                {msg.images && msg.images.length > 0 && (
                  <div className="omni-images">
                    {msg.images.map((img, imgIdx) => {
                      const prompt = getImagePrompt(msg);
                      return (
                        <div key={imgIdx} className="omni-image-wrapper">
                          <img
                            src={`data:image/png;base64,${img}`}
                            alt="Generated"
                            className="omni-generated-image"
                          />
                          <div className="omni-image-actions">
                            <button onClick={() => saveImage(img, prompt)} title="Save image">
                              ↓ Save
                            </button>
                            <button
                              onClick={() => {
                                const dataUrl = `data:image/png;base64,${img}`;
                                setUploadedImages([dataUrl]);
                                inputTextareaRef.current?.focus();
                              }}
                              title="Edit this image"
                            >
                              ✎ Edit
                            </button>
                            <button
                              onClick={() => {
                                const dataUrl = `data:image/png;base64,${img}`;
                                setUploadedImages(prev => [...prev, dataUrl]);
                              }}
                              title="Attach to next message"
                            >
                              📎 Use
                            </button>
                          </div>
                        </div>
                      );
                    })}
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

        {/* Pre-flight loading indicator */}
        {isPreFlight && activeModality === 'llm' && (
          <div className="omni-step-indicator">
            <div className="omni-step-spinner" />
            <span>Loading model...</span>
          </div>
        )}

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
        {/* Voice loop status */}
        {voiceStatus && (
          <div className="omni-voice-status">{voiceStatus}</div>
        )}

        <div
          className="omni-input-container"
          onDragOver={(e) => e.preventDefault()}
          onDrop={handleDrop}
        >
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
            onPaste={handleImagePaste}
            rows={1}
            disabled={isProcessing || isBusy || mic.isRecording}
          />

          {/* Feature 2: Image preview thumbnails */}
          <ImagePreviewList
            images={uploadedImages}
            onRemove={handleImageRemove}
            className="omni-image-previews"
          />

          {/* Hidden file input for image upload */}
          <input
            type="file"
            accept="image/*"
            ref={fileInputRef}
            onChange={handleImageUpload}
            style={{ display: 'none' }}
          />

          <InferenceControls
            isBusy={isBusy || isProcessing}
            isInferring={isInferring || isProcessing}
            stoppable={true}
            onSend={handleSend}
            onStop={handleStop}
            sendDisabled={!inputValue.trim() && uploadedImages.length === 0 || mic.isRecording}
            modelSelector={<ModelSelector disabled={isBusy || isProcessing} filter={omniModelFilter} />}
            leftControls={leftControls}
          />
        </div>
      </div>
    </div>
  );
};

export default OmniPanel;
