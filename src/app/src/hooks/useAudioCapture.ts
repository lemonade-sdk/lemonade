import { useState, useRef, useCallback } from 'react';

/**
 * Capture microphone audio and emit 16 kHz mono PCM16 chunks encoded as base64.
 * Lemonade's realtime transcription WebSocket expects this exact format.
 */
export function useAudioCapture(
  onAudioChunk: (base64: string) => void,
  onAudioLevel?: (level: number) => void,
) {
  const [isRecording, setIsRecording] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const audioContextRef = useRef<AudioContext | null>(null);
  const sourceRef = useRef<MediaStreamAudioSourceNode | null>(null);
  const processorRef = useRef<ScriptProcessorNode | null>(null);
  const muteRef = useRef<GainNode | null>(null);
  const streamRef = useRef<MediaStream | null>(null);

  const stopRecording = useCallback(() => {
    if (processorRef.current) {
      processorRef.current.disconnect();
      processorRef.current.onaudioprocess = null;
      processorRef.current = null;
    }
    if (sourceRef.current) {
      sourceRef.current.disconnect();
      sourceRef.current = null;
    }
    if (muteRef.current) {
      muteRef.current.disconnect();
      muteRef.current = null;
    }
    if (audioContextRef.current) {
      audioContextRef.current.close().catch(() => {});
      audioContextRef.current = null;
    }
    if (streamRef.current) {
      streamRef.current.getTracks().forEach(track => track.stop());
      streamRef.current = null;
    }
    setIsRecording(false);
  }, []);

  const startRecording = useCallback(async () => {
    try {
      setError(null);
      stopRecording();

      if (!navigator.mediaDevices?.getUserMedia) {
        throw new Error('Microphone capture is not available in this browser.');
      }

      const stream = await navigator.mediaDevices.getUserMedia({
        audio: {
          channelCount: 1,
          echoCancellation: true,
          noiseSuppression: true,
        },
      });
      streamRef.current = stream;

      const audioContext = new AudioContext();
      audioContextRef.current = audioContext;
      const nativeRate = audioContext.sampleRate;
      const source = audioContext.createMediaStreamSource(stream);
      sourceRef.current = source;

      // 4096 samples at 48 kHz is ~85 ms, matching Lemonade realtime guidance.
      const processor = audioContext.createScriptProcessor(4096, 1, 1);
      processorRef.current = processor;

      processor.onaudioprocess = event => {
        const input = event.inputBuffer.getChannelData(0);
        const targetRate = 16000;
        const ratio = nativeRate / targetRate;
        const outputLength = Math.max(1, Math.floor(input.length / ratio));
        const pcm16 = new Int16Array(outputLength);
        let sumSquares = 0;

        for (let i = 0; i < outputLength; i++) {
          const srcIdx = i * ratio;
          const floor = Math.floor(srcIdx);
          const ceil = Math.min(floor + 1, input.length - 1);
          const frac = srcIdx - floor;
          const sample = input[floor] * (1 - frac) + input[ceil] * frac;
          sumSquares += sample * sample;
          const clamped = Math.max(-1, Math.min(1, sample));
          pcm16[i] = clamped < 0 ? clamped * 0x8000 : clamped * 0x7fff;
        }

        if (onAudioLevel) {
          const rms = Math.sqrt(sumSquares / outputLength);
          const db = rms > 0 ? 20 * Math.log10(rms) : -60;
          const clampedDb = Math.max(-60, Math.min(-6, db));
          onAudioLevel((clampedDb + 60) / 54);
        }

        onAudioChunk(arrayBufferToBase64(pcm16.buffer));
      };

      source.connect(processor);
      const mute = audioContext.createGain();
      mute.gain.value = 0;
      muteRef.current = mute;
      processor.connect(mute);
      mute.connect(audioContext.destination);
      setIsRecording(true);
    } catch (err) {
      stopRecording();
      let message = 'Failed to access microphone.';
      if (err instanceof Error) {
        message = err.name === 'NotAllowedError'
          ? 'Microphone access denied. Allow microphone permission for this site, then try again.'
          : err.message;
      }
      setError(message);
      throw new Error(message);
    }
  }, [onAudioChunk, onAudioLevel, stopRecording]);

  return { isRecording, startRecording, stopRecording, error };
}

function arrayBufferToBase64(buffer: ArrayBuffer): string {
  const bytes = new Uint8Array(buffer);
  let binary = '';
  for (let i = 0; i < bytes.byteLength; i++) binary += String.fromCharCode(bytes[i]);
  return btoa(binary);
}

export default useAudioCapture;
