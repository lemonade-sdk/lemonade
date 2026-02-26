import React, { useState, useEffect, useCallback, useRef } from 'react';
import { serverConfig, onServerUrlChange } from './utils/serverConfig';

interface ServerStats {
  input_tokens: number | null;
  output_tokens: number | null;
  time_to_first_token: number | null;
  tokens_per_second: number | null;
}

interface SystemStats {
  cpu_percent: number | null;
  memory_gb: number;
  gpu_percent: number | null;
  vram_gb: number | null;
}

const StatusBar: React.FC = () => {
  const [serverStats, setServerStats] = useState<ServerStats>({
    input_tokens: null,
    output_tokens: null,
    time_to_first_token: null,
    tokens_per_second: null,
  });
  const [systemStats, setSystemStats] = useState<SystemStats>({
    cpu_percent: null,
    memory_gb: 0,
    gpu_percent: null,
    vram_gb: null,
  });
  const [connectionStatus, setConnectionStatus] = useState<'connected' | 'connecting' | 'disconnected'>('connecting');
  const [serverUrl, setServerUrl] = useState<string>('');
  const [lastSuccessfulConnection, setLastSuccessfulConnection] = useState<number | null>(null);
  const lastSystemStatsFetchRef = useRef<number>(0);

  const fetchStats = useCallback(async () => {
    try {
      const response = await serverConfig.fetch('/stats');
      if (response.ok) {
        const data = await response.json();
        setServerStats({
          input_tokens: data.input_tokens ?? null,
          output_tokens: data.output_tokens ?? null,
          time_to_first_token: data.time_to_first_token ?? null,
          tokens_per_second: data.tokens_per_second ?? null,
        });
        setConnectionStatus('connected');
        setLastSuccessfulConnection(Date.now());
      }
    } catch {
      // Connection monitoring interval will handle timeout
      setConnectionStatus('disconnected');
    }
  }, []);

  const fetchSystemStats = useCallback(async () => {
    const now = Date.now();
    if (now - lastSystemStatsFetchRef.current < 1000) {
      return;
    }
    lastSystemStatsFetchRef.current = now;

    try {
      if (window.api?.getSystemStats) {
        const stats = await window.api.getSystemStats();
        setSystemStats({
          cpu_percent: stats.cpu_percent ?? null,
          memory_gb: stats.memory_gb ?? 0,
          gpu_percent: stats.gpu_percent ?? null,
          vram_gb: stats.vram_gb ?? null,
        });
      }
    } catch {
    }
  }, []);

  useEffect(() => {
    fetchStats();
    fetchSystemStats();

    const initialUrl = serverConfig.getServerBaseUrl();
    setServerUrl(initialUrl);

    const handleInferenceComplete = () => {
      fetchStats();
    };
    window.addEventListener('inference-complete', handleInferenceComplete);

    const unsubscribe = onServerUrlChange((url, apiKey) => {
      setServerUrl(url);
      setConnectionStatus('connecting');
      fetchStats();
    });

    return () => {
      window.removeEventListener('inference-complete', handleInferenceComplete);
      unsubscribe();
    };
  }, [fetchStats, fetchSystemStats]);

  useEffect(() => {
    // Poll every 3s when connecting/disconnected, every 15s when connected
    const pollInterval = connectionStatus === 'connected' ? 15000 : 3000;
    const statsInterval = setInterval(fetchStats, pollInterval);
    const systemInterval = setInterval(fetchSystemStats, 5000);

    return () => {
      clearInterval(statsInterval);
      clearInterval(systemInterval);
    };
  }, [connectionStatus, fetchStats, fetchSystemStats]);

  const formatTokens = (value: number | null): string => {
    if (value === null || value === undefined) return 'N/A';
    return value.toLocaleString();
  };

  const formatMemory = (gb: number): string => {
    return `${gb.toFixed(1)} GB`;
  };

  const formatVram = (gb: number | null): string => {
    if (gb === null || gb === undefined) return 'N/A';
    return `${gb.toFixed(1)} GB`;
  };

  const formatPercent = (percent: number | null): string => {
    if (percent === null || percent === undefined) return 'N/A';
    return `${percent.toFixed(1)} %`;
  };

  const formatTtft = (seconds: number | null): string => {
    if (seconds === null || seconds === undefined) return 'N/A';
    return `${seconds.toFixed(2)} s`;
  };

  const formatTps = (tps: number | null): string => {
    if (tps === null || tps === undefined) return 'N/A';
    return `${tps.toFixed(1)}`;
  };

  const formatServerUrl = (url: string): string => {
    if (!url) return 'N/A';

    if (url.includes('localhost') || url.includes('127.0.0.1')) {
      return url;
    }

    try {
      const urlObj = new URL(url);
      return urlObj.host;
    } catch {
      return url;
    }
  };

  return (
    <div className="status-bar">
      <div className="status-bar-item status-bar-connection" title={serverUrl}>
        <span className={`connection-indicator connection-${connectionStatus}`}>●</span>
        <span className="status-bar-label status-bar-label-long">STATUS:</span>
        <span className="status-bar-label status-bar-label-short"></span>
        <span className="status-bar-value">{connectionStatus.toUpperCase()}</span>
      </div>

      <div className="status-bar-item">
        <span className="status-bar-label status-bar-label-long">INPUT TOKENS:</span>
        <span className="status-bar-label status-bar-label-short">IN:</span>
        <span className="status-bar-value">{formatTokens(serverStats.input_tokens)}</span>
      </div>
      <div className="status-bar-item">
        <span className="status-bar-label status-bar-label-long">OUTPUT TOKENS:</span>
        <span className="status-bar-label status-bar-label-short">OUT:</span>
        <span className="status-bar-value">{formatTokens(serverStats.output_tokens)}</span>
      </div>
      <div className="status-bar-item">
        <span className="status-bar-label status-bar-label-long">TPS:</span>
        <span className="status-bar-label status-bar-label-short">TPS:</span>
        <span className="status-bar-value">{formatTps(serverStats.tokens_per_second)}</span>
      </div>
      <div className="status-bar-item">
        <span className="status-bar-label status-bar-label-long">TTFT:</span>
        <span className="status-bar-label status-bar-label-short">TTFT:</span>
        <span className="status-bar-value">{formatTtft(serverStats.time_to_first_token)}</span>
      </div>
      <div className="status-bar-item">
        <span className="status-bar-label status-bar-label-long">RAM:</span>
        <span className="status-bar-label status-bar-label-short">RAM:</span>
        <span className="status-bar-value">{formatMemory(systemStats.memory_gb)}</span>
      </div>
      {systemStats.cpu_percent !== null && systemStats.cpu_percent !== undefined && (
        <div className="status-bar-item">
          <span className="status-bar-label status-bar-label-long">CPU:</span>
          <span className="status-bar-label status-bar-label-short">CPU:</span>
          <span className="status-bar-value">{formatPercent(systemStats.cpu_percent)}</span>
        </div>
      )}
      {systemStats.gpu_percent !== null && systemStats.gpu_percent !== undefined && (
        <div className="status-bar-item">
          <span className="status-bar-label status-bar-label-long">GPU:</span>
          <span className="status-bar-label status-bar-label-short">GPU:</span>
          <span className="status-bar-value">{formatPercent(systemStats.gpu_percent)}</span>
        </div>
      )}
      {systemStats.vram_gb !== null && (
        <div className="status-bar-item">
          <span className="status-bar-label status-bar-label-long">VRAM:</span>
          <span className="status-bar-label status-bar-label-short">VRAM:</span>
          <span className="status-bar-value">{formatVram(systemStats.vram_gb)}</span>
        </div>
      )}
    </div>
  );
};

export default StatusBar;
