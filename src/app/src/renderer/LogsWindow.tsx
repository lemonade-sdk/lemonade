import React, { useEffect, useRef, useState, useMemo } from 'react';
import { getAPIKey, getServerBaseUrl, onServerUrlChange, serverConfig, serverFetch } from './utils/serverConfig';
import { connectLogStream, LogEntry, LogStreamHandle } from './utils/logWebSocketClient';

interface LogsWindowProps {
  isVisible: boolean;
  height?: number;
}

const BOTTOM_FOLLOW_THRESHOLD_PX = 60;
const LOG_LEVELS = ['trace', 'debug', 'info', 'notice', 'warning', 'error', 'critical', 'fatal', 'none'] as const;
type LogLevel = typeof LOG_LEVELS[number];

const isLogLevel = (value: unknown): value is LogLevel =>
  typeof value === 'string' && LOG_LEVELS.includes(value as LogLevel);

const LogsWindow: React.FC<LogsWindowProps> = ({ isVisible, height }) => {
  const [logs, setLogs] = useState<LogEntry[]>([]);
  const [connectionStatus, setConnectionStatus] = useState<'connecting' | 'connected' | 'error' | 'disconnected'>('connecting');
  const [autoScroll, setAutoScroll] = useState(true);
  const [logLevel, setLogLevel] = useState<LogLevel>('info');
  const [isSettingLogLevel, setIsSettingLogLevel] = useState(false);
  const [serverSupportsLogLevel, setServerSupportsLogLevel] = useState(false);
  const logsEndRef = useRef<HTMLDivElement>(null);
  const logsContentRef = useRef<HTMLDivElement>(null);
  const autoScrollRef = useRef(true);
  const isProgrammaticScrollRef = useRef(false);
  const lastSeqRef = useRef<number | null>(null);
  const socketRef = useRef<LogStreamHandle | null>(null);
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

  // Initialize server configuration and load the current log level
  useEffect(() => {
    const init = async () => {
      await serverConfig.waitForInit();
      const url = getServerBaseUrl();
      setServerUrl(url);
      setIsInitialized(true);

      try {
        // Using serverFetch with the full URL to reach the root-level /internal/config
        // endpoint while benefiting from automatic authentication and init-waiting.
        const response = await serverFetch(`${url}/internal/config`);
        if (!response.ok) return;

        const config = await response.json();
        if ('log_level' in config) {
          setServerSupportsLogLevel(true);
          if (isLogLevel(config.log_level)) {
            setLogLevel(config.log_level);
          }
        }
      } catch (error) {
        console.error('Failed to load log level:', error);
      }
    };

    init();
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

    if (!autoScrollRef.current && isNearBottom()) {
      setAutoScroll(true);
    }

    setLogs((prevLogs) => {
      const lastSeq = prevLogs.length > 0 ? prevLogs[prevLogs.length - 1].seq : -1;
      const newEntries = incomingEntries.filter((e) => e.seq > lastSeq);
      if (newEntries.length === 0) {
        return prevLogs;
      }

      lastSeqRef.current = newEntries[newEntries.length - 1].seq;
      const combined = [...prevLogs, ...newEntries];
      return combined.length > 1000 ? combined.slice(-1000) : combined;
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

        connectLogStream(lastSeqRef.current, {
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
        }).then((handle) => {
          socketRef.current = handle;
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

  const handleLogLevelChange = async (nextLevel: LogLevel) => {
    if (!isLogLevel(nextLevel)) {
      return;
    }

    const previousLevel = logLevel;
    setLogLevel(nextLevel);
    setIsSettingLogLevel(true);

    try {
      const response = await serverFetch('/log-level', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ level: nextLevel }),
      });

      if (!response.ok) {
        throw new Error(`Server returned ${response.status}`);
      }

      const data = await response.json();
      if (isLogLevel(data.level)) {
        setLogLevel(data.level);
      }
    } catch (error) {
      console.error('Failed to update log level:', error);
      setLogLevel(previousLevel);
    } finally {
      setIsSettingLogLevel(false);
    }
  };

  const getSeverityPriority = (level: string): number => {
    const p: Record<string, number> = {
      'trace': 0,
      'debug': 1,
      'info': 2,
      'notice': 3,
      'warning': 4,
      'warn': 4,
      'error': 5,
      'critical': 6,
      'fatal': 6,
      'none': 10
    };
    return p[level.toLowerCase()] ?? 2; // Default to info
  };

  const filteredLogs = useMemo(() => {
    if (logLevel === 'none') return [];
    const threshold = getSeverityPriority(logLevel);
    return logs.filter((log: LogEntry) => getSeverityPriority(log.severity) >= threshold);
  }, [logs, logLevel]);

  if (!isVisible) return null;

  return (
    <div className="logs-window" style={height ? { height: `${height}px`, flex: 'none' } : undefined}>
      <div className="logs-header">
        <h3>Server Logs</h3>
        <div className="logs-controls">
          {serverSupportsLogLevel && (
            <label className="logs-level-control">
              <span>Level</span>
              <select
                className="logs-level-select"
                value={logLevel}
                disabled={isSettingLogLevel}
                onChange={(event) => handleLogLevelChange(event.target.value as LogLevel)}
                title="Set server log level"
              >
                {LOG_LEVELS.map(level => (
                  <option key={level} value={level}>{level}</option>
                ))}
              </select>
            </label>
          )}
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
        {filteredLogs.length === 0 && connectionStatus === 'connected' && (
          <div className="logs-placeholder">
            {logLevel === 'none' ? 'Logging is disabled' : 'Waiting for logs...'}
          </div>
        )}
        {logs.length === 0 && connectionStatus === 'error' && (
          <div className="logs-error">
            Failed to connect to lemonade-server logs.
            <br />
            Make sure the server is running on {serverUrl}
          </div>
        )}
        <pre className="logs-text">
          {filteredLogs.map((log: LogEntry) => (
            <div key={log.seq} className="log-line">
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
