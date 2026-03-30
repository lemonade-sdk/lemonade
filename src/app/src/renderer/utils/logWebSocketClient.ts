import { serverFetch } from './serverConfig';

export interface LogEntry {
  seq: number;
  timestamp: string;
  severity: string;
  tag: string;
  line: string;
}

interface SnapshotMessage {
  type: 'logs.snapshot';
  entries: LogEntry[];
}

interface EntryMessage {
  type: 'logs.entry';
  entry: LogEntry;
}

interface ErrorMessage {
  type: 'error';
  error?: {
    message?: string;
  };
}

type ServerMessage = SnapshotMessage | EntryMessage | ErrorMessage;

export interface LogWebSocketCallbacks {
  onConnected?: () => void;
  onDisconnected?: () => void;
  onError?: (message: string) => void;
  onSnapshot: (entries: LogEntry[]) => void;
  onEntry: (entry: LogEntry) => void;
}

export class LogWebSocketClient {
  private socket: WebSocket;

  private constructor(socket: WebSocket) {
    this.socket = socket;
  }

  static async connect(
    afterSeq: number | null,
    callbacks: LogWebSocketCallbacks,
  ): Promise<LogWebSocketClient> {
    const response = await serverFetch('/logs/stream/ticket', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
      },
      body: JSON.stringify({
        after_seq: afterSeq,
      }),
    });

    if (!response.ok) {
      throw new Error(`Failed to create log stream ticket: ${response.status}`);
    }

    const ticket = await response.json();
    if (typeof ticket.ws_url !== 'string' || ticket.ws_url.length === 0) {
      throw new Error('Server did not return a log websocket URL');
    }

    const socket = new WebSocket(ticket.ws_url);
    const client = new LogWebSocketClient(socket);

    socket.addEventListener('open', () => {
      callbacks.onConnected?.();
    });

    socket.addEventListener('message', (event) => {
      try {
        const message = JSON.parse(event.data) as ServerMessage;

        if (message.type === 'logs.snapshot') {
          callbacks.onSnapshot(message.entries ?? []);
          return;
        }

        if (message.type === 'logs.entry' && message.entry) {
          callbacks.onEntry(message.entry);
          return;
        }

        if (message.type === 'error') {
          callbacks.onError?.(message.error?.message || 'Server error');
        }
      } catch (error) {
        callbacks.onError?.(`Invalid log stream payload: ${String(error)}`);
      }
    });

    socket.addEventListener('error', () => {
      callbacks.onError?.('WebSocket error');
    });

    socket.addEventListener('close', () => {
      callbacks.onDisconnected?.();
    });

    return client;
  }

  close() {
    this.socket.close(1000, 'OK');
  }
}

export default LogWebSocketClient;
