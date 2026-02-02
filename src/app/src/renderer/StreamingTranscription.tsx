import React, { useState, useRef, useCallback, useEffect } from 'react';
import { useAudioCapture } from './hooks/useAudioCapture';
import { TranscriptionWebSocket } from './utils/websocketClient';
import { getServerBaseUrl } from './utils/serverConfig';

interface StreamingTranscriptionProps {
  model: string;
  onTranscriptionUpdate?: (text: string) => void;
}

/**
 * Streaming transcription component with microphone capture.
 * Uses WebSocket for real-time audio streaming and VAD-triggered transcription.
 */
const StreamingTranscription: React.FC<StreamingTranscriptionProps> = ({
  model,
  onTranscriptionUpdate,
}) => {
  const [transcript, setTranscript] = useState('');
  const [isSpeaking, setIsSpeaking] = useState(false);
  const [isConnected, setIsConnected] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [audioLevel, setAudioLevel] = useState(0);
  const audioLevelRef = useRef(0);
  const wsRef = useRef<TranscriptionWebSocket | null>(null);

  // Handle incoming audio chunks from microphone
  const handleAudioChunk = useCallback((base64: string) => {
    wsRef.current?.sendAudio(base64);
  }, []);

  // Handle audio level updates with exponential smoothing
  const handleAudioLevel = useCallback((level: number) => {
    const smoothed = audioLevelRef.current * 0.7 + level * 0.3;
    audioLevelRef.current = smoothed;
    setAudioLevel(smoothed);
  }, []);

  const { isRecording, startRecording, stopRecording, error: audioError } =
    useAudioCapture(handleAudioChunk, handleAudioLevel);

  // Handle transcription results
  const handleTranscription = useCallback(
    (text: string) => {
      const trimmedText = text.trim();
      if (trimmedText) {
        setTranscript((prev) => {
          const newTranscript = prev ? `${prev} ${trimmedText}` : trimmedText;
          onTranscriptionUpdate?.(newTranscript);
          return newTranscript;
        });
      }
    },
    [onTranscriptionUpdate]
  );

  // Handle speech events
  const handleSpeechEvent = useCallback((event: 'started' | 'stopped') => {
    setIsSpeaking(event === 'started');
  }, []);

  // Start recording and connect WebSocket
  const handleStart = useCallback(async () => {
    setError(null);
    setTranscript('');

    // Create WebSocket connection
    const serverUrl = getServerBaseUrl();
    wsRef.current = new TranscriptionWebSocket(serverUrl, model, {
      onTranscription: handleTranscription,
      onSpeechEvent: handleSpeechEvent,
      onError: (err) => setError(err),
      onConnected: () => setIsConnected(true),
      onDisconnected: () => setIsConnected(false),
    });

    // Wait a brief moment for WebSocket to connect
    await new Promise((resolve) => setTimeout(resolve, 500));

    // Start audio capture
    await startRecording();
  }, [model, handleTranscription, handleSpeechEvent, startRecording]);

  // Stop recording and close WebSocket
  const handleStop = useCallback(() => {
    stopRecording();

    // Commit any remaining audio to force transcription before closing
    if (wsRef.current) {
      wsRef.current.commitAudio();
      // Give the server time to process and send back the transcription
      setTimeout(() => {
        wsRef.current?.close();
        wsRef.current = null;
        setIsConnected(false);
      }, 3000);
    }

    setIsSpeaking(false);
    setAudioLevel(0);
    audioLevelRef.current = 0;
  }, [stopRecording]);

  // Clear transcript
  const handleClear = useCallback(() => {
    setTranscript('');
    wsRef.current?.clearAudio();
    onTranscriptionUpdate?.('');
  }, [onTranscriptionUpdate]);

  // Cleanup on unmount
  useEffect(() => {
    return () => {
      if (wsRef.current) {
        wsRef.current.close();
      }
    };
  }, []);

  // Update model if it changes while recording
  useEffect(() => {
    if (wsRef.current && isRecording) {
      wsRef.current.updateModel(model);
    }
  }, [model, isRecording]);

  const displayError = error || audioError;

  return (
    <div style={styles.container}>
      {/* Controls */}
      <div style={styles.controls}>
        <button
          onClick={isRecording ? handleStop : handleStart}
          style={{
            ...styles.button,
            ...(isRecording ? styles.stopButton : styles.startButton),
          }}
        >
          {isRecording ? (
            <>
              <span style={styles.icon}>&#x25A0;</span> Stop
            </>
          ) : (
            <>
              <span style={styles.icon}>&#x1F3A4;</span> Start Recording
            </>
          )}
        </button>

        {transcript && (
          <button onClick={handleClear} style={styles.clearButton}>
            Clear
          </button>
        )}
      </div>

      {/* Status indicators */}
      <div style={styles.status}>
        {isRecording && (
          <span style={styles.statusItem}>
            <span
              style={{
                ...styles.indicator,
                backgroundColor: isConnected ? '#4caf50' : '#ff9800',
              }}
            />
            {isConnected ? 'Connected' : 'Connecting...'}
          </span>
        )}

        {isSpeaking && (
          <span style={styles.statusItem}>
            <span style={{ ...styles.indicator, backgroundColor: '#2196f3' }} />
            Speaking...
          </span>
        )}

        {isRecording && (
          <div style={styles.levelMeterContainer}>
            <div style={{
              height: '100%',
              width: `${audioLevel * 100}%`,
              backgroundColor: audioLevel > 0.7 ? '#f44336' : audioLevel > 0.3 ? '#4caf50' : '#555',
              borderRadius: '3px',
              transition: 'width 0.08s ease-out',
            }} />
          </div>
        )}
      </div>

      {/* Error display */}
      {displayError && <div style={styles.error}>{displayError}</div>}

      {/* Transcript display */}
      <div style={styles.transcriptContainer}>
        {transcript ? (
          <div style={styles.transcript}>{transcript}</div>
        ) : (
          <div style={styles.placeholder}>
            {isRecording
              ? 'Listening... Start speaking to see transcription.'
              : 'Click "Start Recording" to begin real-time transcription.'}
          </div>
        )}
      </div>
    </div>
  );
};

