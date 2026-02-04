/**
 * WebSocket client for realtime transcription.
 * Thin wrapper around the OpenAI SDK's OpenAIRealtimeWebSocket.
 */

import OpenAI from 'openai';
import { OpenAIRealtimeWebSocket } from 'openai/beta/realtime/websocket';

export interface TranscriptionCallbacks {
  onTranscription: (text: string) => void;
  onSpeechEvent: (event: 'started' | 'stopped') => void;
  onError?: (error: string) => void;
  onConnected?: () => void;
  onDisconnected?: () => void;
}

export class TranscriptionWebSocket {
  private rt: OpenAIRealtimeWebSocket;

  constructor(
    serverUrl: string,
    model: string,
    callbacks: TranscriptionCallbacks,
  ) {
    // Convert http://localhost:8000 -> ws port (port + 100)
    const wsPort = this.getWsPort(serverUrl);

    const client = new OpenAI({
      apiKey: 'local',
      baseURL: `http://localhost:${wsPort}`,
      dangerouslyAllowBrowser: true,
    });

    this.rt = new OpenAIRealtimeWebSocket(
      {
        model,
        dangerouslyAllowBrowser: true,
        // The SDK forces wss:// but our local server uses plain ws://
        onURL: (url) => {
          url.protocol = 'ws:';
        },
      },
      client,
    );

    // Wire up events
    this.rt.on('session.created', () => callbacks.onConnected?.());
    this.rt.on('input_audio_buffer.speech_started', () =>
      callbacks.onSpeechEvent('started'),
    );
    this.rt.on('input_audio_buffer.speech_stopped', () =>
      callbacks.onSpeechEvent('stopped'),
    );
    this.rt.on(
      'conversation.item.input_audio_transcription.completed',
      (e) => {
        if (typeof e.transcript === 'string') {
          callbacks.onTranscription(e.transcript);
        }
      },
    );
    this.rt.on('error', (e) =>
      callbacks.onError?.(e.message || 'Unknown error'),
    );
    this.rt.socket.addEventListener('close', () =>
      callbacks.onDisconnected?.(),
    );

    // Send session.update with model once the socket is open
    this.rt.socket.addEventListener('open', () => {
      this.rt.send({
        type: 'session.update',
        session: { model: model as any },
      });
    });
  }

  private getWsPort(httpUrl: string): number {
    const match = httpUrl.match(/:(\d+)/);
    return match ? parseInt(match[1], 10) + 100 : 8100;
  }

  sendAudio(base64Audio: string) {
    this.rt.send({ type: 'input_audio_buffer.append', audio: base64Audio });
  }

  commitAudio() {
    this.rt.send({ type: 'input_audio_buffer.commit' });
  }

  clearAudio() {
    this.rt.send({ type: 'input_audio_buffer.clear' });
  }

  isConnected(): boolean {
    return this.rt.socket.readyState === WebSocket.OPEN;
  }

  close() {
    this.rt.close();
  }
}

export default TranscriptionWebSocket;
