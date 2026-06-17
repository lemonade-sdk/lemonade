import React, { useCallback, useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { serverConfig } from './utils/serverConfig';
import { useModels } from './hooks/useModels';

// Mirror of one entry in `/v1/system-info`'s `cloud.providers` array.
// Single source of truth lives in the server — we never read the local
// settings file for cloud config. Cloud providers are shared infrastructure
// config, not per-client UI state (see AGENTS.md invariant #11).
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

const QUICK_FILL: Array<{ label: string; name: string; baseUrl: string }> = [
  { label: 'Fireworks', name: 'fireworks', baseUrl: 'https://api.fireworks.ai/inference/v1' },
  { label: 'OpenAI', name: 'openai', baseUrl: 'https://api.openai.com/v1' },
  { label: 'OpenRouter', name: 'openrouter', baseUrl: 'https://openrouter.ai/api/v1' },
  { label: 'Together', name: 'together', baseUrl: 'https://api.together.xyz/v1' },
];

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

// Install modal — single source for both new-provider registration and
// supplying an initial key. Optional api_key because LEMONADE_<P>_API_KEY may
// already cover this provider on the server; in that case the server returns
// a "warning" field which we surface verbatim.
interface InstallModalProps {
  onClose: () => void;
  onInstalled: () => void;
  showError: (msg: string) => void;
  showSuccess: (msg: string) => void;
}

const InstallModal: React.FC<InstallModalProps> = ({ onClose, onInstalled, showError, showSuccess }) => {
  const [provider, setProvider] = useState('');
  const [baseUrl, setBaseUrl] = useState('');
  const [apiKey, setApiKey] = useState('');
  const [showKey, setShowKey] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const submit = useCallback(async () => {
    if (!provider.trim() || !baseUrl.trim()) {
      setError('Provider name and base URL are required.');
      return;
    }
    setBusy(true);
    setError(null);
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
        setError(`Install failed (${response.status}): ${text}`);
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
      setError(`Install failed: ${err instanceof Error ? err.message : String(err)}`);
      setBusy(false);
    }
  }, [provider, baseUrl, apiKey, onInstalled, showSuccess]);

  return (
    <>
      <div className="settings-header">
        <h2>Install cloud provider</h2>
        <button className="settings-close-button" onClick={onClose} title="Close">×</button>
      </div>
      <div className="settings-content">
        <span className="settings-description" style={{ display: 'block', marginBottom: '12px' }}>
          Registers an OpenAI-compatible chat provider on this lemonade server. Provider URL is
          persisted; the API key (if you supply one) lives in process memory only and dies on restart.
          For persistence across restarts, set <code>LEMONADE_&lt;PROVIDER&gt;_API_KEY</code> in
          lemond's environment instead.
        </span>

        <div className="form-section">
          <label className="form-label">Quick-fill</label>
          <div style={{ display: 'flex', flexWrap: 'wrap', gap: '6px' }}>
            {QUICK_FILL.map((q) => (
              <button
                key={q.label}
                className="settings-reset-button"
                onClick={() => { setProvider(q.name); setBaseUrl(q.baseUrl); }}
                disabled={busy}
              >
                {q.label}
              </button>
            ))}
          </div>
        </div>

        <div className="form-section">
          <label className="form-label" title="Short identifier used as the model name prefix">
            Provider name
          </label>
          <input
            type="text"
            className="form-input"
            placeholder="fireworks"
            value={provider}
            onChange={(e) => setProvider(e.target.value)}
            disabled={busy}
          />
        </div>

        <div className="form-section">
          <label className="form-label" title="OpenAI-compatible base URL ending in /v1">
            Base URL
          </label>
          <input
            type="url"
            className="form-input"
            placeholder="https://api.fireworks.ai/inference/v1"
            value={baseUrl}
            onChange={(e) => setBaseUrl(e.target.value)}
            disabled={busy}
          />
        </div>

        <div className="form-section">
          <label className="form-label" title="Optional. If unset, lemond will use LEMONADE_<PROVIDER>_API_KEY at request time.">
            API key (optional)
          </label>
          <div style={{ display: 'flex', gap: '6px' }}>
            <input
              type={showKey ? 'text' : 'password'}
              className="form-input"
              placeholder="Leave blank if env var is set"
              value={apiKey}
              onChange={(e) => setApiKey(e.target.value)}
              disabled={busy}
              style={{ flex: 1 }}
            />
            <button
              className="settings-reset-button"
              onClick={() => setShowKey(!showKey)}
              disabled={busy}
            >
              {showKey ? 'Hide' : 'Show'}
            </button>
          </div>
        </div>

        {error && <div className="form-error">{error}</div>}
      </div>

      <div className="settings-footer">
        <button className="settings-reset-button" onClick={onClose} disabled={busy}>
          Cancel
        </button>
        <button className="settings-save-button" onClick={submit} disabled={busy}>
          {busy ? 'Installing…' : 'Install'}
        </button>
      </div>
    </>
  );
};

