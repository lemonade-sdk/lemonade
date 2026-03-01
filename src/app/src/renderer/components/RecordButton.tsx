import React, { useState, useRef, useCallback, useEffect } from 'react';
import { MicrophoneIcon } from './Icons';
import { Modality } from '../hooks/useInferenceState';
import { ModelsData } from '../utils/modelData';
import { useAudioCapture } from '../hooks/useAudioCapture';
import { TranscriptionWebSocket } from '../utils/websocketClient';
import { adjustTextareaHeight } from '../utils/textareaUtils';

interface RecordButtonProps {
  disabled?: boolean;
  modelsData: ModelsData;
  inputValue: string;
  setInputValue: (updater: string | ((prev: string) => string)) => void;
  textareaRef?: React.RefObject<HTMLTextAreaElement | null>;
  onError: (error: string) => void;
  runPreFlight: (modality: Modality, options: { modelName: string; modelsData: ModelsData; onError: (msg: string) => void }) => Promise<boolean>;
  reset: () => void;
  onAutoSubmit?: (text: string) => void;
}

const RecordButton: React.FC<RecordButtonProps> = ({
  disabled, modelsData, inputValue, setInputValue, textareaRef, onError, runPreFlight, reset, onAutoSubmit,
}) => {
  const whisperModel = Object.entries(modelsData).find(
    ([, info]) => info.recipe === 'whispercpp' && info.downloaded
  )?.[0] ?? Object.keys(modelsData).find(name => modelsData[name].recipe === 'whispercpp');

  const [isRecording, setIsRecording] = useState(false);
  const [isConnected, setIsConnected] = useState(false);
  const [audioLevel, setAudioLevel] = useState(0);

  const wsClientRef = useRef<TranscriptionWebSocket | null>(null);
  const isRecordingRef = useRef(false);
  const finalsRef = useRef('');
  const baseTextRef = useRef('');   // textarea content at recording start
  const inputValueRef = useRef(inputValue); // always-current textarea value
  const audioLevelRef = useRef(0);
  const pendingAutoSubmitRef = useRef(false); // true after manual stop, waiting for completed
  const fallbackTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const onAutoSubmitRef = useRef(onAutoSubmit);
  onAutoSubmitRef.current = onAutoSubmit;
  // Stable refs so WS callbacks can call these without stale closures
  const stopRecordingRef = useRef<() => void>(() => {});
  const resetRef = useRef<() => void>(() => {});

  // Keep refs in sync every render
  inputValueRef.current = inputValue;

  const handleAudioChunk = useCallback((base64: string) => {
    wsClientRef.current?.sendAudio(base64);
  }, []);

  const handleAudioLevel = useCallback((level: number) => {
    const smoothed = audioLevelRef.current * 0.7 + level * 0.3;
    audioLevelRef.current = smoothed;
    setAudioLevel(smoothed);
  }, []);

  const { startRecording, stopRecording, error: micError } =
    useAudioCapture(handleAudioChunk, handleAudioLevel);

  // Sync after useAudioCapture / props are available
  stopRecordingRef.current = stopRecording;
  resetRef.current = reset;

  useEffect(() => { if (micError) onError(micError); }, [micError, onError]);

  useEffect(() => () => {
    if (isRecordingRef.current) stopRecording();
    wsClientRef.current?.close();
    if (fallbackTimerRef.current) clearTimeout(fallbackTimerRef.current);
  }, []);

  // Helper: shared WS teardown used by both VAD auto-stop and manual stop
  const closeWs = useCallback(() => {
    if (wsClientRef.current) {
      wsClientRef.current.clearAudio();
      const wsToClose = wsClientRef.current;
      wsClientRef.current = null;
      setTimeout(() => wsToClose.close(), 3000);
    }
  }, []);

  // Mirrors TranscriptionPanel's handleLiveTranscription, but also updates the textarea live.
  // completed events are always processed; delta events are dropped after manual stop.
  const handleLiveTranscription = useCallback((text: string, isFinal: boolean) => {
    if (!isFinal && !isRecordingRef.current) return;
    const trimmed = text.trim();
    if (!trimmed) return;

    let liveText: string;
    if (isFinal) {
      const next = finalsRef.current ? `${finalsRef.current} ${trimmed}` : trimmed;
      finalsRef.current = next;
      liveText = next;
    } else {
      liveText = finalsRef.current ? `${finalsRef.current} ${trimmed}` : trimmed;
    }

    // Update textarea in real-time
    const base = baseTextRef.current;
    const separator = base && !base.endsWith(' ') ? ' ' : '';
    const newValue = base + separator + liveText;
    setInputValue(newValue);
    if (textareaRef?.current) adjustTextareaHeight(textareaRef.current);

    if (!isFinal) return;

    // completed event — two cases:
    if (isRecordingRef.current) {
      // VAD-triggered: speech ended naturally, auto-stop and submit
      isRecordingRef.current = false;
      stopRecordingRef.current();
      closeWs();
      finalsRef.current = '';
      baseTextRef.current = '';
      setIsRecording(false);
      setIsConnected(false);
      setAudioLevel(0);
      audioLevelRef.current = 0;
      resetRef.current();
      onAutoSubmitRef.current?.(newValue.trim());
    } else if (pendingAutoSubmitRef.current) {
      // User clicked stop manually; completed arrived within the grace window
      pendingAutoSubmitRef.current = false;
      if (fallbackTimerRef.current) clearTimeout(fallbackTimerRef.current);
      finalsRef.current = '';
      baseTextRef.current = '';
      onAutoSubmitRef.current?.(newValue.trim());
    }
  }, [setInputValue, textareaRef, closeWs]);

  // Mirrors TranscriptionPanel's handleMicStart
  const handleMicStart = useCallback(async () => {
    if (!whisperModel) {
      onError('No Whisper model available. Pull one from the Model Manager first.');
      return;
    }
    // Capture current textarea content as base
    baseTextRef.current = inputValue;
    finalsRef.current = '';

    const ready = await runPreFlight('transcription', {
      modelName: whisperModel,
      modelsData,
      onError: (msg) => onError(`Error preparing model: ${msg}`),
    });
    if (!ready) return;

    try {
      wsClientRef.current = await TranscriptionWebSocket.connect(whisperModel, {
        onTranscription: handleLiveTranscription,
        onSpeechEvent: () => {},
        onError: (err) => onError(err),
        onConnected: () => setIsConnected(true),
        onDisconnected: () => setIsConnected(false),
      });
      await new Promise(r => setTimeout(r, 500));
      await startRecording();
      isRecordingRef.current = true;
      setIsRecording(true);
    } catch (err: any) {
      onError(`Failed to connect: ${err?.message ?? err}`);
      wsClientRef.current?.close();
      wsClientRef.current = null;
      reset();
    }
  }, [whisperModel, modelsData, inputValue, handleLiveTranscription, startRecording, runPreFlight, reset, onError]);

  // Manual stop: clean up mic+WS, then wait up to 3s for completed before submitting.
  // If completed arrives first (handled above), the fallback timer is cleared.
  const handleMicStop = useCallback(() => {
    stopRecording();
    isRecordingRef.current = false;
    closeWs();
    setIsRecording(false);
    setIsConnected(false);
    setAudioLevel(0);
    audioLevelRef.current = 0;
    reset();

    pendingAutoSubmitRef.current = true;
    fallbackTimerRef.current = setTimeout(() => {
      if (pendingAutoSubmitRef.current) {
        pendingAutoSubmitRef.current = false;
        finalsRef.current = '';
        baseTextRef.current = '';
        const text = inputValueRef.current.trim();
        if (text) onAutoSubmitRef.current?.(text);
      }
    }, 3000);
  }, [stopRecording, reset, closeWs]);

  const title = !whisperModel
    ? 'No Whisper model available'
    : isRecording ? 'Stop recording'
    : 'Record voice (auto-loads Whisper)';

  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: '6px' }}>
      <button
        className={`audio-file-button${isRecording ? ' recording' : ''}`}
        onClick={isRecording ? handleMicStop : handleMicStart}
        disabled={!isRecording && disabled}
        title={title}
      >
        <MicrophoneIcon active={isRecording} />
      </button>

      {isRecording && (
        isConnected ? (
          <div className="level-meter" style={{ width: '60px' }}>
            <div
              className="level-fill"
              style={{
                width: `${audioLevel * 100}%`,
                backgroundColor: audioLevel > 0.7 ? '#f44336' : audioLevel > 0.3 ? '#4caf50' : '#555',
              }}
            />
          </div>
        ) : (
          <span className="status-indicator">
            <span className="dot connecting" />
            Connecting...
          </span>
        )
      )}
    </div>
  );
};

export default RecordButton;
