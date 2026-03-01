import { useState, useRef, useCallback, useEffect } from 'react';
import { useAudioCapture } from './useAudioCapture';
import { TranscriptionWebSocket } from '../utils/websocketClient';
import { getServerBaseUrl } from '../utils/serverConfig';
import { Modality } from './useInferenceState';
import { ModelsData } from '../utils/modelData';

interface UseLiveTranscriptionOptions {
  modelName: string;
  modelsData: ModelsData;
  runPreFlight: (modality: Modality, options: { modelName: string; modelsData: ModelsData; onError: (msg: string) => void }) => Promise<boolean>;
  onError?: (msg: string) => void;
  onSpeechStopped?: () => void;
}

interface UseLiveTranscriptionReturn {
  isRecording: boolean;
  isConnected: boolean;
  transcript: string;
  audioLevel: number;
  error: string | null;
  start: () => Promise<void>;
  stop: () => string;
}

/**
 * Reusable hook for live microphone transcription via WebSocket.
 * Extracted from TranscriptionPanel so OmniPanel (and others) can share the same logic.
 */
export function useLiveTranscription({
  modelName,
  modelsData,
  runPreFlight,
  onError,
  onSpeechStopped,
}: UseLiveTranscriptionOptions): UseLiveTranscriptionReturn {
  const [isLiveRecording, setIsLiveRecording] = useState(false);
  const [isLiveConnected, setIsLiveConnected] = useState(false);
  const [audioLevel, setAudioLevel] = useState(0);
  const [liveTranscript, setLiveTranscript] = useState('');
  const [error, setError] = useState<string | null>(null);

  const audioLevelRef = useRef(0);
  const wsClientRef = useRef<TranscriptionWebSocket | null>(null);
  const isLiveRecordingRef = useRef(false);
  const liveTranscriptRef = useRef('');

  const handleAudioChunk = useCallback((base64: string) => {
    wsClientRef.current?.sendAudio(base64);
  }, []);

  const handleAudioLevel = useCallback((level: number) => {
    const smoothed = audioLevelRef.current * 0.7 + level * 0.3;
    audioLevelRef.current = smoothed;
    setAudioLevel(smoothed);
  }, []);

  const { isRecording: isMicActive, startRecording, stopRecording, error: micError } =
    useAudioCapture(handleAudioChunk, handleAudioLevel);

  const handleLiveTranscription = useCallback((text: string, isFinal: boolean) => {
    if (!isLiveRecordingRef.current) return;
    const trimmed = text.trim();
    if (!trimmed) return;

    const accumulated = liveTranscriptRef.current;
    if (isFinal) {
      const next = accumulated ? `${accumulated} ${trimmed}` : trimmed;
      liveTranscriptRef.current = next;
      setLiveTranscript(next);
    } else {
      const display = accumulated ? `${accumulated} ${trimmed}` : trimmed;
      setLiveTranscript(display);
    }
  }, []);

  const handleSpeechEvent = useCallback((event: 'started' | 'stopped') => {
    if (event === 'stopped') {
      onSpeechStopped?.();
    }
  }, [onSpeechStopped]);

  const start = useCallback(async () => {
    setError(null);
    setLiveTranscript('');
    liveTranscriptRef.current = '';

    const ready = await runPreFlight('transcription', {
      modelName,
      modelsData,
      onError: (msg) => {
        const errMsg = `Error preparing model: ${msg}`;
        setError(errMsg);
        onError?.(errMsg);
      },
    });
    if (!ready) return;

    try {
      const serverUrl = getServerBaseUrl();
      wsClientRef.current = await TranscriptionWebSocket.connect(serverUrl, modelName, {
        onTranscription: handleLiveTranscription,
        onSpeechEvent: handleSpeechEvent,
        onError: (err) => { setError(err); onError?.(err); },
        onConnected: () => setIsLiveConnected(true),
        onDisconnected: () => setIsLiveConnected(false),
      });

      await new Promise(r => setTimeout(r, 500));
      await startRecording();
      isLiveRecordingRef.current = true;
      setIsLiveRecording(true);
    } catch (err) {
      const msg = err instanceof Error ? err.message : 'Failed to connect';
      setError(msg);
      onError?.(msg);
    }
  }, [modelName, modelsData, handleLiveTranscription, handleSpeechEvent, startRecording, runPreFlight, onError]);

  const stop = useCallback((): string => {
    stopRecording();
    isLiveRecordingRef.current = false;

    const finalText = liveTranscriptRef.current.trim();

    liveTranscriptRef.current = '';
    setLiveTranscript('');

    if (wsClientRef.current) {
      wsClientRef.current.clearAudio();
      const wsToClose = wsClientRef.current;
      wsClientRef.current = null;
      setIsLiveConnected(false);
      setTimeout(() => { wsToClose.close(); }, 3000);
    }

    setIsLiveRecording(false);
    setAudioLevel(0);
    audioLevelRef.current = 0;

    return finalText;
  }, [stopRecording]);

  // Cleanup on unmount
  useEffect(() => {
    return () => {
      if (isLiveRecordingRef.current) {
        stopRecording();
      }
      wsClientRef.current?.close();
    };
  }, []);

  return {
    isRecording: isLiveRecording,
    isConnected: isLiveConnected,
    transcript: liveTranscript,
    audioLevel,
    error: error || micError || null,
    start,
    stop,
  };
}
