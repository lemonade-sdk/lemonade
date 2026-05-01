import { useRef, useState, useCallback, useEffect } from 'react';
import { Modality } from './useInferenceState';
import { ModelsData } from '../utils/modelData';
import { useModels } from './useModels';
import { useAudioCapture } from './useAudioCapture';
import { TranscriptionWebSocket } from '../utils/websocketClient';
import { adjustTextareaHeight } from '../utils/textareaUtils';
import { serverFetch } from '../utils/serverConfig';

interface UseVoiceTranscriptionOptions {
  inputValue: string;
  setInputValue: (value: string) => void;
  textareaRef?: React.RefObject<HTMLTextAreaElement | null>;
  runPreFlight: (modality: Modality, options: { modelName: string; modelsData: ModelsData; onError: (msg: string) => void }) => Promise<boolean>;
  reset: () => void;
  onError: (msg: string) => void;
}

interface UseVoiceTranscriptionResult {
  activeModel: string | undefined;
  isRecording: boolean;
  start: () => Promise<void>;
  stop: () => void;
}

/**
 * Returns the name of an already-loaded audio model from the server, or
 * `null` if none is loaded or the health check fails.
 */
async function fetchLoadedAudioModel(modelsData: ModelsData): Promise<string | null> {
  try {
    const res = await serverFetch('/health');
    if (!res.ok) return null;
    const health = await res.json();
    const allLoaded: { model_name: string; type?: string }[] = health.all_models_loaded || [];
    // Must be *loaded as audio* — omni models (e.g. gemma4) carry the "audio"
    // label statically but may be currently loaded as an LLM; using one of
    // those for transcription hangs the realtime session.
    const loaded = allLoaded.find(
      (m) => m.type === 'audio' && modelsData[m.model_name]?.labels?.includes('audio'),
    );
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
  onError,
}: UseVoiceTranscriptionOptions): UseVoiceTranscriptionResult {
  const { modelsData } = useModels();
  const audioModels = Object.keys(modelsData).filter(name => modelsData[name]?.labels?.includes('audio'));

  // Prefer the smallest downloaded model (fastest for real-time), fall back to any audio model.
  const activeModel =
    audioModels
      .filter(name => modelsData[name].downloaded)
      .sort((a, b) => (modelsData[a].size ?? Infinity) - (modelsData[b].size ?? Infinity))[0]
    ?? audioModels[0];

  const [isRecording, setIsRecording] = useState(false);

  // Refs that must survive across renders and WS callbacks without stale closures
  const wsClientRef = useRef<TranscriptionWebSocket | null>(null);
  const wsToCloseRef = useRef<TranscriptionWebSocket | null>(null);
  const isRecordingRef = useRef(false);
  const finalsRef = useRef('');
  const baseTextRef = useRef('');

  // Always-current refs for values used inside WS callbacks
  const inputValueRef = useRef(inputValue);
  const setInputValueRef = useRef(setInputValue);

  inputValueRef.current = inputValue;
  setInputValueRef.current = setInputValue;

  const handleAudioChunk = useCallback((base64: string) => {
    wsClientRef.current?.sendAudio(base64);
  }, []);

  const { startRecording, stopRecording, error: micError } =
    useAudioCapture(handleAudioChunk);

  useEffect(() => { if (micError) onError(micError); }, [micError, onError]);

  useEffect(() => () => {
    if (isRecordingRef.current) stopRecording();
    wsClientRef.current?.close();
    wsToCloseRef.current?.close();
  }, []);

  const closeCommittedWs = useCallback(() => {
    if (wsToCloseRef.current) {
      wsToCloseRef.current.close();
      wsToCloseRef.current = null;
    }
  }, []);

  // Manual-stop path: ask the server to finish any active VAD speech window,
  // then keep the socket alive until the server replies with completed/cleared.
  const closeWs = useCallback(() => {
    if (wsClientRef.current) {
      wsClientRef.current.commitAudio();
      wsToCloseRef.current = wsClientRef.current;
      wsClientRef.current = null;
    }
  }, []);

  // Stable callback given to the WS at connect time; uses refs so it never goes stale.
  // Finals from server VAD segments accumulate into the textarea while recording;
  // the user submits with Send, not automatically.
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

    // After manual stop, close the socket once the server finishes or discards
    // the pending buffer.
    if (isFinal && !isRecordingRef.current && wsToCloseRef.current) {
      closeCommittedWs();
    }
  }, [textareaRef, closeCommittedWs]);

  const start = useCallback(async () => {
    if (!activeModel) {
      onError('No Whisper model available. Pull one from the Model Manager first.');
      return;
    }
    baseTextRef.current = inputValue;
    finalsRef.current = '';

    // Prefer an already-loaded model to avoid an unnecessary reload.
    const modelToUse = (await fetchLoadedAudioModel(modelsData)) ?? activeModel;

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
        onAudioBufferCleared: closeCommittedWs,
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
  }, [activeModel, modelsData, inputValue, handleTranscription, startRecording, runPreFlight, reset, onError]);

  // Manual stop — mic stops immediately; commit buffered audio so the server
  // emits a final 'completed' for whatever's in the buffer, then let it settle
  // into the textarea. Do not auto-submit; the user presses Send.
  const stop = useCallback(() => {
    stopRecording();
    isRecordingRef.current = false;
    closeWs();
    setIsRecording(false);
    reset();
  }, [stopRecording, reset, closeWs]);

  return {  activeModel, isRecording, start, stop };
}
