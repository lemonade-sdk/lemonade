import React, { useCallback, useEffect, useRef, useState } from 'react';
import { getAPIKey, getServerBaseUrl, onServerUrlChange, serverConfig } from './utils/serverConfig';
import {EventSource} from 'eventsource';

interface LogsWindowProps {
  isVisible: boolean;
  height?: number;
}

interface LogSource {
  name: string;
  label: string;
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
  const eventSourceRef = useRef<EventSource | null>(null);
  const reconnectTimeoutRef = useRef<NodeJS.Timeout | null>(null);
  const [serverUrl, setServerUrl] = useState<string>('');
  const [apiKey, setAPIKey] = useState<string>('');
  const [isInitialized, setIsInitialized] = useState(false);

  const [selectedSource, setSelectedSource] = useState<string>('all');
  const [availableSources, setAvailableSources] = useState<LogSource[]>([]);
  const sourcePollRef = useRef<NodeJS.Timeout | null>(null);

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

    requestAnimationFrame(() => {
      isProgrammaticScrollRef.current = false;
    });
  };

  useEffect(() => {
    serverConfig.waitForInit().then(() => {
      setServerUrl(getServerBaseUrl());
      setAPIKey(getAPIKey());
      setIsInitialized(true);
    });
  }, []);

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

  // Poll for available log sources
  const fetchSources = useCallback(async () => {
    if (!serverUrl || !isInitialized) return;

    try {
      const headers: Record<string, string> = {};
      if (apiKey) {
        headers['Authorization'] = `Bearer ${apiKey}`;
      }
      const resp = await fetch(`${serverUrl}/api/v1/logs/sources`, { headers });
      if (resp.ok) {
        const data = await resp.json();
        setAvailableSources(data.sources || []);
      }
    } catch {
      // Silently ignore — the sources list is a convenience, not critical
    }
  }, [serverUrl, apiKey, isInitialized]);

  useEffect(() => {
    if (!isVisible || !isInitialized || !serverUrl) {
      if (sourcePollRef.current) {
        clearInterval(sourcePollRef.current);
        sourcePollRef.current = null;
      }
      return;
    }

    fetchSources();
    sourcePollRef.current = setInterval(fetchSources, 10000);

    return () => {
      if (sourcePollRef.current) {
        clearInterval(sourcePollRef.current);
        sourcePollRef.current = null;
      }
    };
  }, [isVisible, isInitialized, serverUrl, apiKey, fetchSources]);

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

  // Connect to SSE log stream — reconnects when selectedSource changes
  useEffect(() => {
    if (!isVisible || !isInitialized || !serverUrl) {
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

        const streamUrl = selectedSource && selectedSource !== 'all'
          ? `${serverUrl}/api/v1/logs/stream?source=${encodeURIComponent(selectedSource)}`
          : `${serverUrl}/api/v1/logs/stream`;

        const eventSource = new EventSource(streamUrl, options);
        eventSourceRef.current = eventSource;

        eventSource.onopen = () => {
          console.log('Log stream connected to:', streamUrl);
          setConnectionStatus('connected');
        };

        eventSource.onmessage = (event) => {
          const logLine = event.data;

          if (logLine.trim() === '' || logLine === 'heartbeat') {
            return;
          }

          const shouldFollowNextLine = autoScrollRef.current || isNearBottom();
          if (shouldFollowNextLine && !autoScrollRef.current) {
            setAutoScroll(true);
          }

          setLogs((prevLogs) => {
            const newLogs = [...prevLogs, logLine];
            return newLogs.length > 1000 ? newLogs.slice(-1000) : newLogs;
          });
        };

        eventSource.onerror = (error) => {
          console.error('Log stream error:', error);
          setConnectionStatus('error');
          eventSource.close();

          reconnectTimeoutRef.current = setTimeout(() => {
            console.log('Attempting to reconnect to log stream...');
            connectToLogStream();
          }, 5000);
        };
      } catch (error) {
        console.error('Failed to connect to log stream:', error);
        setConnectionStatus('error');

        reconnectTimeoutRef.current = setTimeout(() => {
          connectToLogStream();
        }, 5000);
      }
    };

    connectToLogStream();

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
  }, [isVisible, serverUrl, apiKey, isInitialized, selectedSource]);

  const handleClearLogs = () => {
    setLogs([]);
  };

  const handleScrollToBottom = () => {
    setAutoScroll(true);
    scrollToBottom();
  };

  const handleSourceChange = (e: React.ChangeEvent<HTMLSelectElement>) => {
    setLogs([]);
    setSelectedSource(e.target.value);
  };

  if (!isVisible) return null;

  return (
    <div className="logs-window" style={height ? { height: `${height}px`, flex: 'none' } : undefined}>
      <div className="logs-header">
        <h3>Server Logs</h3>
        <div className="logs-controls">
          <select
            className="logs-source-select"
            value={selectedSource}
            onChange={handleSourceChange}
            title="Filter logs by source"
          >
            <option value="all">All Sources</option>
            {availableSources.map((src) => (
              <option key={src.name} value={src.name}>
                {src.label}
              </option>
            ))}
          </select>
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
