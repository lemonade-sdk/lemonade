import React, { useCallback, useEffect, useState } from 'react';
import { createPortal } from 'react-dom';
import { serverConfig } from './utils/serverConfig';

// Mirror of one entry in `/v1/system-info`'s `cloud.providers` array.
// Single source of truth lives in the server — we never read the local
// settings file for cloud config (per the post-#2076 refactor: cloud
// providers are shared infrastructure config, not per-client UI state).
interface CloudProviderRow {
  name: string;
  base_url: string;
  env_var: string;
  env_var_set: boolean;
  runtime_key_set: boolean;
  models_discovered: number;
}

interface CloudProvidersSectionProps {
  searchQuery: string;
  showError: (msg: string) => void;
  showSuccess: (msg: string) => void;
}

const fetchCloudProviders = async (): Promise<CloudProviderRow[]> => {
  const response = await serverConfig.fetch('/system-info');
  if (!response.ok) return [];
  const info = await response.json();
  const providers = info?.cloud?.providers;
  if (!Array.isArray(providers)) return [];
  return providers
    .filter((p: any) => p && typeof p.name === 'string')
    .map((p: any) => ({
      name: String(p.name),
      base_url: typeof p.base_url === 'string' ? p.base_url : '',
      env_var: typeof p.env_var === 'string' ? p.env_var : '',
      env_var_set: p.env_var_set === true,
      runtime_key_set: p.runtime_key_set === true,
      models_discovered: typeof p.models_discovered === 'number' ? p.models_discovered : 0,
    }));
};

// Simple inline install modal. Single form: provider name, base URL, optional
// API key. Optional because the operator may have already exported
// LEMONADE_<P>_API_KEY in lemond's environment — in that case the server's
// env var takes precedence and the runtime key is silently ignored (the
// server returns a "warning" field, which we surface).
interface InstallModalProps {
  onCancel: () => void;
  onInstalled: () => void;
  showError: (msg: string) => void;
  showSuccess: (msg: string) => void;
}

