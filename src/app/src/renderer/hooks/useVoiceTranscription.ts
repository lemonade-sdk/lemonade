import { useRef, useState, useCallback, useEffect } from 'react';
import { Modality } from './useInferenceState';
import { ModelsData } from '../utils/modelData';
import { useModels } from './useModels';
import { useAudioCapture } from './useAudioCapture';
import { TranscriptionWebSocket } from '../utils/websocketClient';
import { adjustTextareaHeight } from '../utils/textareaUtils';
import { serverFetch } from '../utils/serverConfig';

// How long to keep the WebSocket open after stop() waiting for the server's
// 'completed' transcript message. Slow models (e.g. Whisper Large) can take
// several seconds to finish inference.
const WS_CLOSE_TIMEOUT_MS = 30_000;

// If 'completed' event never arrives (server error / crash), submit
// whatever text is buffered after this delay. Kept in sync with the socket
// timeout so both paths resolve at the same time.
const TRANSCRIPT_FALLBACK_MS = WS_CLOSE_TIMEOUT_MS;

interface UseVoiceTranscriptionOptions {
  inputValue: string;
  setInputValue: (value: string) => void;
  textareaRef?: React.RefObject<HTMLTextAreaElement | null>;
  runPreFlight: (modality: Modality, options: { modelName: string; modelsData: ModelsData; onError: (msg: string) => void }) => Promise<boolean>;
  reset: () => void;
  onAutoSubmit?: (text: string) => void;
  onError: (msg: string) => void;
}

interface UseVoiceTranscriptionResult {
  whisperModel: string | undefined;
  isRecording: boolean;
  start: () => Promise<void>;
  stop: () => void;
}

/**
 * Returns the name of an already-loaded whispercpp model from the server, or
 * `null` if none is loaded or the health check fails.
 */
async function fetchLoadedWhisperModel(modelsData: ModelsData): Promise<string | null> {
  try {
    const res = await serverFetch('/health');
    if (!res.ok) return null;
    const health = await res.json();
    const allLoaded: { model_name: string }[] = health.all_models_loaded || [];
    const loaded = allLoaded.find((m) => modelsData[m.model_name]?.recipe === 'whispercpp');
    return loaded?.model_name ?? null;
  } catch {
    return null;
  }
}

