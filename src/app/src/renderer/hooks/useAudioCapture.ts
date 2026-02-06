import { useState, useRef, useCallback } from 'react';

/**
 * Hook for capturing microphone audio using Web Audio API.
 * Captures audio and downsamples to 16kHz mono PCM16 format suitable for Whisper.
 */
export function useAudioCapture(
  onAudioChunk: (base64: string) => void,
  onAudioLevel?: (level: number) => void
) {
  const [isRecording, setIsRecording] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const audioContextRef = useRef<AudioContext | null>(null);
  const sourceRef = useRef<MediaStreamAudioSourceNode | null>(null);
  const processorRef = useRef<ScriptProcessorNode | null>(null);
  const streamRef = useRef<MediaStream | null>(null);

  const startRecording = useCallback(async () => {
    try {
      setError(null);

      // Request microphone access — don't force sampleRate, let the
      // system use its native rate (typically 44100 or 48000)
      const stream = await navigator.mediaDevices.getUserMedia({
        audio: {
          channelCount: 1,
          echoCancellation: true,
          noiseSuppression: true,
        },
      });

      streamRef.current = stream;

      // Create audio context at the system's native sample rate
      const audioContext = new AudioContext();
      audioContextRef.current = audioContext;

      const nativeRate = audioContext.sampleRate;
      console.log('[useAudioCapture] Native sample rate:', nativeRate);

      // Create source from microphone stream
      const source = audioContext.createMediaStreamSource(stream);
      sourceRef.current = source;

      // Use ScriptProcessorNode for raw PCM access
      // Buffer size of 4096 at 48kHz ≈ 85ms, at 44.1kHz ≈ 93ms
      const processor = audioContext.createScriptProcessor(4096, 1, 1);
      processorRef.current = processor;

      processor.onaudioprocess = (e) => {
        const inputData = e.inputBuffer.getChannelData(0);

        // Downsample from native rate to 16kHz
        const targetRate = 16000;
        const ratio = nativeRate / targetRate;
        const outputLength = Math.floor(inputData.length / ratio);
        const int16 = new Int16Array(outputLength);
        let sumSquares = 0;

        for (let i = 0; i < outputLength; i++) {
          // Linear interpolation for downsampling
          const srcIdx = i * ratio;
          const srcIdxFloor = Math.floor(srcIdx);
          const srcIdxCeil = Math.min(srcIdxFloor + 1, inputData.length - 1);
          const frac = srcIdx - srcIdxFloor;
          const sample = inputData[srcIdxFloor] * (1 - frac) + inputData[srcIdxCeil] * frac;

          sumSquares += sample * sample;

          // Clamp and convert to int16
          const s = Math.max(-1, Math.min(1, sample));
          int16[i] = s < 0 ? s * 0x8000 : s * 0x7FFF;
        }

        // Compute RMS level for audio level indicator
        if (onAudioLevel) {
          const rms = Math.sqrt(sumSquares / outputLength);
          const db = rms > 0 ? 20 * Math.log10(rms) : -60;
          const clampedDb = Math.max(-60, Math.min(-6, db));
          const normalized = (clampedDb - (-60)) / ((-6) - (-60));
          onAudioLevel(normalized);
        }

        // Base64 encode the int16 buffer
        const base64 = arrayBufferToBase64(int16.buffer);
        onAudioChunk(base64);
      };

      // Connect: source -> processor -> mute -> destination
      // ScriptProcessorNode must be connected to destination to function,
      // but a zero-gain node prevents mic audio from playing through speakers
      source.connect(processor);
      const muteNode = audioContext.createGain();
      muteNode.gain.value = 0;
      processor.connect(muteNode);
      muteNode.connect(audioContext.destination);

      setIsRecording(true);
    } catch (err) {
      const errorMessage = err instanceof Error ? err.message : 'Failed to access microphone';
      setError(errorMessage);
      console.error('Failed to start recording:', err);
    }
  }, [onAudioChunk, onAudioLevel]);

  const stopRecording = useCallback(() => {
    // Disconnect and clean up
    if (processorRef.current) {
      processorRef.current.disconnect();
      processorRef.current.onaudioprocess = null;
      processorRef.current = null;
    }

    if (sourceRef.current) {
      sourceRef.current.disconnect();
      sourceRef.current = null;
    }

    if (audioContextRef.current) {
      audioContextRef.current.close().catch(console.error);
      audioContextRef.current = null;
    }

    // Stop all tracks in the media stream
    if (streamRef.current) {
      streamRef.current.getTracks().forEach((track) => track.stop());
      streamRef.current = null;
    }

    setIsRecording(false);
  }, []);

  return {
    isRecording,
    startRecording,
    stopRecording,
    error,
  };
}

/**
 * Convert ArrayBuffer to base64 string.
 */
function arrayBufferToBase64(buffer: ArrayBuffer): string {
  const bytes = new Uint8Array(buffer);
  let binary = '';
  for (let i = 0; i < bytes.byteLength; i++) {
    binary += String.fromCharCode(bytes[i]);
  }
  return btoa(binary);
}

export default useAudioCapture;
