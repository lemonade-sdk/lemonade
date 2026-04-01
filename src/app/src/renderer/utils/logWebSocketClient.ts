import { getAPIKey, getServerHost, serverFetch } from './serverConfig';

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
  // Get websocket port from health endpoint
  const healthResponse = await serverFetch('/health');
  if (!healthResponse.ok) {
    throw new Error(`Failed to fetch health: ${healthResponse.status}`);
  }
  const health = await healthResponse.json();
  const wsPort = health.websocket_port;
  if (typeof wsPort !== 'number') {
    throw new Error('Server did not advertise a websocket port');
  }

  const query = new URLSearchParams();
  const apiKey = getAPIKey();
  if (apiKey) {
    query.set('api_key', apiKey);
  }

  const wsUrl = query.size > 0
    ? `ws://${getServerHost()}:${wsPort}/logs/stream?${query.toString()}`
    : `ws://${getServerHost()}:${wsPort}/logs/stream`;
  const socket = new WebSocket(wsUrl);

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
