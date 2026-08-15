import { buildWebSocketUrl, webSocketProtocols } from './serverConfig';

export interface LogEntry {
  seq: number;
  timestamp: string;
  severity: string;
  tag: string;
  line: string;
}

export interface LogStreamHandle {
  close: () => void;
}

export interface LogStreamCallbacks {
  onConnected?: () => void;
  onDisconnected?: () => void;
  onError?: (message: string) => void;
  onSnapshot: (entries: LogEntry[]) => void;
  onEntry: (entry: LogEntry) => void;
}

export async function connectLogStream(
  afterSeq: number | null,
  callbacks: LogStreamCallbacks,
): Promise<LogStreamHandle> {
  const protocols = await webSocketProtocols();
  const wsUrl = buildWebSocketUrl('/logs/stream');
  const socket = new WebSocket(wsUrl, protocols);

  socket.addEventListener('open', () => {
    socket.send(JSON.stringify({
      type: 'logs.subscribe',
      after_seq: afterSeq,
    }));
    callbacks.onConnected?.();
  });

  socket.addEventListener('message', (event) => {
    try {
      const message = JSON.parse(event.data);
      console.log('[LogStream] Received message type:', message.type, 'entries:', message.entries?.length ?? message.entry?.seq ?? 'n/a');
      if (message.type === 'logs.snapshot') {
        callbacks.onSnapshot(message.entries ?? []);
      } else if (message.type === 'logs.entry' && message.entry) {
        callbacks.onEntry(message.entry);
      } else if (message.type === 'error') {
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

  return { close: () => socket.close(1000, 'OK') };
}