const InstallModal: React.FC<InstallModalProps> = ({ onCancel, onInstalled, showError, showSuccess }) => {
  const [provider, setProvider] = useState('');
  const [baseUrl, setBaseUrl] = useState('');
  const [apiKey, setApiKey] = useState('');
  const [busy, setBusy] = useState(false);

  const submit = useCallback(async () => {
    if (!provider.trim() || !baseUrl.trim()) {
      showError('Provider name and base URL are required.');
      return;
    }
    setBusy(true);
    try {
      const body: Record<string, string> = {
        backend: 'cloud',
        provider: provider.trim(),
        base_url: baseUrl.trim(),
      };
      if (apiKey.trim()) body.api_key = apiKey.trim();
      const response = await serverConfig.fetch('/install', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      if (!response.ok) {
        const text = await response.text();
        showError(`Install failed (${response.status}): ${text}`);
        setBusy(false);
        return;
      }
      const result = await response.json();
      const discovered = result?.models_discovered ?? 0;
      if (result?.warning) {
        showSuccess(`Installed '${provider.trim()}' (${discovered} models). ${result.warning}`);
      } else {
        showSuccess(`Installed '${provider.trim()}' (${discovered} models).`);
      }
      onInstalled();
    } catch (err) {
      showError(`Install failed: ${err instanceof Error ? err.message : String(err)}`);
      setBusy(false);
    }
  }, [provider, baseUrl, apiKey, onInstalled, showError, showSuccess]);

  return createPortal(
    <div className="modal-backdrop">
      <div className="modal">
        <div className="modal-header">
          <h3>Install cloud provider</h3>
        </div>
        <div className="modal-body">
          <label className="form-row">
            <span>Provider name</span>
            <input
              type="text"
              placeholder="e.g. fireworks, openai"
              value={provider}
              onChange={(e) => setProvider(e.target.value)}
              disabled={busy}
            />
          </label>
          <label className="form-row">
            <span>Base URL (OpenAI-compat /v1)</span>
            <input
              type="url"
              placeholder="https://api.fireworks.ai/inference/v1"
              value={baseUrl}
              onChange={(e) => setBaseUrl(e.target.value)}
              disabled={busy}
            />
          </label>
          <label className="form-row">
            <span>API key (optional)</span>
            <input
              type="password"
              placeholder="Leave blank if LEMONADE_<PROVIDER>_API_KEY is set"
              value={apiKey}
              onChange={(e) => setApiKey(e.target.value)}
              disabled={busy}
            />
          </label>
          <p className="form-help">
            Keys supplied here are kept in lemond's process memory only — never written to disk.
            For persistence across restarts, set the <code>LEMONADE_&lt;PROVIDER&gt;_API_KEY</code>
            environment variable before launching lemond.
          </p>
        </div>
        <div className="modal-actions">
          <button onClick={onCancel} disabled={busy}>Cancel</button>
          <button onClick={submit} disabled={busy} className="primary">
            {busy ? 'Installing…' : 'Install'}
          </button>
        </div>
      </div>
    </div>,
    document.body,
  );
};

const CloudProvidersSection: React.FC<CloudProvidersSectionProps> = ({ searchQuery, showError, showSuccess }) => {
  const [providers, setProviders] = useState<CloudProviderRow[]>([]);
  const [showInstall, setShowInstall] = useState(false);
  const [authKeyDraft, setAuthKeyDraft] = useState<Record<string, string>>({});

  const reload = useCallback(async () => {
    try {
      const rows = await fetchCloudProviders();
      setProviders(rows);
    } catch (err) {
      showError(`Failed to load cloud providers: ${err instanceof Error ? err.message : String(err)}`);
    }
  }, [showError]);

  useEffect(() => {
    reload();
  }, [reload]);

  const submitAuth = useCallback(async (name: string) => {
    const key = (authKeyDraft[name] ?? '').trim();
    if (!key) {
      showError('API key cannot be empty.');
      return;
    }
    const response = await serverConfig.fetch('/cloud/auth', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ provider: name, api_key: key }),
    });
    if (response.status === 409) {
      const body = await response.json().catch(() => null);
      const envVar = body?.error?.env_var ?? `LEMONADE_${name.toUpperCase()}_API_KEY`;
      showError(`${envVar} is set in lemond's environment; the env var takes precedence and the runtime key was not stored.`);
      return;
    }
    if (!response.ok) {
      showError(`Set auth failed (${response.status}): ${await response.text()}`);
      return;
    }
    const result = await response.json();
    setAuthKeyDraft((prev) => ({ ...prev, [name]: '' }));
    showSuccess(`API key stored for '${name}' (${result?.models_discovered ?? 0} models discovered).`);
    reload();
  }, [authKeyDraft, reload, showError, showSuccess]);

  const clearAuth = useCallback(async (name: string) => {
    const response = await serverConfig.fetch(`/cloud/auth/${encodeURIComponent(name)}`, {
      method: 'DELETE',
    });
    if (!response.ok) {
      showError(`Clear auth failed (${response.status})`);
      return;
    }
    showSuccess(`Runtime key cleared for '${name}'.`);
    reload();
  }, [reload, showError, showSuccess]);

  const uninstall = useCallback(async (name: string) => {
    if (!window.confirm(`Remove cloud provider '${name}'? Discovered models will be dropped from the cache.`)) return;
    const response = await serverConfig.fetch('/uninstall', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ backend: 'cloud', provider: name }),
    });
    if (!response.ok) {
      showError(`Uninstall failed (${response.status})`);
      return;
    }
    showSuccess(`Removed cloud provider '${name}'.`);
    reload();
  }, [reload, showError, showSuccess]);

  const query = searchQuery.trim().toLowerCase();
  const visible = providers.filter((p) => {
    if (!query) return true;
    return `${p.name} ${p.base_url}`.toLowerCase().includes(query);
  });

  return (
    <div className="model-category">
      <div className="model-category-header static">
        <span className="category-label">Cloud providers</span>
        <span className="category-count">({providers.length})</span>
        <button className="link-button" onClick={() => setShowInstall(true)}>+ Install provider</button>
      </div>
      <div className="model-list">
        {visible.length === 0 && (
          <div className="left-panel-empty-state-row">
            {providers.length === 0
              ? 'No cloud providers installed.'
              : 'No providers match the current filter.'}
          </div>
        )}
        {visible.map((p) => (
          <div key={p.name} className="cloud-provider-row">
            <div className="cloud-provider-summary">
              <strong>{p.name}</strong>
              <span className="cloud-provider-url">{p.base_url}</span>
              <span className="cloud-provider-models">{p.models_discovered} models</span>
            </div>
            <div className="cloud-provider-auth">
              {p.env_var_set ? (
                <span className="badge">Auth: env var {p.env_var}</span>
              ) : p.runtime_key_set ? (
                <span className="badge">Auth: runtime key</span>
              ) : (
                <span className="badge warn">Auth: none — discovery disabled</span>
              )}
            </div>
            {!p.env_var_set && (
              <div className="cloud-provider-auth-form">
                <input
                  type="password"
                  placeholder={p.runtime_key_set ? 'Replace runtime key…' : 'Set API key…'}
                  value={authKeyDraft[p.name] ?? ''}
                  onChange={(e) => setAuthKeyDraft((prev) => ({ ...prev, [p.name]: e.target.value }))}
                />
                <button onClick={() => submitAuth(p.name)}>Save</button>
                {p.runtime_key_set && (
                  <button onClick={() => clearAuth(p.name)} className="secondary">Clear</button>
                )}
              </div>
            )}
            <div className="cloud-provider-actions">
              <button onClick={() => uninstall(p.name)} className="danger">Remove</button>
            </div>
          </div>
        ))}
      </div>
      {showInstall && (
        <InstallModal
          onCancel={() => setShowInstall(false)}
          onInstalled={() => { setShowInstall(false); reload(); }}
          showError={showError}
          showSuccess={showSuccess}
        />
      )}
    </div>
  );
};

export default CloudProvidersSection;
