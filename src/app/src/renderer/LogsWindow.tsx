import React, { useEffect, useRef, useState } from 'react';
import { getServerBaseUrl, onServerUrlChange, serverConfig } from './utils/serverConfig';
import LogWebSocketClient, { LogEntry } from './utils/logWebSocketClient';

interface LogsWindowProps {
  isVisible: boolean;
  height?: number;
}

const BOTTOM_FOLLOW_THRESHOLD_PX = 60;

const LogsWindow: React.FC<LogsWindowProps> = ({ isVisible, height }) => {
  const [logs, setLogs] = useState<LogEntry[]>([]);
  const [connectionStatus, setConnectionStatus] = useState<'connecting' | 'connected' | 'error' | 'disconnected'>('connecting');
  const [autoScroll, setAutoScroll] = useState(true);
  const logsEndRef = useRef<HTMLDivElement>(null);
  const logsContentRef = useRef<HTMLDivElement>(null);
  const autoScrollRef = useRef(true);
  const isProgrammaticScrollRef = useRef(false);
  const lastSeqRef = useRef<number | null>(null);
  const socketRef = useRef<LogWebSocketClient | null>(null);
  const reconnectTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const [serverUrl, setServerUrl] = useState<string>('');
  const [isInitialized, setIsInitialized] = useState(false);

  const isNearBottom = () => {
    const logsContent = logsContentRef.current;
    if (!logsContent) return true;
    return (
      logsContent.scrollHeight - logsContent.scrollTop <=
      logsContent.clientHeight + BOTTOM_FOLLOW_THRESHOLD_PX
    );
  };

  const scrollToBottom = () => {
    if (!logsEndRef.current) return;

    isProgrammaticScrollRef.current = true;
    logsEndRef.current.scrollIntoView({ behavior: 'auto', block: 'end' });

    // Keep programmatic-scroll guard through the next paint.
    requestAnimationFrame(() => {
      isProgrammaticScrollRef.current = false;
    });
  };

  // Wait for serverConfig to initialize and get the correct URL
  useEffect(() => {
    serverConfig.waitForInit().then(() => {
      setServerUrl(getServerBaseUrl());
      setIsInitialized(true);
    });
  }, []);

  // Listen for URL changes (covers both port changes and explicit URL updates)
  useEffect(() => {
    const unsubscribe = onServerUrlChange((newUrl: string) => {
      console.log('Server URL changed, updating logs URL:', newUrl);
      setServerUrl(newUrl);
    });

    return () => {
      unsubscribe();
    };
  }, []);

  // Auto-scroll to bottom when new logs arrive (if auto-scroll is enabled)
  useEffect(() => {
    if (autoScroll) {
      scrollToBottom();
    }
  }, [logs, autoScroll]);

  useEffect(() => {
    autoScrollRef.current = autoScroll;
  }, [autoScroll]);

  // Detect if user scrolls up (disable auto-scroll) or scrolls to bottom (enable auto-scroll)
  useEffect(() => {
    const logsContent = logsContentRef.current;
    if (!logsContent) return;

    const handleScroll = () => {
      if (isProgrammaticScrollRef.current) {
        return;
      }

      const isAtBottom = isNearBottom();
      setAutoScroll((prev) => (prev === isAtBottom ? prev : isAtBottom));
    };

    logsContent.addEventListener('scroll', handleScroll);
    return () => logsContent.removeEventListener('scroll', handleScroll);
  }, []);

  const appendEntries = (incomingEntries: LogEntry[]) => {
    if (incomingEntries.length === 0) {
      return;
    }

    const shouldFollowNextLine = autoScrollRef.current || isNearBottom();
    if (shouldFollowNextLine && !autoScrollRef.current) {
      setAutoScroll(true);
    }

    setLogs((prevLogs) => {
      const nextLogs = [...prevLogs];
      const seenSeq = new Set(prevLogs.map((entry) => entry.seq));

      for (const entry of incomingEntries) {
        if (!seenSeq.has(entry.seq)) {
          nextLogs.push(entry);
          seenSeq.add(entry.seq);
          lastSeqRef.current = entry.seq;
        }
      }

      nextLogs.sort((a, b) => a.seq - b.seq);
      return nextLogs.length > 1000 ? nextLogs.slice(-1000) : nextLogs;
    });
  };

  // Connect to websocket log stream
  useEffect(() => {
    if (!isVisible || !isInitialized || !serverUrl) {
      if (socketRef.current) {
        socketRef.current.close();
        socketRef.current = null;
      }
      if (reconnectTimeoutRef.current) {
        clearTimeout(reconnectTimeoutRef.current);
        reconnectTimeoutRef.current = null;
      }
      return;
    }

    const connectToLogStream = () => {
      try {
        setConnectionStatus('connecting');

        if (socketRef.current) {
          socketRef.current.close();
          socketRef.current = null;
        }

        LogWebSocketClient.connect(lastSeqRef.current, {
          onConnected: () => {
            console.log('Log stream connected to:', serverUrl);
            setConnectionStatus('connected');
          },
          onDisconnected: () => {
            if (isVisible) {
              setConnectionStatus('error');
              reconnectTimeoutRef.current = setTimeout(() => {
                console.log('Attempting to reconnect to log stream...');
                connectToLogStream();
              }, 5000);
            } else {
              setConnectionStatus('disconnected');
            }
          },
          onError: (message) => {
            console.error('Log stream error:', message);
            setConnectionStatus('error');
          },
          onSnapshot: (entries) => {
            appendEntries(entries);
          },
          onEntry: (entry) => {
            appendEntries([entry]);
          },
        }).then((client) => {
          socketRef.current = client;
        }).catch((error) => {
          console.error('Failed to connect to log stream:', error);
          setConnectionStatus('error');

          reconnectTimeoutRef.current = setTimeout(() => {
            console.log('Attempting to reconnect to log stream...');
            connectToLogStream();
          }, 5000);
        });
      } catch (error) {
        console.error('Failed to connect to log stream:', error);
        setConnectionStatus('error');

        reconnectTimeoutRef.current = setTimeout(() => {
          connectToLogStream();
        }, 5000);
      }
    };

    // Initial connection
    connectToLogStream();

    // Cleanup on unmount or when visibility changes
    return () => {
      if (socketRef.current) {
        socketRef.current.close();
        socketRef.current = null;
      }
      if (reconnectTimeoutRef.current) {
        clearTimeout(reconnectTimeoutRef.current);
        reconnectTimeoutRef.current = null;
      }
    };
  }, [isVisible, serverUrl, isInitialized]);

  const handleClearLogs = () => {
    setLogs([]);
    lastSeqRef.current = null;
  };

  const handleScrollToBottom = () => {
    setAutoScroll(true);
    scrollToBottom();
  };

  if (!isVisible) return null;

  return (
    <div className="logs-window" style={height ? { height: `${height}px`, flex: 'none' } : undefined}>
      <div className="logs-header">
        <h3>Server Logs</h3>
        <div className="logs-controls">
          <span className={`connection-status status-${connectionStatus}`}>
            {connectionStatus === 'connecting' && '⟳ Connecting...'}
            {connectionStatus === 'connected' && '● Connected'}
            {connectionStatus === 'error' && '⚠ Error (Reconnecting...)'}
            {connectionStatus === 'disconnected' && '○ Disconnected'}
          </span>
          {!autoScroll && (
            <button className="logs-control-btn" onClick={handleScrollToBottom} title="Scroll to bottom">
              ↓ Jump to Bottom
            </button>
          )}
          <button className="logs-control-btn" onClick={handleClearLogs} title="Clear logs">
            Clear
          </button>
        </div>
      </div>
      <div className="logs-content" ref={logsContentRef}>
        {logs.length === 0 && connectionStatus === 'connected' && (
          <div className="logs-placeholder">Waiting for logs...</div>
        )}
        {logs.length === 0 && connectionStatus === 'error' && (
          <div className="logs-error">
            Failed to connect to lemonade-server logs.
            <br />
            Make sure the server is running on {serverUrl}
          </div>
        )}
        <pre className="logs-text">
          {logs.map((log, index) => (
            <div key={`${log.seq}-${index}`} className="log-line">
              {log.line}
            </div>
          ))}
          <div ref={logsEndRef} />
        </pre>
      </div>
    </div>
  );
};

export default LogsWindow;