export function useVoiceTranscription({
  inputValue,
  setInputValue,
  textareaRef,
  runPreFlight,
  reset,
  onAutoSubmit,
  onError,
}: UseVoiceTranscriptionOptions): UseVoiceTranscriptionResult {
  const { modelsData } = useModels();
  const whisperModel = Object.entries(modelsData).find(
    ([, info]) => info.recipe === 'whispercpp' && info.downloaded
  )?.[0] ?? Object.keys(modelsData).find(name => modelsData[name].recipe === 'whispercpp');

  const [isRecording, setIsRecording] = useState(false);

  // Refs that must survive across renders and WS callbacks without stale closures
  const wsClientRef = useRef<TranscriptionWebSocket | null>(null);
  const wsToCloseRef = useRef<TranscriptionWebSocket | null>(null);
  const wsCloseTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const isRecordingRef = useRef(false);
  const finalsRef = useRef('');
  const baseTextRef = useRef('');
  const pendingAutoSubmitRef = useRef(false);
  const fallbackTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  // Always-current refs for values used inside WS callbacks
  const inputValueRef = useRef(inputValue);
  const stopRecordingRef = useRef<() => void>(() => {});
  const resetRef = useRef(reset);
  const onAutoSubmitRef = useRef(onAutoSubmit);
  const setInputValueRef = useRef(setInputValue);

  inputValueRef.current = inputValue;
  resetRef.current = reset;
  onAutoSubmitRef.current = onAutoSubmit;
  setInputValueRef.current = setInputValue;

  const handleAudioChunk = useCallback((base64: string) => {
    wsClientRef.current?.sendAudio(base64);
  }, []);

  const { startRecording, stopRecording, error: micError } =
    useAudioCapture(handleAudioChunk);

  stopRecordingRef.current = stopRecording;

  useEffect(() => { if (micError) onError(micError); }, [micError, onError]);

  useEffect(() => () => {
    if (isRecordingRef.current) stopRecording();
    wsClientRef.current?.close();
    if (wsCloseTimerRef.current) clearTimeout(wsCloseTimerRef.current);
    wsToCloseRef.current?.close();
    if (fallbackTimerRef.current) clearTimeout(fallbackTimerRef.current);
  }, []);

  // Called once the final transcript is handled (or the safety timeout fires).
  const flushWsClose = useCallback(() => {
    if (wsCloseTimerRef.current) {
      clearTimeout(wsCloseTimerRef.current);
      wsCloseTimerRef.current = null;
    }
    wsToCloseRef.current?.close();
    wsToCloseRef.current = null;
  }, []);

  const closeWs = useCallback(() => {
    if (wsClientRef.current) {
      // Commit: tell the server to transcribe buffered audio.
      // Keep the socket open so the 'completed' message can be delivered
      wsClientRef.current.commitAudio();
      wsToCloseRef.current = wsClientRef.current;
      wsClientRef.current = null;
      // Safety timeout — close regardless if no response.
      wsCloseTimerRef.current = setTimeout(flushWsClose, WS_CLOSE_TIMEOUT_MS);
    }
  }, [flushWsClose]);

  const doAutoStop = useCallback((transcribedValue: string) => {
    isRecordingRef.current = false;
    stopRecordingRef.current();
    closeWs();
    finalsRef.current = '';
    baseTextRef.current = '';
    setIsRecording(false);
    resetRef.current();
    onAutoSubmitRef.current?.(transcribedValue);
  }, [closeWs]);

  // Stable callback given to the WS at connect time; uses refs so it never goes stale.
  const handleTranscription = useCallback((text: string, isFinal: boolean) => {
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

    const base = baseTextRef.current;
    const separator = base && !base.endsWith(' ') ? ' ' : '';
    const newValue = base + separator + liveText;
    setInputValueRef.current(newValue);
    if (textareaRef?.current) adjustTextareaHeight(textareaRef.current);

    if (!isFinal) return;

    if (isRecordingRef.current) {
      // VAD-triggered end of speech — auto stop and submit
      doAutoStop(newValue.trim());
    } else if (pendingAutoSubmitRef.current) {
      // Manual stop already happened; 'completed' arrived — close socket and submit.
      pendingAutoSubmitRef.current = false;
      if (fallbackTimerRef.current) clearTimeout(fallbackTimerRef.current);
      finalsRef.current = '';
      baseTextRef.current = '';
      flushWsClose();
      onAutoSubmitRef.current?.(newValue.trim());
    }
  }, [textareaRef, doAutoStop, flushWsClose]);

  const start = useCallback(async () => {
    if (!whisperModel) {
      onError('No Whisper model available. Pull one from the Model Manager first.');
      return;
    }
    baseTextRef.current = inputValue;
    finalsRef.current = '';

    // Prefer an already-loaded whisper model to avoid an unnecessary reload.
    const modelToUse = (await fetchLoadedWhisperModel(modelsData)) ?? whisperModel;

    const ready = await runPreFlight('transcription', {
      modelName: modelToUse,
      modelsData,
      onError: (msg) => onError(`Error preparing model: ${msg}`),
    });
    if (!ready) return;

    try {
      wsClientRef.current = await TranscriptionWebSocket.connect(modelToUse, {
        onTranscription: handleTranscription,
        onSpeechEvent: () => {},
        onError: (err) => onError(err),
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
  }, [whisperModel, modelsData, inputValue, handleTranscription, startRecording, runPreFlight, reset, onError]);

  // Manual stop — mic stops immediately; wait for completed before submitting (3s fallback)
  const stop = useCallback(() => {
    stopRecording();
    isRecordingRef.current = false;
    closeWs();
    setIsRecording(false);
    reset();

    pendingAutoSubmitRef.current = true;
    // Fallback: if 'completed' never arrives (e.g. server error), submit whatever
    // text is in the input after the timeout (matches the socket safety timeout).
    fallbackTimerRef.current = setTimeout(() => {
      if (pendingAutoSubmitRef.current) {
        pendingAutoSubmitRef.current = false;
        finalsRef.current = '';
        baseTextRef.current = '';
        flushWsClose();
        const text = inputValueRef.current.trim();
        if (text) onAutoSubmitRef.current?.(text);
      }
    }, TRANSCRIPT_FALLBACK_MS);
  }, [stopRecording, reset, closeWs, flushWsClose]);

  return { whisperModel, isRecording, start, stop };
}
