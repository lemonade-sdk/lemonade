import React, { useState, useRef } from 'react';
import { serverFetch } from '../utils/serverConfig';
import { MicrophoneIcon } from './Icons';
import { ModelsData } from '../utils/modelData';
import { Modality } from '../hooks/useInferenceState';

interface RecordButtonProps {
  disabled?: boolean;
  onTranscription: (text: string) => void;
  onError: (error: string) => void;
  modelsData: ModelsData;
  runPreFlight: (modality: Modality, options: { modelName: string; modelsData: ModelsData; onError: (msg: string) => void }) => Promise<boolean>;
}

const LoadingIcon = () => (
  <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 18 18" width="18px" height="18px" style={{opacity:1}}>
    <circle cx="3" cy="9" r="2" fill="currentColor">
      <animate id="SVG9IgbRbsl" attributeName="r" begin="0;SVGFUNpCWdG.end-0.35s" dur="0.95s" values="3;.2;3"/>
    </circle>
    <circle cx="9" cy="9" r="2" fill="currentColor">
      <animate attributeName="r" begin="SVG9IgbRbsl.end-0.7s" dur="0.95s" values="3;.2;3"/>
    </circle>
    <circle cx="16" cy="9" r="2" fill="currentColor">
      <animate id="SVGFUNpCWdG" attributeName="r" begin="SVG9IgbRbsl.end-0.55s" dur="0.95s" values="3;.2;3"/>
    </circle>
  </svg>
);

/**
 * Build a WAV Blob from collected Int16Array PCM chunks.
 * whisper.cpp requires 16-bit PCM WAV at 16kHz mono.
 */
function buildWavBlob(chunks: Int16Array[], sampleRate = 16000): Blob {
  const totalSamples = chunks.reduce((n, c) => n + c.length, 0);
  const dataBytes = totalSamples * 2; // 2 bytes per int16 sample
  const buffer = new ArrayBuffer(44 + dataBytes);
  const view = new DataView(buffer);

  const writeStr = (offset: number, str: string) => {
    for (let i = 0; i < str.length; i++) view.setUint8(offset + i, str.charCodeAt(i));
  };

  writeStr(0, 'RIFF');
  view.setUint32(4, 36 + dataBytes, true);
  writeStr(8, 'WAVE');
  writeStr(12, 'fmt ');
  view.setUint32(16, 16, true);            // PCM chunk size
  view.setUint16(20, 1, true);             // PCM format
  view.setUint16(22, 1, true);             // mono
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2, true); // byte rate
  view.setUint16(32, 2, true);             // block align
  view.setUint16(34, 16, true);            // bits per sample
  writeStr(36, 'data');
  view.setUint32(40, dataBytes, true);

  let offset = 44;
  for (const chunk of chunks) {
    for (let i = 0; i < chunk.length; i++, offset += 2) {
      view.setInt16(offset, chunk[i], true);
    }
  }

  return new Blob([buffer], { type: 'audio/wav' });
}

