/**
 * WebSocket client for realtime transcription.
 * Uses a raw WebSocket with OpenAI Realtime API message format.
 */

export interface TranscriptionCallbacks {
  /** Called with transcription text. isFinal=false for interim results that replace previous interim. */
  onTranscription: (text: string, isFinal: boolean) => void;
  onSpeechEvent: (event: 'started' | 'stopped') => void;
  onError?: (error: string) => void;
  onConnected?: () => void;
  onDisconnected?: () => void;
}

export class TranscriptionWebSocket {
  private socket: WebSocket;
  private wsPort: number;

  /**
   * Create a new TranscriptionWebSocket.
   * Use the static connect() method instead of calling this directly.
   */
  private constructor(wsPort: number, model: string, callbacks: TranscriptionCallbacks) {
    this.wsPort = wsPort;
    const wsUrl = `ws://localhost:${wsPort}/realtime?model=${encodeURIComponent(model)}`;

    console.log('[WebSocket] Connecting to:', wsUrl);

    // Use raw WebSocket without subprotocols
    this.socket = new WebSocket(wsUrl);

    this.socket.addEventListener('open', () => {
      console.log('[WebSocket] Connection opened');
      // Send session.update with model (server sends session.created automatically)
      this.send({
        type: 'session.update',
        session: { model },
      });
    });

    this.socket.addEventListener('message', (event) => {
      try {
        const msg = JSON.parse(event.data);
        console.log('[WebSocket] Received:', msg.type);

        switch (msg.type) {
          case 'session.created':
            callbacks.onConnected?.();
            break;
          case 'session.updated':
            // Session config updated, nothing to do
            break;
          case 'input_audio_buffer.speech_started':
            callbacks.onSpeechEvent('started');
            break;
          case 'input_audio_buffer.speech_stopped':
            callbacks.onSpeechEvent('stopped');
            break;
          case 'conversation.item.input_audio_transcription.delta':
            // Interim result - replaces previous interim text
            if (typeof msg.delta === 'string') {
              callbacks.onTranscription(msg.delta, false);
            }
            break;
          case 'conversation.item.input_audio_transcription.completed':
            // Final result for this utterance
            if (typeof msg.transcript === 'string') {
              callbacks.onTranscription(msg.transcript, true);
            }
            break;
          case 'error':
            callbacks.onError?.(msg.error?.message || 'Server error');
            break;
        }
      } catch (e) {
        console.error('[WebSocket] Failed to parse message:', e);
      }
    });

    this.socket.addEventListener('error', (event) => {
      console.error('[WebSocket] Error event:', event);
      callbacks.onError?.('WebSocket error');
    });

    this.socket.addEventListener('close', (ev) => {
      console.log('[WebSocket] Close event:', { code: ev.code, reason: ev.reason });
      if (ev.code !== 1000) {
        callbacks.onError?.(
          `WebSocket closed (code=${ev.code}). Is the server running on port ${this.wsPort}?`,
        );
      }
      callbacks.onDisconnected?.();
    });
  }

  /**
   * Connect to the realtime transcription WebSocket.
   * Fetches the WebSocket port from /health endpoint.
   */
  static async connect(
    serverUrl: string,
    model: string,
    callbacks: TranscriptionCallbacks,
  ): Promise<TranscriptionWebSocket> {
    // Fetch WebSocket port from /health endpoint
    const healthUrl = `${serverUrl}/api/v1/health`;
    console.log('[WebSocket] Fetching WebSocket port from:', healthUrl);

    const response = await fetch(healthUrl);
    if (!response.ok) {
      throw new Error(`Failed to fetch health: ${response.status}`);
    }

    const health = await response.json();
    const wsPort = health.websocket_port;

    if (typeof wsPort !== 'number') {
      throw new Error('Server did not provide websocket_port in /health response');
    }

    console.log('[WebSocket] Got WebSocket port:', wsPort);
    return new TranscriptionWebSocket(wsPort, model, callbacks);
  }

  private send(msg: object) {
    if (this.socket.readyState === WebSocket.OPEN) {
      this.socket.send(JSON.stringify(msg));
    }
  }

  sendAudio(base64Audio: string) {
    this.send({ type: 'input_audio_buffer.append', audio: base64Audio });
  }

  commitAudio() {
    this.send({ type: 'input_audio_buffer.commit' });
  }

  clearAudio() {
    this.send({ type: 'input_audio_buffer.clear' });
  }

  isConnected(): boolean {
    return this.socket.readyState === WebSocket.OPEN;
  }

  close() {
    this.socket.close(1000, 'OK');
  }
}

export default TranscriptionWebSocket;