const styles: { [key: string]: React.CSSProperties } = {
  container: {
    display: 'flex',
    flexDirection: 'column',
    gap: '12px',
    padding: '16px',
    backgroundColor: 'var(--bg-secondary, #1e1e1e)',
    borderRadius: '8px',
  },
  controls: {
    display: 'flex',
    gap: '8px',
    alignItems: 'center',
  },
  button: {
    display: 'flex',
    alignItems: 'center',
    gap: '6px',
    padding: '10px 16px',
    border: 'none',
    borderRadius: '6px',
    cursor: 'pointer',
    fontSize: '14px',
    fontWeight: 500,
    transition: 'all 0.2s ease',
  },
  startButton: {
    backgroundColor: '#4caf50',
    color: 'white',
  },
  stopButton: {
    backgroundColor: '#f44336',
    color: 'white',
  },
  clearButton: {
    padding: '10px 16px',
    border: '1px solid var(--border-color, #333)',
    borderRadius: '6px',
    backgroundColor: 'transparent',
    color: 'var(--text-secondary, #888)',
    cursor: 'pointer',
    fontSize: '14px',
  },
  icon: {
    fontSize: '16px',
  },
  status: {
    display: 'flex',
    gap: '16px',
    alignItems: 'center',
    minHeight: '24px',
  },
  statusItem: {
    display: 'flex',
    alignItems: 'center',
    gap: '6px',
    fontSize: '13px',
    color: 'var(--text-secondary, #888)',
  },
  indicator: {
    width: '8px',
    height: '8px',
    borderRadius: '50%',
    display: 'inline-block',
  },
  levelMeterContainer: {
    height: '6px',
    backgroundColor: '#121212',
    borderRadius: '3px',
    overflow: 'hidden' as const,
    minWidth: '100px',
    maxWidth: '200px',
    flex: 1,
  },
  error: {
    padding: '8px 12px',
    backgroundColor: 'rgba(244, 67, 54, 0.1)',
    border: '1px solid rgba(244, 67, 54, 0.3)',
    borderRadius: '4px',
    color: '#f44336',
    fontSize: '13px',
  },
  transcriptContainer: {
    flex: 1,
    minHeight: '150px',
    maxHeight: '400px',
    overflowY: 'auto',
    padding: '12px',
    backgroundColor: 'var(--bg-primary, #121212)',
    borderRadius: '6px',
    border: '1px solid var(--border-color, #333)',
  },
  transcript: {
    fontSize: '15px',
    lineHeight: 1.6,
    color: 'var(--text-primary, #fff)',
    whiteSpace: 'pre-wrap',
    wordBreak: 'break-word',
  },
  placeholder: {
    fontSize: '14px',
    color: 'var(--text-secondary, #666)',
    fontStyle: 'italic',
    textAlign: 'center',
    paddingTop: '40px',
  },
};

export default StreamingTranscription;
