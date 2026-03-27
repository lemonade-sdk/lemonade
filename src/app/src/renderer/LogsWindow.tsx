import React, { useEffect, useRef, useState } from 'react';
import { getAPIKey, getServerBaseUrl, onServerUrlChange, serverConfig } from './utils/serverConfig';

interface LogsWindowProps {
  isVisible: boolean;
  height?: number;
}

interface LogStreamConnection {
  close: () => void;
}

const BOTTOM_FOLLOW_THRESHOLD_PX = 60;

const LogsWindow: React.FC<LogsWindowProps> = ({ isVisible, height }) => {
  const [logs, setLogs] = useState<string[]>([]);
  const [connectionStatus, setConnectionStatus] = useState<'connecting' | 'connected' | 'error' | 'disconnected'>('connecting');
  const [autoScroll, setAutoScroll] = useState(true);
  const logsEndRef = useRef<HTMLDivElement>(null);
  const logsContentRef = useRef<HTMLDivElement>(null);
  const autoScrollRef = useRef(true);
  const isProgrammaticScrollRef = useRef(false);
  const eventSourceRef = useRef<LogStreamConnection | null>(null);
  const reconnectTimeoutRef = useRef<NodeJS.Timeout | null>(null);
  const [serverUrl, setServerUrl] = useState<string>('');
  const [apiKey, setAPIKey] = useState<string>('');
  const [isInitialized, setIsInitialized] = useState(false);
  const [reconnectSignal, setReconnectSignal] = useState(0);

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
      setAPIKey(getAPIKey());
      setIsInitialized(true);
    });
  }, []);

  // Listen for URL changes (covers both port changes and explicit URL updates)
  useEffect(() => {
    const unsubscribe = onServerUrlChange((newUrl: string, newAPIKey: string) => {
      console.log('Server URL changed, updating logs URL:', newUrl);
      setServerUrl(newUrl);
      setAPIKey(newAPIKey);
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

  // Connect to SSE log stream
  useEffect(() => {
    // Don't connect until we have the correct URL from initialization
    if (!isVisible || !isInitialized || !serverUrl) {
      // Clean up connection when logs window is hidden or not ready
      if (eventSourceRef.current) {
        eventSourceRef.current.close();
        eventSourceRef.current = null;
      }
      if (reconnectTimeoutRef.current) {
        clearTimeout(reconnectTimeoutRef.current);
        reconnectTimeoutRef.current = null;
      }
      return;
    }

    const connectToLogStream = () => {
      const appendLogLine = (logLine: string) => {
        // Skip heartbeat messages
        if (logLine.trim() === '' || logLine === 'heartbeat') {
          return;
        }

        // Keep follow mode sticky when user is effectively at bottom.
        const shouldFollowNextLine = autoScrollRef.current || isNearBottom();
        if (shouldFollowNextLine && !autoScrollRef.current) {
          setAutoScroll(true);
        }

        setLogs((prevLogs) => {
          // Keep last 1000 lines to prevent memory issues
          const newLogs = [...prevLogs, logLine];
          return newLogs.length > 1000 ? newLogs.slice(-1000) : newLogs;
        });
      };

      try {
        setConnectionStatus('connecting');

        // Close existing connection if any
        if (eventSourceRef.current) {
          eventSourceRef.current.close();
        }

        const streamUrl = `${serverUrl}/api/v1/logs/stream`;

        if (!apiKey) {
          const eventSource = new window.EventSource(streamUrl);
          eventSourceRef.current = eventSource;

          eventSource.onopen = () => {
            console.log('Log stream connected to:', serverUrl);
            setConnectionStatus('connected');
          };

          eventSource.onmessage = (event) => {
            // SSE sends data as "data: <log line>"
            appendLogLine(event.data);
          };

          eventSource.onerror = (error) => {
            console.error('Log stream error:', error);
            setConnectionStatus('error');
            eventSource.close();

            // Reconnect after 5 seconds
            reconnectTimeoutRef.current = setTimeout(() => {
              console.log('Attempting to reconnect to log stream...');
              connectToLogStream();
            }, 5000);
          };
          return;
        }

        const abortController = new AbortController();
        eventSourceRef.current = {
          close: () => abortController.abort(),
        };

        const connectWithFetch = async () => {
          try {
            const response = await fetch(streamUrl, {
              headers: {
                Authorization: `Bearer ${apiKey}`,
              },
              signal: abortController.signal,
            });

            if (!response.ok || !response.body) {
              throw new Error(`Log stream request failed with status ${response.status}`);
            }

            console.log('Log stream connected to:', serverUrl);
            setConnectionStatus('connected');

            const reader = response.body.getReader();
            const decoder = new TextDecoder();
            let buffer = '';

            while (true) {
              const { done, value } = await reader.read();
              if (done) {
                break;
              }

              buffer += decoder.decode(value, { stream: true });

              let delimiterIndex = buffer.indexOf('\n\n');
              while (delimiterIndex !== -1) {
                const rawEvent = buffer.slice(0, delimiterIndex).replace(/\r/g, '');
                buffer = buffer.slice(delimiterIndex + 2);

                const eventLines = rawEvent.split('\n');
                const dataLines: string[] = [];

                for (const line of eventLines) {
                  if (line.startsWith('data:')) {
                    dataLines.push(line.slice(5).trimStart());
                  }
                }

                if (dataLines.length > 0) {
                  appendLogLine(dataLines.join('\n'));
                }

                delimiterIndex = buffer.indexOf('\n\n');
              }
            }

            if (!abortController.signal.aborted) {
              throw new Error('Log stream connection closed');
            }
          } catch (error) {
            if (abortController.signal.aborted) {
              return;
            }

            console.error('Log stream error:', error);
            setConnectionStatus('error');
            reconnectTimeoutRef.current = setTimeout(() => {
              console.log('Attempting to reconnect to log stream...');
              connectToLogStream();
            }, 5000);
          }
        };

        void connectWithFetch();
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
      if (eventSourceRef.current) {
        eventSourceRef.current.close();
        eventSourceRef.current = null;
      }
      if (reconnectTimeoutRef.current) {
        clearTimeout(reconnectTimeoutRef.current);
        reconnectTimeoutRef.current = null;
      }
    };
  }, [isVisible, serverUrl, apiKey, isInitialized, reconnectSignal]);

  const handleClearLogs = () => {
    setLogs([]);
  };

  const handleScrollToBottom = () => {
    setAutoScroll(true);
    scrollToBottom();
  };

  const handleRefreshConnection = () => {
    setServerUrl(getServerBaseUrl());
    setAPIKey(getAPIKey());
    setConnectionStatus('connecting');
    setReconnectSignal((prev) => prev + 1);
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
          <button className="logs-control-btn" onClick={handleRefreshConnection} title="Refresh credentials and reconnect">
            Refresh
          </button>
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
            <div key={index} className="log-line">
              {log}
            </div>
          ))}
          <div ref={logsEndRef} />
        </pre>
      </div>
    </div>
  );
};

export default LogsWindow;
