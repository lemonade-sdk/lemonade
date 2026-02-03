import React, { useEffect, useRef, useState } from 'react';
import { getAPIKey, getServerBaseUrl, onServerUrlChange, serverConfig } from './utils/serverConfig';
import {EventSource} from 'eventsource';

interface LogsWindowProps {
  isVisible: boolean;
  height?: number;
}

const LogsWindow: React.FC<LogsWindowProps> = ({ isVisible, height }) => {
  const [logs, setLogs] = useState<string[]>([]);
  const [connectionStatus, setConnectionStatus] = useState<'connecting' | 'connected' | 'error' | 'disconnected'>('connecting');
  const [autoScroll, setAutoScroll] = useState(true);
  const logsEndRef = useRef<HTMLDivElement>(null);
  const logsContentRef = useRef<HTMLDivElement>(null);
  const eventSourceRef = useRef<EventSource | null>(null);
  const reconnectTimeoutRef = useRef<NodeJS.Timeout | null>(null);
  const [serverUrl, setServerUrl] = useState<string>('');
  const [apiKey, setAPIKey] = useState<string>('');
  const [isInitialized, setIsInitialized] = useState(false);

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
    if (autoScroll && logsEndRef.current) {
      logsEndRef.current.scrollIntoView({ behavior: 'smooth' });
    }
  }, [logs, autoScroll]);

  // Detect if user scrolls up (disable auto-scroll) or scrolls to bottom (enable auto-scroll)
  useEffect(() => {
    const logsContent = logsContentRef.current;
    if (!logsContent) return;

    const handleScroll = () => {
      const isAtBottom =
        logsContent.scrollHeight - logsContent.scrollTop <= logsContent.clientHeight + 30;
      setAutoScroll(isAtBottom);
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
      try {
        setConnectionStatus('connecting');

        // Close existing connection if any
        if (eventSourceRef.current) {
          eventSourceRef.current.close();
        }

        const options = apiKey ? {
            fetch: (input: string | URL | Request, init: RequestInit) =>
              fetch(input, {
                ...init,
                headers: {
                  ...init.headers,
                  Authorization: `Bearer ${apiKey}`,
                },
            })} : {};

        const eventSource = new EventSource(`${serverUrl}/api/v1/logs/stream`, options)
        eventSourceRef.current = eventSource;

        eventSource.onopen = () => {
          console.log('Log stream connected to:', serverUrl);
          setConnectionStatus('connected');
        };

        eventSource.onmessage = (event) => {
          // SSE sends data as "data: <log line>"
          const logLine = event.data;

          // Skip heartbeat messages
          if (logLine.trim() === '' || logLine === 'heartbeat') {
            return;
          }

          setLogs((prevLogs) => {
            // Keep last 1000 lines to prevent memory issues
            const newLogs = [...prevLogs, logLine];
            return newLogs.length > 1000 ? newLogs.slice(-1000) : newLogs;
          });
        };

        eventSource.onerror = (error) => {
          console.error('Log stream error:', error);
          setConnectionStatus('error');
          eventSource.close();

          // Attempt to reconnect after 5 seconds
          reconnectTimeoutRef.current = setTimeout(() => {
            console.log('Attempting to reconnect to log stream...');
            connectToLogStream();
          }, 5000);
        };
      } catch (error) {
        console.error('Failed to connect to log stream:', error);
        setConnectionStatus('error');

        // Attempt to reconnect after 5 seconds
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
  }, [isVisible, serverUrl, apiKey, isInitialized]);

  const handleClearLogs = () => {
    setLogs([]);
  };

  const handleScrollToBottom = () => {
    setAutoScroll(true);
    logsEndRef.current?.scrollIntoView({ behavior: 'smooth' });
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
