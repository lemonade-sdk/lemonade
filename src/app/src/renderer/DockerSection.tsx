import React, { useCallback, useEffect, useRef, useState } from 'react';
import { serverConfig, getServerBaseUrl, serverFetch } from './utils/serverConfig';
import {
  AppSettings,
  mergeWithDefaultSettings,
} from './utils/appSettings';

interface DockerConfig {
  images: string[];
  models: Array<{ id: string; serve_args: string }>;
  default_model: string;
  provider: string;
  cloud_api_key: string;
  default_port: number;
  docker_available: boolean;
}

interface DockerStatus {
  running: boolean;
  api_ready?: boolean;
  log_ready?: boolean;
  last_log_line?: string;
  recent_log_lines?: string[];
  container_id?: string;
  container_name?: string;
  image?: string;
  model?: string;
  base_url?: string;
  port?: number;
  provider?: string;
  gpu_count?: number;
  gpu_type?: string;
}

interface DockerSectionProps {
  searchQuery: string;
  showError: (msg: string) => void;
  showSuccess: (msg: string) => void;
}

const DEFAULT_DOCKER_CLOUD_API_KEY = 'local lemonade';

const formatModelCount = (count: number): string => (
  `${count} model${count === 1 ? '' : 's'}`
);

const formatGpuUsage = (gpuCount?: number, gpuType?: string): string => {
  if (!gpuCount || gpuCount <= 0 || !gpuType) {
    return '';
  }
  return ` • ${gpuCount}× ${gpuType}`;
};

const discoverProviderModels = async (
  provider: string,
  baseUrl: string,
  apiKey: string,
): Promise<number> => {
  const response = await serverConfig.fetch(`${getServerBaseUrl()}/internal/cloud/discover`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ provider, base_url: baseUrl, api_key: apiKey }),
  });
  if (!response.ok) return 0;
  const body = await response.json();
  const list = Array.isArray(body?.data) ? body.data : [];
  const ids = list
    .map((entry: any) => entry?.id)
    .filter((id: unknown): id is string => typeof id === 'string');
  serverConfig.setKnownCloudModels(provider, ids);
  return list.length;
};

const saveCloudProvider = async (
  provider: string,
  baseUrl: string,
  apiKey: string,
): Promise<void> => {
  if (!window.api?.getSettings || !window.api?.saveSettings) {
    throw new Error('Settings storage unavailable');
  }
  const stored = await window.api.getSettings();
  const merged = mergeWithDefaultSettings(stored as AppSettings | undefined);
  merged.cloudProviders[provider] = { baseUrl, apiKey };
  await window.api.saveSettings(merged);
};

const removeCloudProvider = async (provider: string): Promise<void> => {
  if (!window.api?.getSettings || !window.api?.saveSettings) {
    throw new Error('Settings storage unavailable');
  }
  const stored = await window.api.getSettings();
  const merged = mergeWithDefaultSettings(stored as AppSettings | undefined);
  delete merged.cloudProviders[provider];
  await window.api.saveSettings(merged);
  serverConfig.setKnownCloudModels(provider, []);
};

const loadModelInChat = async (modelName: string): Promise<void> => {
  window.dispatchEvent(new CustomEvent('modelLoadStart', { detail: { modelId: modelName } }));
  try {
    const loadResponse = await serverFetch('/load', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ model_name: modelName }),
    });
    if (!loadResponse.ok) {
      const errorData = await loadResponse.json().catch(() => ({}));
      const errorMsg = (typeof errorData.error === 'string'
        ? errorData.error
        : errorData.error?.message)
        || `Failed to load model: ${loadResponse.statusText}`;
      throw new Error(errorMsg);
    }
    window.dispatchEvent(new CustomEvent('modelLoadEnd', { detail: { modelId: modelName } }));
  } catch (error) {
    window.dispatchEvent(new CustomEvent('modelLoadEnd', { detail: { modelId: modelName } }));
    throw error;
  }
};