const RecordButton: React.FC<RecordButtonProps> = ({ disabled, onTranscription, onError, modelsData, runPreFlight }) => {
  // Find the first downloaded whisper model, falling back to any whisper model
  const whisperModel = Object.entries(modelsData).find(
    ([, info]) => info.recipe === 'whispercpp' && info.downloaded
  )?.[0] ?? Object.keys(modelsData).find(
    (name) => modelsData[name].recipe === 'whispercpp'
  );

  const [isRecording, setIsRecording] = useState(false);
  const [isTranscribing, setIsTranscribing] = useState(false);

  // AudioContext-based capture — same approach as useAudioCapture hook.
  // Produces 16kHz mono PCM16 which whisper.cpp requires (audio/webm is not supported).
  const audioContextRef = useRef<AudioContext | null>(null);
  const processorRef = useRef<ScriptProcessorNode | null>(null);
  const sourceRef = useRef<MediaStreamAudioSourceNode | null>(null);
  const streamRef = useRef<MediaStream | null>(null);
  const pcmChunksRef = useRef<Int16Array[]>([]);

  const startRecording = async () => {
    try {
      // getUserMedia triggers the browser/OS microphone permission prompt
      const stream = await navigator.mediaDevices.getUserMedia({
        audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true },
      });
      streamRef.current = stream;

      const audioContext = new AudioContext();
      audioContextRef.current = audioContext;
      const nativeRate = audioContext.sampleRate;

      const source = audioContext.createMediaStreamSource(stream);
      sourceRef.current = source;

      const processor = audioContext.createScriptProcessor(4096, 1, 1);
      processorRef.current = processor;
      pcmChunksRef.current = [];

      processor.onaudioprocess = (e) => {
        const inputData = e.inputBuffer.getChannelData(0);
        // Downsample from native rate to 16kHz via linear interpolation
        const ratio = nativeRate / 16000;
        const outputLength = Math.floor(inputData.length / ratio);
        const int16 = new Int16Array(outputLength);
        for (let i = 0; i < outputLength; i++) {
          const srcIdx = i * ratio;
          const floor = Math.floor(srcIdx);
          const ceil = Math.min(floor + 1, inputData.length - 1);
          const frac = srcIdx - floor;
          const sample = inputData[floor] * (1 - frac) + inputData[ceil] * frac;
          const s = Math.max(-1, Math.min(1, sample));
          int16[i] = s < 0 ? s * 0x8000 : s * 0x7FFF;
        }
        pcmChunksRef.current.push(int16);
      };

      // Must connect to destination (muted) for ScriptProcessorNode to fire
      const mute = audioContext.createGain();
      mute.gain.value = 0;
      source.connect(processor);
      processor.connect(mute);
      mute.connect(audioContext.destination);

      setIsRecording(true);
    } catch (err: any) {
      console.error('Microphone access error:', err);
      if (err?.name === 'NotAllowedError' || err?.name === 'PermissionDeniedError') {
        onError('Microphone permission denied. Please allow microphone access and try again.');
      } else {
        onError(`Unable to access microphone: ${err?.message || err}`);
      }
    }
  };

  const stopRecording = () => {
    processorRef.current?.disconnect();
    if (processorRef.current) processorRef.current.onaudioprocess = null;
    sourceRef.current?.disconnect();
    audioContextRef.current?.close().catch(console.error);
    streamRef.current?.getTracks().forEach(t => t.stop());

    processorRef.current = null;
    sourceRef.current = null;
    audioContextRef.current = null;
    streamRef.current = null;

    setIsRecording(false);
    setIsTranscribing(true);

    const wavBlob = buildWavBlob(pcmChunksRef.current);
    pcmChunksRef.current = [];
    handleTranscribe(wavBlob);
  };

  const handleTranscribe = async (wavBlob: Blob) => {
    if (!whisperModel) {
      onError('No Whisper model available. Pull a Whisper model (e.g. Whisper-Base) from the Model Manager first.');
      setIsTranscribing(false);
      return;
    }

    try {
      const ready = await runPreFlight('transcription', {
        modelName: whisperModel,
        modelsData,
        onError,
      });
      if (!ready) {
        setIsTranscribing(false);
        return;
      }

      const formData = new FormData();
      formData.append('file', wavBlob, 'recording.wav');
      formData.append('model', whisperModel);

      const response = await serverFetch('/audio/transcriptions', {
        method: 'POST',
        body: formData,
      });

      if (!response.ok) {
        const body = await response.text().catch(() => '');
        throw new Error(`HTTP error! status: ${response.status}${body ? ` — ${body}` : ''}`);
      }

      const data = await response.json();
      if (data.text) {
        onTranscription(data.text);
      } else {
        throw new Error('Unexpected response format');
      }
    } catch (error: any) {
      console.error('Failed to transcribe:', error);
      onError(`Transcription failed: ${error.message || 'Unknown error'}`);
    } finally {
      setIsTranscribing(false);
    }
  };

  const toggleRecording = () => {
    if (isRecording) {
      stopRecording();
    } else {
      startRecording();
    }
  };

  return (
    <button
      className={`chat-action-button ${isRecording ? 'recording' : ''}`}
      onClick={toggleRecording}
      disabled={disabled || isTranscribing}
      title={!whisperModel ? 'No Whisper model available' : isRecording ? 'Stop recording...' : isTranscribing ? 'Transcribing...' : 'Record voice (auto-loads Whisper)'}
      style={{
        background: 'none',
        border: 'none',
        cursor: (disabled || isTranscribing) ? 'default' : 'pointer',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        padding: '5px',
        color: isRecording ? '#ef4444' : 'inherit',
        opacity: (disabled && !isTranscribing) ? 0.5 : 1,
      }}
    >
      {isTranscribing ? <LoadingIcon /> : <MicrophoneIcon active={isRecording} />}
    </button>
  );
};

export default RecordButton;
