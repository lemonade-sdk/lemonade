/**
 * WebSocket client for realtime transcription.
 * Implements OpenAI-compatible Realtime API message protocol.
 */

export interface TranscriptionCallbacks {
  onTranscription: (text: string) => void;
  onSpeechEvent: (event: 'started' | 'stopped') => void;
  onError?: (error: string) => void;
  onConnected?: () => void;
  onDisconnected?: () => void;
}

interface RealtimeMessage {
  type: string;
  [key: string]: unknown;
}

export class TranscriptionWebSocket {
  private ws: WebSocket | null = null;
  private callbacks: TranscriptionCallbacks;
  private model: string;
  private reconnectAttempts = 0;
  private maxReconnectAttempts = 3;
  private isClosing = false;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;

  constructor(
    serverUrl: string,
    model: string,
    callbacks: TranscriptionCallbacks
  ) {
    this.model = model;
    this.callbacks = callbacks;

    // Convert http://localhost:8000 to ws://localhost:8100
    const wsUrl = this.convertToWebSocketUrl(serverUrl);
    this.connect(wsUrl);
  }

  private convertToWebSocketUrl(httpUrl: string): string {
    console.log('[TranscriptionWebSocket] Converting URL:', httpUrl);

    // Replace http(s):// with ws(s)://
    let wsUrl = httpUrl.replace(/^http/, 'ws');

    // Handle port conversion - HTTP port to WebSocket port (port + 100)
    // port + 1 is reserved for backend subprocesses (whisper.cpp, etc.)
    const portMatch = wsUrl.match(/:(\d+)/);
    if (portMatch) {
      const httpPort = parseInt(portMatch[1], 10);
      const wsPort = httpPort + 100;
      wsUrl = wsUrl.replace(`:${httpPort}`, `:${wsPort}`);
    } else {
      // No port specified, assume default 8000 -> 8100
      wsUrl = wsUrl.replace(/(ws:\/\/[^\/]+)/, '$1:8100');
    }

    // Add the realtime endpoint with transcription intent
    if (!wsUrl.endsWith('/')) {
      wsUrl += '/';
    }
    wsUrl += 'api/v1/realtime?intent=transcription';

    console.log('[TranscriptionWebSocket] Final WebSocket URL:', wsUrl);
    return wsUrl;
  }

  private connect(wsUrl: string) {
    try {
      console.log('[TranscriptionWebSocket] Attempting connection to:', wsUrl);
      this.ws = new WebSocket(wsUrl);

      this.ws.onopen = () => {
        console.log('[TranscriptionWebSocket] Connected successfully!');
        this.reconnectAttempts = 0;

        // Send session update with model
        this.sendMessage({
          type: 'transcription_session.update',
          session: { model: this.model },
        });

        this.callbacks.onConnected?.();
      };

      this.ws.onmessage = (event) => {
        this.handleMessage(event.data);
      };

      this.ws.onerror = (event) => {
        console.error('[TranscriptionWebSocket] WebSocket error event:', event);
        console.error('[TranscriptionWebSocket] WebSocket readyState:', this.ws?.readyState);
        console.error('[TranscriptionWebSocket] Target URL was:', wsUrl);
        this.callbacks.onError?.(`WebSocket connection failed to ${wsUrl}`);
      };

      this.ws.onclose = (event) => {
        console.log('[TranscriptionWebSocket] Closed:', event.code, event.reason);
        this.callbacks.onDisconnected?.();

        // Attempt reconnection if not intentionally closed
        if (!this.isClosing && this.reconnectAttempts < this.maxReconnectAttempts) {
          this.reconnectAttempts++;
          console.log(
            `[TranscriptionWebSocket] Reconnecting (attempt ${this.reconnectAttempts}/${this.maxReconnectAttempts})...`
          );
          this.reconnectTimer = setTimeout(() => this.connect(wsUrl), 1000 * this.reconnectAttempts);
        }
      };
    } catch (error) {
      console.error('[TranscriptionWebSocket] Connection failed:', error);
      this.callbacks.onError?.('Failed to connect to WebSocket server');
    }
  }

  private handleMessage(data: string) {
    try {
      const msg: RealtimeMessage = JSON.parse(data);

      switch (msg.type) {
        case 'transcription_session.created':
          console.log('[TranscriptionWebSocket] Session created:', msg.session);
          break;

        case 'transcription_session.updated':
          console.log('[TranscriptionWebSocket] Session updated:', msg.session);
          break;

        case 'input_audio_buffer.speech_started':
          console.log('[TranscriptionWebSocket] Speech started at:', msg.audio_start_ms);
          this.callbacks.onSpeechEvent('started');
          break;

        case 'input_audio_buffer.speech_stopped':
          console.log('[TranscriptionWebSocket] Speech stopped at:', msg.audio_end_ms);
          this.callbacks.onSpeechEvent('stopped');
          break;

        case 'input_audio_buffer.committed':
          console.log('[TranscriptionWebSocket] Audio committed');
          break;

        case 'input_audio_buffer.cleared':
          console.log('[TranscriptionWebSocket] Audio cleared');
          break;

        case 'conversation.item.input_audio_transcription.completed':
          console.log('[TranscriptionWebSocket] Transcription:', msg.transcript);
          if (typeof msg.transcript === 'string') {
            this.callbacks.onTranscription(msg.transcript);
          }
          break;

        case 'error':
          console.error('[TranscriptionWebSocket] Error from server:', msg.error);
          const errorMsg = (msg.error as { message?: string })?.message || 'Unknown error';
          this.callbacks.onError?.(errorMsg);
          break;

        default:
          console.log('[TranscriptionWebSocket] Unknown message type:', msg.type);
      }
    } catch (error) {
      console.error('[TranscriptionWebSocket] Failed to parse message:', error, data);
    }
  }

  private sendMessage(msg: RealtimeMessage) {
    if (this.ws?.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(msg));
    }
  }

  /**
   * Send audio data to the server.
   * @param base64Audio Base64-encoded PCM16 audio data
   */
  sendAudio(base64Audio: string) {
    this.sendMessage({
      type: 'input_audio_buffer.append',
      audio: base64Audio,
    });
  }

  /**
   * Commit the current audio buffer (force transcription).
   */
  commitAudio() {
    this.sendMessage({
      type: 'input_audio_buffer.commit',
    });
  }

  /**
   * Clear the audio buffer without transcribing.
   */
  clearAudio() {
    this.sendMessage({
      type: 'input_audio_buffer.clear',
    });
  }

  /**
   * Update the transcription model.
   */
  updateModel(model: string) {
    this.model = model;
    this.sendMessage({
      type: 'transcription_session.update',
      session: { model },
    });
  }

  /**
   * Check if the WebSocket is connected.
   */
  isConnected(): boolean {
    return this.ws?.readyState === WebSocket.OPEN;
  }

  /**
   * Close the WebSocket connection.
   */
  close() {
    this.isClosing = true;
    if (this.reconnectTimer !== null) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
  }
}

export default TranscriptionWebSocket;