// Per-provider edit modal — sets/clears the runtime key, removes the
// provider. The base URL is not editable here because changing it makes the
// stored runtime key meaningless against the new URL; we'd rather force the
// user to uninstall + reinstall and re-authenticate explicitly.
interface EditModalProps {
  row: CloudProviderRow;
  onClose: () => void;
  onChanged: () => void;
  showError: (msg: string) => void;
  showSuccess: (msg: string) => void;
}

const EditModal: React.FC<EditModalProps> = ({ row, onClose, onChanged, showError, showSuccess }) => {
  const [apiKey, setApiKey] = useState('');
  const [showKey, setShowKey] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const saveKey = useCallback(async () => {
    if (!apiKey.trim()) {
      setError('API key cannot be empty.');
      return;
    }
    setBusy(true); setError(null);
    try {
      const r = await serverConfig.fetch('/cloud/auth', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ provider: row.name, api_key: apiKey.trim() }),
      });
      if (r.status === 409) {
        const body = await r.json().catch(() => null);
        const envVar = body?.error?.env_var ?? row.env_var;
        setError(`${envVar} is already set in lemond's environment; the env var takes precedence and the key you entered was not stored.`);
        setBusy(false);
        return;
      }
      if (!r.ok) {
        setError(`Set auth failed (${r.status}): ${await r.text()}`);
        setBusy(false);
        return;
      }
      const result = await r.json();
      showSuccess(`API key stored for '${row.name}' (${result?.models_discovered ?? 0} models discovered).`);
      onChanged();
    } catch (err) {
      setError(`Set auth failed: ${err instanceof Error ? err.message : String(err)}`);
      setBusy(false);
    }
  }, [apiKey, row.env_var, row.name, onChanged, showSuccess]);

  const clearKey = useCallback(async () => {
    setBusy(true); setError(null);
    try {
      const r = await serverConfig.fetch(`/cloud/auth/${encodeURIComponent(row.name)}`, { method: 'DELETE' });
      if (!r.ok) {
        setError(`Clear failed (${r.status}).`);
        setBusy(false);
        return;
      }
      showSuccess(`API key cleared for '${row.name}'.`);
      onChanged();
    } catch (err) {
      setError(`Clear failed: ${err instanceof Error ? err.message : String(err)}`);
      setBusy(false);
    }
  }, [row.name, onChanged, showSuccess]);

  const uninstall = useCallback(async () => {
    if (!window.confirm(`Remove cloud provider '${row.name}'? Discovered models will be dropped from the cache.`)) return;
    setBusy(true); setError(null);
    try {
      const r = await serverConfig.fetch('/uninstall', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ backend: 'cloud', provider: row.name }),
      });
      if (!r.ok) {
        setError(`Uninstall failed (${r.status}).`);
        setBusy(false);
        return;
      }
      showSuccess(`Removed cloud provider '${row.name}'.`);
      onChanged();
    } catch (err) {
      setError(`Uninstall failed: ${err instanceof Error ? err.message : String(err)}`);
      setBusy(false);
    }
  }, [row.name, onChanged, showSuccess]);

  return (
    <>
      <div className="settings-header">
        <h2>{row.name}</h2>
        <button className="settings-close-button" onClick={onClose} title="Close">×</button>
      </div>
      <div className="settings-content">
        <div className="form-section">
          <label className="form-label">Base URL</label>
          <input
            type="url"
            className="form-input"
            value={row.base_url}
            disabled
            style={{ opacity: 0.7 }}
          />
        </div>

        <div className="form-section">
          <label className="form-label">API key</label>
          <div style={{ fontSize: '0.85rem', lineHeight: 1.5 }}>
            {row.env_var_set ? (
              <span>
                ✅ Set via <code>{row.env_var}</code> in lemond's environment.
                Env vars take precedence — any key entered here is ignored.
              </span>
            ) : row.runtime_key_set ? (
              <span>
                ✅ Set. Stored in lemond's process memory only; will be cleared
                on lemond restart unless re-supplied.
              </span>
            ) : (
              <span>
                ⚠️ Not set. Paste a key below for in-memory use, or set
                {' '}<code>{row.env_var}</code> in lemond's environment for
                persistence across restarts.
              </span>
            )}
          </div>
        </div>

        {!row.env_var_set && (
          <div className="form-section">
            <label className="form-label">
              {row.runtime_key_set ? 'Replace API key' : 'Set API key'}
            </label>
            <div style={{ display: 'flex', gap: '6px' }}>
              <input
                type={showKey ? 'text' : 'password'}
                className="form-input"
                placeholder="Paste API key"
                value={apiKey}
                onChange={(e) => setApiKey(e.target.value)}
                disabled={busy}
                style={{ flex: 1 }}
              />
              <button
                className="settings-reset-button"
                onClick={() => setShowKey(!showKey)}
                disabled={busy}
              >
                {showKey ? 'Hide' : 'Show'}
              </button>
              <button
                className="settings-save-button"
                onClick={saveKey}
                disabled={busy || !apiKey.trim()}
              >
                Save
              </button>
            </div>
            {row.runtime_key_set && (
              <button
                className="settings-reset-button"
                onClick={clearKey}
                disabled={busy}
                style={{ marginTop: '6px' }}
              >
                Clear API key
              </button>
            )}
          </div>
        )}

        {error && <div className="form-error">{error}</div>}
      </div>

      <div className="settings-footer">
        <button
          className="settings-reset-button"
          onClick={uninstall}
          disabled={busy}
          style={{ color: 'var(--text-danger, #d04444)' }}
        >
          Remove provider
        </button>
        <button className="settings-save-button" onClick={onClose} disabled={busy}>
          Close
        </button>
      </div>
    </>
  );
};

