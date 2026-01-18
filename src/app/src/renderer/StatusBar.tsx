import React, { useState, useEffect, useCallback } from 'react';
import { serverConfig, onServerUrlChange } from './utils/serverConfig';

interface ServerStats {
  input_tokens: number | null;
  output_tokens: number | null;
  time_to_first_token: number | null;
  tokens_per_second: number | null;
}

interface SystemStats {
  cpu_percent: number;
  memory_gb: number;
  gpu_percent: number | null;
  npu_percent: number | null;
}

const StatusBar: React.FC = () => {
  const [serverStats, setServerStats] = useState<ServerStats>({
    input_tokens: null,
    output_tokens: null,
    time_to_first_token: null,
    tokens_per_second: null,
  });
  const [systemStats, setSystemStats] = useState<SystemStats>({
    cpu_percent: 0,
    memory_gb: 0,
    gpu_percent: null,
    npu_percent: null,
  });

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
      }
    } catch {
      // Server may not be running, ignore errors
    }
  }, []);

  const fetchSystemStats = useCallback(async () => {
    try {
      if (window.api?.getSystemStats) {
        const stats = await window.api.getSystemStats();
        setSystemStats({
          cpu_percent: stats.cpu_percent ?? 0,
          memory_gb: stats.memory_gb ?? 0,
          gpu_percent: stats.gpu_percent ?? null,
          npu_percent: stats.npu_percent ?? null,
        });
      }
    } catch {
      // Ignore errors
    }
  }, []);

  useEffect(() => {
    // Initial fetch
    fetchStats();
    fetchSystemStats();

    // Poll every 2 seconds
    const statsInterval = setInterval(fetchStats, 2000);
    const systemInterval = setInterval(fetchSystemStats, 2000);

    // Re-fetch when server URL changes
    const unsubscribe = onServerUrlChange(() => {
      fetchStats();
    });

    return () => {
      clearInterval(statsInterval);
      clearInterval(systemInterval);
      unsubscribe();
    };
  }, [fetchStats, fetchSystemStats]);

  const formatTokens = (value: number | null): string => {
    if (value === null || value === undefined) return 'N/A';
    return value.toLocaleString();
  };

  const formatMemory = (gb: number): string => {
    return `${gb.toFixed(1)} GB`;
  };

  const formatPercent = (percent: number | null): string => {
    if (percent === null || percent === undefined) return 'N/A';
    return `${percent.toFixed(2)} %`;
  };

  const formatTtft = (seconds: number | null): string => {
    if (seconds === null || seconds === undefined) return 'N/A';
    return `${seconds.toFixed(2)} s`;
  };

  return (
    <div className="status-bar">
      <div className="status-bar-item">
        <span className="status-bar-label">INPUT TOKENS:</span>
        <span className="status-bar-value">{formatTokens(serverStats.input_tokens)}</span>
      </div>
      <div className="status-bar-item">
        <span className="status-bar-label">TOKENS:</span>
        <span className="status-bar-value">{formatTokens(serverStats.output_tokens)}</span>
      </div>
      <div className="status-bar-item">
        <span className="status-bar-label">TTFT:</span>
        <span className="status-bar-value">{formatTtft(serverStats.time_to_first_token)}</span>
      </div>
      <div className="status-bar-item">
        <span className="status-bar-label">RAM:</span>
        <span className="status-bar-value">{formatMemory(systemStats.memory_gb)}</span>
      </div>
      <div className="status-bar-item">
        <span className="status-bar-label">CPU:</span>
        <span className="status-bar-value">{formatPercent(systemStats.cpu_percent)}</span>
      </div>
      <div className="status-bar-item">
        <span className="status-bar-label">GPU:</span>
        <span className="status-bar-value">{formatPercent(systemStats.gpu_percent)}</span>
      </div>
      <div className="status-bar-item">
        <span className="status-bar-label">NPU:</span>
        <span className="status-bar-value">{formatPercent(systemStats.npu_percent)}</span>
      </div>
    </div>
  );
};

export default StatusBar;