const DockerSection: React.FC<DockerSectionProps> = ({
  searchQuery, showError, showSuccess,
}) => {
  const [config, setConfig] = useState<DockerConfig | null>(null);
  const [status, setStatus] = useState<DockerStatus | null>(null);
  const [selectedImage, setSelectedImage] = useState('');
  const [selectedModel, setSelectedModel] = useState('');
  const [isLoading, setIsLoading] = useState(true);
  const [isStarting, setIsStarting] = useState(false);
  const [isStopping, setIsStopping] = useState(false);
  const [isWaitingForReady, setIsWaitingForReady] = useState(false);
  const [modelCount, setModelCount] = useState<number | null>(null);
  const [lastLogLine, setLastLogLine] = useState('');
  const [recentLogLines, setRecentLogLines] = useState<string[]>([]);
  const [logReady, setLogReady] = useState(false);
  const wasRunningRef = useRef(false);
  const cloudRegisteredRef = useRef(false);

  const applyStatus = useCallback((st: DockerStatus) => {
    setStatus(st);
    setLastLogLine(st.last_log_line ?? '');
    setRecentLogLines(Array.isArray(st.recent_log_lines) ? st.recent_log_lines : []);
    setLogReady(st.log_ready === true);
  }, []);

  const clearDockerCloudConnection = useCallback(async (provider: string) => {
    if (!window.api?.getSettings) return;
    try {
      await removeCloudProvider(provider);
      cloudRegisteredRef.current = false;
      window.dispatchEvent(new CustomEvent('modelsUpdated'));
    } catch (e) {
      console.error('Failed to remove docker cloud provider:', e);
    }
  }, []);

  const registerCloudAndActivate = useCallback(async (
    provider: string,
    baseUrl: string,
    apiKey: string,
    upstreamModel: string,
  ): Promise<number> => {
    if (cloudRegisteredRef.current) {
      return 0;
    }

    await saveCloudProvider(provider, baseUrl, apiKey);
    const count = await discoverProviderModels(provider, baseUrl, apiKey);
    window.dispatchEvent(new CustomEvent('modelsUpdated'));

    const modelName = `${provider}.${upstreamModel}`;
    await loadModelInChat(modelName);

    cloudRegisteredRef.current = true;
    return count;
  }, []);

  const pollUntilFullyReady = useCallback(async (
    provider: string,
    baseUrl: string,
    apiKey: string,
    upstreamModel: string,
    maxAttempts = 120,
    intervalMs = 5000,
  ): Promise<number> => {
    for (let attempt = 0; attempt < maxAttempts; attempt += 1) {
      const statusRes = await fetch(`${getServerBaseUrl()}/internal/docker/status`);
      if (statusRes.ok) {
        const st = await statusRes.json() as DockerStatus;
        applyStatus(st);
        if (st.log_ready) {
          return registerCloudAndActivate(provider, baseUrl, apiKey, upstreamModel);
        }
      }
      await new Promise((resolve) => setTimeout(resolve, intervalMs));
    }
    throw new Error('SGLang server did not become fully ready in time');
  }, [applyStatus, registerCloudAndActivate]);

  const finalizeStartup = useCallback(async (
    provider: string,
    baseUrl: string,
    apiKey: string,
    upstreamModel: string,
    logReadyInitially: boolean,
  ): Promise<number> => {
    if (logReadyInitially) {
      return registerCloudAndActivate(provider, baseUrl, apiKey, upstreamModel);
    }
    setIsWaitingForReady(true);
    try {
      return await pollUntilFullyReady(provider, baseUrl, apiKey, upstreamModel);
    } finally {
      setIsWaitingForReady(false);
    }
  }, [pollUntilFullyReady, registerCloudAndActivate]);

  const pollStatus = useCallback(async () => {
    try {
      const statusRes = await fetch(`${getServerBaseUrl()}/internal/docker/status`);
      if (!statusRes.ok) return;
      const st = await statusRes.json() as DockerStatus;
      const provider = st.provider || 'sglang';
      if (wasRunningRef.current && !st.running) {
        await clearDockerCloudConnection(provider);
        setModelCount(null);
      }
      if (
        st.running
        && st.log_ready
        && !cloudRegisteredRef.current
        && st.base_url
        && st.model
      ) {
        const apiKey = config?.cloud_api_key || DEFAULT_DOCKER_CLOUD_API_KEY;
        try {
          const count = await registerCloudAndActivate(
            provider,
            st.base_url,
            apiKey,
            st.model,
          );
          setModelCount(count);
          window.dispatchEvent(new CustomEvent('modelsUpdated'));
        } catch (e) {
          console.error('Failed to register docker cloud endpoint:', e);
        }
      }
      wasRunningRef.current = st.running;
      applyStatus(st);
    } catch (e) {
      console.error('Failed to poll docker status:', e);
    }
  }, [applyStatus, clearDockerCloudConnection, config?.cloud_api_key, registerCloudAndActivate]);

  const loadState = useCallback(async () => {
    setIsLoading(true);
    let registrationTask: (() => Promise<void>) | null = null;
    try {
      const [configRes, statusRes] = await Promise.all([
        fetch(`${getServerBaseUrl()}/internal/docker/config`),
        fetch(`${getServerBaseUrl()}/internal/docker/status`),
      ]);
      let cloudApiKey = DEFAULT_DOCKER_CLOUD_API_KEY;
      let defaultModel = '';
      if (configRes.ok) {
        const cfg = await configRes.json() as DockerConfig;
        setConfig(cfg);
        cloudApiKey = cfg.cloud_api_key || DEFAULT_DOCKER_CLOUD_API_KEY;
        defaultModel = cfg.default_model || cfg.models[0]?.id || '';
        setSelectedImage((prev) => prev || cfg.images[0] || '');
        setSelectedModel((prev) => {
          if (prev) return prev;
          const known = cfg.models.some((m) => m.id === defaultModel);
          return known ? defaultModel : (cfg.models[0]?.id || '');
        });
      }
      if (statusRes.ok) {
        const st = await statusRes.json() as DockerStatus;
        applyStatus(st);
        wasRunningRef.current = st.running;
        const upstreamModel = st.model || defaultModel;
        if (st.running && st.provider && st.base_url && upstreamModel) {
          const provider = st.provider;
          const baseUrl = st.base_url;
          const apiKey = cloudApiKey;
          registrationTask = async () => {
            if (st.log_ready) {
              try {
                const count = await registerCloudAndActivate(
                  provider,
                  baseUrl,
                  apiKey,
                  upstreamModel,
                );
                setModelCount(count);
              } catch (e) {
                console.error('Failed to register docker cloud endpoint:', e);
              }
              return;
            }
            setIsWaitingForReady(true);
            try {
              const count = await pollUntilFullyReady(
                provider,
                baseUrl,
                apiKey,
                upstreamModel,
              );
              setModelCount(count);
              window.dispatchEvent(new CustomEvent('modelsUpdated'));
            } catch (e) {
              console.error('Failed waiting for SGLang readiness:', e);
            } finally {
              setIsWaitingForReady(false);
            }
          };
        } else if (st.provider) {
          setModelCount(null);
          if (!st.running) {
            await clearDockerCloudConnection(st.provider);
          }
        }
      }
    } catch (e) {
      console.error('Failed to load docker section:', e);
    } finally {
      setIsLoading(false);
    }
    if (registrationTask) {
      void registrationTask();
    }
  }, [
    applyStatus,
    clearDockerCloudConnection,
    pollUntilFullyReady,
    registerCloudAndActivate,
  ]);

  useEffect(() => {
    loadState();
  }, [loadState]);

  useEffect(() => {
    pollStatus();
    const intervalId = window.setInterval(pollStatus, 10000);
    return () => window.clearInterval(intervalId);
  }, [pollStatus]);

  const handleStart = async () => {
    if (!selectedImage || !selectedModel) {
      showError('Select a docker image and model first.');
      return;
    }
    setIsStarting(true);
    cloudRegisteredRef.current = false;
    try {
      const response = await fetch(`${getServerBaseUrl()}/internal/docker/start`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ image: selectedImage, model: selectedModel }),
      });
      const body = await response.json();
      if (!response.ok) {
        throw new Error(body?.error || 'Failed to start container');
      }

      const provider = body.provider as string;
      const baseUrl = body.base_url as string;
      const logReadyInitially = body.log_ready === true;
      const apiKey = config?.cloud_api_key || DEFAULT_DOCKER_CLOUD_API_KEY;

      setStatus({
        running: true,
        api_ready: body.ready === true,
        log_ready: logReadyInitially,
        last_log_line: '',
        container_id: body.container_id,
        image: selectedImage,
        model: selectedModel,
        base_url: baseUrl,
        port: body.port,
        provider,
      });
      setLastLogLine('');
      setLogReady(logReadyInitially);

      const count = await finalizeStartup(
        provider,
        baseUrl,
        apiKey,
        selectedModel,
        logReadyInitially,
      );
      setModelCount(count);
      setStatus((prev) => (prev ? { ...prev, api_ready: true, log_ready: true } : prev));
      setLogReady(true);

      window.dispatchEvent(new CustomEvent('modelsUpdated'));
      showSuccess(`SGLang ready — loaded ${provider}.${selectedModel} in chat`);
    } catch (e: any) {
      showError(e?.message || String(e));
    } finally {
      setIsStarting(false);
    }
  };

  const handleStop = async () => {
    const provider = status?.provider || config?.provider || 'sglang';
    if (!window.confirm('Stop the SGLang container and remove its cloud endpoint?')) {
      return;
    }
    setIsStopping(true);
    try {
      const response = await fetch(`${getServerBaseUrl()}/internal/docker/stop`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({}),
      });
      const body = await response.json();
      if (!response.ok) {
        throw new Error(body?.error || 'Failed to stop container');
      }

      await clearDockerCloudConnection(provider);
      applyStatus({ running: false, provider });
      setModelCount(null);
      window.dispatchEvent(new CustomEvent('modelsUpdated'));
      showSuccess('Stopped SGLang container');
    } catch (e: any) {
      showError(e?.message || String(e));
    } finally {
      setIsStopping(false);
    }
  };

  const query = searchQuery.trim().toLowerCase();
  const haystack = `local docker container docker sglang ${selectedImage} ${selectedModel} ${status?.base_url || ''}`.toLowerCase();
  if (query && !haystack.includes(query)) {
    return null;
  }

  if (isLoading) {
    return (
      <div className="model-category">
        <div className="model-category-header static">
          <span className="category-label">Local docker container</span>
        </div>
        <div className="left-panel-empty-state">Loading…</div>
      </div>
    );
  }

  const running = status?.running ?? false;
  const apiReady = status?.api_ready ?? false;
  const busy = isStarting || isWaitingForReady;
  const containerIndicatorClass = !running
    ? 'stopped'
    : logReady
      ? 'ready'
      : 'starting';
  const containerIndicatorTitle = !running
    ? 'Container not running'
    : logReady
      ? 'Container ready to serve'
      : 'Container starting';
  const logPreviewText = running
    ? (lastLogLine || 'Waiting for container output…')
    : 'No container running';
  const logTooltip = recentLogLines.length > 0
    ? recentLogLines.join('\n')
    : logPreviewText;

  return (
    <div className="model-category">
      <div className="model-category-header static">
        <span className="category-label">Local docker container</span>
        <span className="category-count">({running ? 1 : 0})</span>
      </div>
      <div className="model-list">
        <div className="backend-row-item docker-section">
          <span
            className={`model-status-indicator ${running && logReady ? 'loaded' : running ? 'loading' : 'update-required'}`}
            title={running && logReady ? 'SGLang ready in chat' : running ? 'SGLang container starting' : 'No container running'}
          />
          <div className="docker-section-body">
            {config && !config.docker_available && (
              <p className="form-hint">Docker CLI is not available on this server.</p>
            )}

            <label className="form-label">Docker image</label>
            <select
              className="form-input"
              value={selectedImage}
              onChange={(e) => setSelectedImage(e.target.value)}
              disabled={running || busy || isStopping}
            >
              {(config?.images ?? []).map((image) => (
                <option key={image} value={image}>{image}</option>
              ))}
            </select>

            <label className="form-label">Model</label>
            <select
              className="form-input"
              value={selectedModel}
              onChange={(e) => setSelectedModel(e.target.value)}
              disabled={running || busy || isStopping}
            >
              {(config?.models ?? []).map((model) => (
                <option key={model.id} value={model.id}>{model.id}</option>
              ))}
            </select>

            {running && status?.base_url && (
              <p className="form-hint">
                {logReady ? 'Ready' : apiReady ? 'API up, warming up…' : 'Starting'} at {status.base_url}
                {modelCount != null && modelCount > 0
                  ? ` • ${formatModelCount(modelCount)}`
                  : busy ? ' • waiting for SGLang…' : ''}
                {formatGpuUsage(status.gpu_count, status.gpu_type)}
              </p>
            )}

            <div className="docker-section-actions">
              {!running ? (
                <button
                  type="button"
                  className="settings-save-button"
                  disabled={!config?.docker_available || busy || !selectedImage || !selectedModel}
                  onClick={handleStart}
                >
                  {isStarting ? 'Creating container…' : 'Create container'}
                </button>
              ) : (
                <button
                  type="button"
                  className="danger-button"
                  disabled={isStopping || busy}
                  onClick={handleStop}
                >
                  {isStopping ? 'Stopping…' : 'Stop container'}
                </button>
              )}
              {config?.docker_available && (
                <>
                  <span
                    className={`docker-container-status-dot ${containerIndicatorClass}`}
                    title={containerIndicatorTitle}
                  />
                  <div
                    className="docker-log-preview"
                    title={logTooltip}
                  >
                    {logPreviewText}
                  </div>
                </>
              )}
            </div>
          </div>
        </div>
      </div>
    </div>
  );
};

export default DockerSection;