const CloudProvidersSection: React.FC<CloudProvidersSectionProps> = ({ searchQuery, showError, showSuccess }) => {
  const [providers, setProviders] = useState<CloudProviderRow[]>([]);
  const [isLoading, setIsLoading] = useState(true);
  const [modal, setModal] = useState<
    { mode: 'install' } | { mode: 'edit'; row: CloudProviderRow } | null
  >(null);

  // To avoid frequent, visually visiable reloads
  const showErrorRef = useRef(showError);
  useEffect(() => {
    showErrorRef.current = showError;
  }, [showError]);

  // Install / uninstall / edit on a cloud provider can change the set of
  // models visible in /v1/models (discovery adds the provider's chat-capable
  // ids, eviction removes them). The Backends tab doesn't re-fetch the
  // models list on its own — refresh the global store so the user doesn't
  // have to reload the tab to see new cloud entries.
  const { refresh: refreshModels } = useModels();

  const reload = useCallback(async () => {
    setIsLoading(true);
    try {
      const rows = await fetchCloudProviders();
      setProviders(rows);
    } catch (err) {
      showErrorRef.current(`Failed to load cloud providers: ${err instanceof Error ? err.message : String(err)}`);
    } finally {
      setIsLoading(false);
    }
  }, []);

  const reloadAll = useCallback(async () => {
    await reload();
    await refreshModels();
  }, [reload, refreshModels]);

  useEffect(() => {
    reload();
  }, [reload]);

  const query = searchQuery.trim().toLowerCase();
  const filtered = providers.filter((p) => {
    if (!query) return true;
    return `${p.name} ${p.base_url}`.toLowerCase().includes(query);
  });

  return (
    <>
      <div className="model-category">
        <div className="model-category-header static" style={{ justifyContent: 'space-between' }}>
          <span>
            <span className="category-label">Cloud</span>
            <span className="category-count">({filtered.length})</span>
          </span>
          <button
            className="settings-reset-button"
            style={{ fontSize: '0.65rem', padding: '1px 8px', whiteSpace: 'nowrap' }}
            title="Install a new cloud provider"
            onClick={(e) => { e.stopPropagation(); setModal({ mode: 'install' }); }}
          >
            + Add
          </button>
        </div>

        <div className="model-list">
          {isLoading ? (
            <div className="backend-row-item" style={{ padding: '4px 12px', opacity: 0.7, fontSize: '0.74rem' }}>
              Loading…
            </div>
          ) : filtered.length === 0 ? (
            <div className="backend-row-item" style={{ padding: '4px 12px', opacity: 0.7, fontSize: '0.74rem' }}>
              {query
                ? 'No providers match your search.'
                : 'No providers configured. Click "+ Add" to connect Fireworks, OpenAI, Together, OpenRouter, or any OpenAI-compatible endpoint.'}
            </div>
          ) : (
            filtered.map((p) => {
              const authed = p.env_var_set || p.runtime_key_set;
              const dotClass = authed ? 'loaded' : 'update-required';
              const dotTitle = p.env_var_set
                ? `API key from ${p.env_var} (env var)`
                : p.runtime_key_set
                  ? 'API key set in lemond process memory'
                  : `No API key. Click Edit to set one, or export ${p.env_var} on the server.`;

              return (
                <div
                  key={p.name}
                  className="model-item backend-row-item"
                  style={{ padding: '2px 12px' }}
                >
                  <div className="model-item-content">
                    <div className="model-info-left backend-row-main">
                      <div className="backend-row-head">
                        <span className="model-name backend-name">
                          <span
                            className={`model-status-indicator ${dotClass}`}
                            title={dotTitle}
                          >●</span>
                          {p.name}
                        </span>
                      </div>
                      <div className="backend-row-detail">
                        <div className="backend-inline-meta">
                          <span className="backend-version">
                            {p.models_discovered} model{p.models_discovered === 1 ? '' : 's'}
                          </span>
                          {p.base_url && (
                            <>
                              <span className="backend-meta-separator">•</span>
                              <span className="backend-size" style={{ overflow: 'hidden', textOverflow: 'ellipsis' }}>
                                {p.base_url}
                              </span>
                            </>
                          )}
                        </div>
                        <div className="model-actions">
                          <button
                            className="settings-reset-button"
                            style={{ fontSize: '0.65rem', padding: '1px 8px' }}
                            onClick={() => setModal({ mode: 'edit', row: p })}
                          >
                            Edit
                          </button>
                        </div>
                      </div>
                    </div>
                  </div>
                </div>
              );
            })
          )}
        </div>
      </div>

      {modal && createPortal(
        <div
          className="settings-overlay"
          onMouseDown={(e: React.MouseEvent<HTMLDivElement>) => {
            if (e.target === e.currentTarget) setModal(null);
          }}
        >
          <div className="settings-modal" onMouseDown={(e: React.MouseEvent) => e.stopPropagation()}>
            {modal.mode === 'install' ? (
              <InstallModal
                onClose={() => setModal(null)}
                onInstalled={() => { setModal(null); reloadAll(); }}
                showError={showError}
                showSuccess={showSuccess}
              />
            ) : (
              <EditModal
                row={modal.row}
                onClose={() => setModal(null)}
                onChanged={() => { setModal(null); reloadAll(); }}
                showError={showError}
                showSuccess={showSuccess}
              />
            )}
          </div>
        </div>,
        document.body,
      )}
    </>
  );
};

export default CloudProvidersSection;
