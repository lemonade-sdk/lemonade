/**
 * WebSocket client for realtime transcription.
 * Uses a raw WebSocket with OpenAI Realtime API message format.
 */
import { buildWebSocketUrl, serverFetch, webSocketProtocols } from './serverConfig';

export interface TranscriptionCallbacks {
  /** Called with transcription text. isFinal=false for interim results that replace previous interim. */
  onTranscription: (text: string, isFinal: boolean) => void;
  onSpeechEvent: (event: 'started' | 'stopped') => void;
  onError?: (error: string) => void;
  onAudioBufferCleared?: () => void;
  onConnected?: () => void;
  onDisconnected?: () => void;
}

export class TranscriptionWebSocket {
  private socket: WebSocket;
  private wsUrl: string;
  // Errors before the socket opens are connection probing (the main-port
  // attempt may fall back to the legacy port) — not user-facing errors
  private opened = false;

  /**
   * Create a new TranscriptionWebSocket.
   * Use the static connect() method instead of calling this directly.
   */
  private constructor(wsUrl: string, model: string, callbacks: TranscriptionCallbacks, protocols?: string[]) {
    this.wsUrl = wsUrl;

    const redactedUrl = wsUrl.replace(/\?.*/, '?<redacted>');
    console.log('[WebSocket] Connecting to:', redactedUrl);

    this.socket = new WebSocket(wsUrl, protocols);

    this.socket.addEventListener('open', () => {
      console.log('[WebSocket] Connection opened');
      this.opened = true;
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
          case 'input_audio_buffer.cleared':
            callbacks.onAudioBufferCleared?.();
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
      if (this.opened) {
        callbacks.onError?.('WebSocket error');
      }
    });

    this.socket.addEventListener('close', (ev) => {
      console.log('[WebSocket] Close event:', { code: ev.code, reason: ev.reason });
      if (!this.opened) {
        return;  // connect() handles pre-open failures (port fallback)
      }
      if (ev.code !== 1000) {
        callbacks.onError?.(
          `WebSocket closed (code=${ev.code}). Is the server reachable at ${this.wsUrl}?`,
        );
      }
      callbacks.onDisconnected?.();
    });
  }

  /** Open a socket and resolve once connected (reject on error/timeout). */
  private static openSocket(
    wsUrl: string,
    model: string,
    callbacks: TranscriptionCallbacks,
    protocols?: string[],
    timeoutMs = 5000,
  ): Promise<TranscriptionWebSocket> {
    return new Promise((resolve, reject) => {
      const client = new TranscriptionWebSocket(wsUrl, model, callbacks, protocols);
      const timer = setTimeout(() => {
        client.socket.close();
        reject(new Error(`WebSocket connect timeout: ${wsUrl}`));
      }, timeoutMs);
      client.socket.addEventListener('open', () => {
        clearTimeout(timer);
        resolve(client);
      });
      client.socket.addEventListener('error', () => {
        clearTimeout(timer);
        reject(new Error(`WebSocket connect failed: ${wsUrl}`));
      });
    });
  }

  /**
   * Connect to the realtime transcription WebSocket.
   *
   * Prefers the main HTTP port (which accepts WebSocket upgrades on
   * /v1/realtime) — one port for everything, and the only option that works
   * through firewalls/proxies that expose just the API port. Falls back to
   * the dedicated websocket_port from /health for older servers.
   */
  static async connect(
    model: string,
    callbacks: TranscriptionCallbacks,
  ): Promise<TranscriptionWebSocket> {
    const query = new URLSearchParams({ model });
    const protocols = await webSocketProtocols();

    const mainUrl = buildWebSocketUrl('/v1/realtime', undefined, query);
    try {
      return await TranscriptionWebSocket.openSocket(mainUrl, model, callbacks, protocols);
    } catch (err) {
      console.warn('[WebSocket] Main-port connect failed, falling back to websocket_port:', err);
    }

    // Legacy: dedicated WebSocket port advertised by /health
    const response = await serverFetch('/health');
    if (!response.ok) {
      throw new Error(`Failed to fetch health: ${response.status}`);
    }
    const health = await response.json();
    const wsPort = health.websocket_port;
    if (typeof wsPort !== 'number') {
      throw new Error('Server did not provide websocket_port in /health response');
    }

    const legacyUrl = buildWebSocketUrl('/realtime', wsPort, query);
    return TranscriptionWebSocket.openSocket(legacyUrl, model, callbacks, protocols);
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
