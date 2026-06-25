import React, { useCallback, useEffect, useMemo, useState } from 'react';
import api, { CloudProviderRow, ConnectionStatus, DirectorySettings, friendlyErrorMessage, normalizeBaseUrl } from '../api';
import { AccountSession, clearAllAccountsAndScopedData, clearCurrentSessionData, describeSession } from '../features/accounts/accountStore';
import { Icon, IconName } from './Icon';
import McpPanel from './McpPanel';

interface ConnectViewProps {
  status: ConnectionStatus;
  accountSession: AccountSession;
  onLocalDataReset: () => void;
  onSessionChange: (session: AccountSession) => void;
}

type MarketplaceApp = {
  id: string;
  name: string;
  description?: string;
  category?: string[];
  logo?: string;
  pinned?: boolean;
  links?: { app?: string; guide?: string; video?: string };
};

const MARKETPLACE_URL = 'https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps.json';

const HELP_LINKS: { label: string; href: string; icon: IconName; description: string }[] = [
  { label: 'Documentation', href: 'https://lemonade-server.ai/docs/', icon: 'book-open', description: 'Setup, APIs, and integration guides.' },
  { label: 'Release notes', href: 'https://github.com/lemonade-sdk/lemonade/releases', icon: 'newspaper', description: 'Latest packaged changes and tags.' },
  { label: 'GitHub', href: 'https://github.com/lemonade-sdk/lemonade', icon: 'github', description: 'Source, issues, and pull requests.' },
  { label: 'Discord', href: 'https://discord.gg/5xXzkMu8Zk', icon: 'discord', description: 'Community support and discussion.' },
];

const CLOUD_QUICK_FILL = [
  { label: 'Fireworks', provider: 'fireworks', baseUrl: 'https://api.fireworks.ai/inference/v1' },
  { label: 'OpenAI', provider: 'openai', baseUrl: 'https://api.openai.com/v1' },
  { label: 'OpenRouter', provider: 'openrouter', baseUrl: 'https://openrouter.ai/api/v1' },
  { label: 'Together', provider: 'together', baseUrl: 'https://api.together.xyz/v1' },
];

const emptyDirectorySettings: DirectorySettings = { modelsDir: '', extraModelsDir: '', canPersist: false };

const ConnectView: React.FC<ConnectViewProps> = ({ status, accountSession, onLocalDataReset, onSessionChange }) => {
  const [host, setHost] = useState(api.baseUrl);
  const [apiKey, setApiKey] = useState(api.apiKey);
  const [canPersistApiKey, setCanPersistApiKey] = useState(api.canPersistApiKey);
  const [rememberApiKey, setRememberApiKey] = useState(api.canPersistApiKey && Boolean(api.apiKey));
  const [connecting, setConnecting] = useState(false);
  const [error, setError] = useState<string | null>(api.lastConnectionError);
  const [notice, setNotice] = useState<string | null>(null);

  const [providers, setProviders] = useState<CloudProviderRow[]>([]);
  const [providerName, setProviderName] = useState('');
  const [providerBaseUrl, setProviderBaseUrl] = useState('');
  const [providerApiKey, setProviderApiKey] = useState('');
  const [editingProvider, setEditingProvider] = useState<string | null>(null);
  const [editingApiKey, setEditingApiKey] = useState('');
  const [cloudBusy, setCloudBusy] = useState(false);
  const [cloudLoading, setCloudLoading] = useState(false);
  const [cloudLoadedOnce, setCloudLoadedOnce] = useState(false);
  const [cloudError, setCloudError] = useState<string | null>(null);
  const [cloudNotice, setCloudNotice] = useState<string | null>(null);

  const [directories, setDirectories] = useState<DirectorySettings>(emptyDirectorySettings);
  const [savingDirectories, setSavingDirectories] = useState(false);
  const [directoryNotice, setDirectoryNotice] = useState<string | null>(null);
  const [directoryError, setDirectoryError] = useState<string | null>(null);

  const [marketplaceApps, setMarketplaceApps] = useState<MarketplaceApp[]>([]);
  const [marketplaceSearch, setMarketplaceSearch] = useState('');
  const [marketplaceError, setMarketplaceError] = useState<string | null>(null);
  const [marketplaceLoading, setMarketplaceLoading] = useState(false);

  const loadCloudProviders = useCallback(async () => {
    if (!api.isConnected) {
      setProviders([]);
      setCloudLoadedOnce(false);
      return;
    }
    setCloudLoading(true);
    try {
      const rows = await api.cloudProviders();
      setProviders(rows);
      setCloudLoadedOnce(true);
      setCloudError(null);
    } catch (err) {
      setCloudError(`Cloud providers unavailable: ${friendlyErrorMessage(err)}`);
    } finally {
      setCloudLoading(false);
    }
  }, []);

  useEffect(() => {
    let cancelled = false;

    api.loadConnectionSettings()
      .then(() => {
        if (cancelled) return;
        setHost(api.baseUrl);
        setApiKey(api.apiKey);
        setCanPersistApiKey(api.canPersistApiKey);
        setRememberApiKey(api.canPersistApiKey && Boolean(api.apiKey));
        setError(api.lastConnectionError);
      })
      .catch(err => {
        if (cancelled) return;
        setHost(api.baseUrl);
        setApiKey(api.apiKey);
        setCanPersistApiKey(api.canPersistApiKey);
        setRememberApiKey(false);
        setError(friendlyErrorMessage(err));
      });

    api.loadDirectorySettings()
      .then(settings => { if (!cancelled) setDirectories(settings); })
      .catch(err => { if (!cancelled) setDirectoryError(friendlyErrorMessage(err)); });

    return () => { cancelled = true; };
  }, []);

  useEffect(() => {
    if (status === 'connected') void loadCloudProviders();
    else setProviders([]);
  }, [status, loadCloudProviders]);

  useEffect(() => {
    let cancelled = false;
    setMarketplaceLoading(true);
    fetch(MARKETPLACE_URL)
      .then(response => {
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return response.json();
      })
      .then(data => {
        if (cancelled) return;
        const apps = Array.isArray(data?.apps) ? data.apps as MarketplaceApp[] : [];
        setMarketplaceApps(apps);
        setMarketplaceError(null);
      })
      .catch(err => { if (!cancelled) setMarketplaceError(friendlyErrorMessage(err)); })
      .finally(() => { if (!cancelled) setMarketplaceLoading(false); });
    return () => { cancelled = true; };
  }, []);

  const handleConnect = async () => {
    setConnecting(true);
    setError(null);
    setNotice(null);
    let normalized: string;
    try {
      normalized = normalizeBaseUrl(host);
    } catch (err) {
      setError(friendlyErrorMessage(err));
      setConnecting(false);
      return;
    }

    try {
      api.baseUrl = normalized;
      api.setSessionApiKey(apiKey);

      const connected = await api.connect();
      if (!connected) {
        setError(api.lastConnectionError || `Could not connect to ${normalized}.`);
        return;
      }

      const saveResult = await api.saveConnectionSettings(normalized, apiKey, canPersistApiKey && rememberApiKey);
      setHost(normalized);
      setCanPersistApiKey(api.canPersistApiKey);
      setRememberApiKey(api.canPersistApiKey && saveResult.apiKeyPersisted);

      if (apiKey && saveResult.apiKeyPersisted) {
        setNotice(`Connected to ${normalized}. API key saved in Lemonade app settings.`);
      } else {
        setNotice(`Connected to ${normalized}.`);
      }
      await loadCloudProviders();
    } catch (err) {
      setError(friendlyErrorMessage(err));
    } finally {
      setConnecting(false);
    }
  };

  const handleClearLocalData = async () => {
    const target = accountSession.role === 'admin'
      ? 'all scoped user/guest data plus global connection settings'
      : `${describeSession(accountSession)} data plus global connection settings`;
    const ok = window.confirm(`Clear ${target} on this device? Other signed-in users are protected unless you are admin.`);
    if (!ok) return;

    if (accountSession.role === 'admin') {
      onSessionChange(clearAllAccountsAndScopedData());
    } else {
      clearCurrentSessionData(accountSession);
    }

    for (const store of [localStorage, sessionStorage]) {
      Object.keys(store)
        .filter(k => k === 'lemonade_base_url' || k === 'lemonade_api_key' || k === 'lemonade_current_view' || k === 'lemonade_theme')
        .forEach(k => store.removeItem(k));
    }

    let clearSettingsError: string | null = null;
    try {
      await api.clearConnectionSettings();
    } catch (err) {
      clearSettingsError = `Local browser data was cleared, but Lemonade app settings could not be cleared: ${friendlyErrorMessage(err)}`;
    }

    api.setSessionApiKey('');
    setHost(api.baseUrl);
    setApiKey('');
    setCanPersistApiKey(api.canPersistApiKey);
    setRememberApiKey(false);
    onLocalDataReset();
    setNotice(accountSession.role === 'admin'
      ? 'Admin cleared all scoped local user data and global connection settings.'
      : 'Current profile data and global connection settings were cleared.');
    setError(clearSettingsError);
  };

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter') handleConnect();
  };

  const handleInstallCloudProvider = async () => {
    if (!providerName.trim() || !providerBaseUrl.trim()) {
      setCloudError('Provider name and base URL are required.');
      return;
    }
    setCloudBusy(true);
    setCloudError(null);
    setCloudNotice(null);
    try {
      const result = await api.installCloudProvider(providerName, providerBaseUrl, providerApiKey);
      setCloudNotice(`Installed ${providerName.trim()} (${Number(result.models_discovered || 0)} models discovered).`);
      setProviderName('');
      setProviderBaseUrl('');
      setProviderApiKey('');
      await loadCloudProviders();
    } catch (err) {
      setCloudError(friendlyErrorMessage(err));
    } finally {
      setCloudBusy(false);
    }
  };

  const handleSaveProviderKey = async (provider: string) => {
    if (!editingApiKey.trim()) return;
    setCloudBusy(true);
    setCloudError(null);
    setCloudNotice(null);
    try {
      const result = await api.setCloudProviderAuth(provider, editingApiKey);
      setCloudNotice(`API key saved for ${provider} (${Number(result.models_discovered || 0)} models discovered).`);
      setEditingProvider(null);
      setEditingApiKey('');
      await loadCloudProviders();
    } catch (err) {
      setCloudError(friendlyErrorMessage(err));
    } finally {
      setCloudBusy(false);
    }
  };

  const handleClearProviderKey = async (provider: string) => {
    setCloudBusy(true);
    setCloudError(null);
    try {
      await api.clearCloudProviderAuth(provider);
      setCloudNotice(`API key cleared for ${provider}.`);
      await loadCloudProviders();
    } catch (err) {
      setCloudError(friendlyErrorMessage(err));
    } finally {
      setCloudBusy(false);
    }
  };

  const handleRemoveProvider = async (provider: string) => {
    if (!window.confirm(`Remove cloud provider ${provider}?`)) return;
    setCloudBusy(true);
    setCloudError(null);
    try {
      await api.uninstallCloudProvider(provider);
      setCloudNotice(`Removed ${provider}.`);
      await loadCloudProviders();
    } catch (err) {
      setCloudError(friendlyErrorMessage(err));
    } finally {
      setCloudBusy(false);
    }
  };

  const handleSaveDirectories = async () => {
    setSavingDirectories(true);
    setDirectoryError(null);
    setDirectoryNotice(null);
    try {
      const saved = await api.saveDirectorySettings(directories.modelsDir, directories.extraModelsDir);
      setDirectories(saved);
      setDirectoryNotice(saved.canPersist
        ? 'Directory settings saved. Restart or rescan the Lemonade server for model discovery changes to take effect.'
        : 'This runtime cannot persist directory settings; use the desktop app host bridge or start lemond with --extra-models-dir.');
    } catch (err) {
      setDirectoryError(friendlyErrorMessage(err));
    } finally {
      setSavingDirectories(false);
    }
  };

  const filteredMarketplaceApps = useMemo(() => {
    const query = marketplaceSearch.trim().toLowerCase();
    return marketplaceApps
      .filter(app => !query || app.name.toLowerCase().includes(query) || (app.description || '').toLowerCase().includes(query) || (app.category || []).join(' ').toLowerCase().includes(query))
      .sort((a, b) => Number(Boolean(b.pinned)) - Number(Boolean(a.pinned)) || a.name.localeCompare(b.name));
  }, [marketplaceApps, marketplaceSearch]);

  const openExternal = (url?: string) => {
    if (!url) return;
    const hostApi = (window as unknown as { api?: { openExternal?: (url: string) => void } }).api;
    if (hostApi?.openExternal) {
      hostApi.openExternal(url);
      return;
    }
    window.open(url, '_blank', 'noopener,noreferrer');
  };

  return (
    <div className="connect">
      <header className="connect__hero">
        <div>
          <p className="connect__eyebrow">Server, cloud, directories, apps</p>
          <h1>Connect</h1>
          <p className="connect__hero-copy">Configure the Lemonade server connection, cloud providers, custom model directories and compatible app integrations from one view.</p>
        </div>
        <div className="connect__status" aria-live="polite">
          <span className={`connect__status-dot ${
            status === 'connected' ? 'connect__status-dot--connected' :
            status === 'connecting' ? 'connect__status-dot--connecting' : ''
          }`} />
          <span>
            {status === 'connected' ? `Connected to ${api.baseUrl}` :
             status === 'connecting' ? `Connecting to ${host || api.baseUrl}...` :
             'Disconnected'}
          </span>
        </div>
      </header>

      <div className="connect__layout">
        <section className="connect__section connect__section--server">
          <h2>Server</h2>
          <form className="connect__form" onSubmit={e => { e.preventDefault(); handleConnect(); }}>
            <div className="form-field">
              <label className="form-field__label" htmlFor="host-input">Server URL</label>
              <input
                id="host-input"
                type="text"
                value={host}
                onChange={e => setHost(e.target.value)}
                onKeyDown={handleKeyDown}
                placeholder="http://localhost:13305"
                aria-invalid={Boolean(error)}
              />
              <span className="form-field__hint">Use a full http:// or https:// URL. Connection errors show the exact endpoint.</span>
            </div>

            <div className="form-field">
              <label className="form-field__label" htmlFor="key-input">API Key (optional)</label>
              <input
                id="key-input"
                type="text"
                value={apiKey}
                onChange={e => setApiKey(e.target.value)}
                onKeyDown={handleKeyDown}
                placeholder="sk-..."
              />
              {canPersistApiKey && (
                <label className="connect__checkbox">
                  <input
                    type="checkbox"
                    checked={rememberApiKey}
                    onChange={e => setRememberApiKey(e.target.checked)}
                  />
                  <span>Remember API key</span>
                </label>
              )}
            </div>

            {error && <div className="connect__error">Warning: {error}</div>}
            {notice && <div className="connect__notice">{notice}</div>}

            <div className="connect__actions">
              <button type="submit" className="btn btn--primary" disabled={connecting || !host.trim()}>
                {connecting ? 'Connecting...' : 'Connect'}
              </button>
              <button type="button" className="btn btn--ghost" onClick={() => { void handleClearLocalData(); }}>
                Clear permitted local data
              </button>
            </div>
          </form>
        </section>


        <section className="connect__section connect__section--help">
          <h2>Help</h2>
          <p className="connect__hint">Quick access to project support, documentation, and community channels.</p>
          <div className="connect__help-grid" aria-label="Help links">
            {HELP_LINKS.map(link => (
              <button key={link.href} className="connect__help-card" type="button" onClick={() => openExternal(link.href)}>
                <span className="connect__help-icon"><Icon name={link.icon} size={18} /></span>
                <span>
                  <strong>{link.label}</strong>
                  <small>{link.description}</small>
                </span>
              </button>
            ))}
          </div>
        </section>

        <section className="connect__section connect__section--directories">
          <h2>Custom model directories</h2>
          <p className="connect__hint">Keep the normal Lemonade model cache separate from an external GGUF directory scanned as extra custom models.</p>
          <div className="connect__directory-grid">
            <label className="connect__directory-field">Models directory
              <input value={directories.modelsDir} onChange={e => setDirectories(prev => ({ ...prev, modelsDir: e.target.value }))} placeholder="Default Lemonade model cache" />
            </label>
            <label className="connect__directory-field">External custom models directory
              <input value={directories.extraModelsDir} onChange={e => setDirectories(prev => ({ ...prev, extraModelsDir: e.target.value }))} placeholder="/path/to/llama.cpp/models" />
            </label>
          </div>
          <div className="connect__actions">
            <button className="btn btn--primary" type="button" onClick={() => { void handleSaveDirectories(); }} disabled={savingDirectories}>
              {savingDirectories ? 'Saving...' : 'Save directories'}
            </button>
          </div>
          {directoryNotice && <div className="connect__notice">{directoryNotice}</div>}
          {directoryError && <div className="connect__error">{directoryError}</div>}
        </section>

        <section className="connect__section connect__section--cloud">
          <div className="connect__section-head">
            <h2>Cloud providers</h2>
            <button className="btn btn--ghost" type="button" onClick={() => { void loadCloudProviders(); }} disabled={status !== 'connected' || cloudBusy || cloudLoading}>{cloudLoading ? 'Refreshing...' : 'Refresh'}</button>
          </div>
          <p className="connect__hint">Register OpenAI-compatible providers on the connected Lemonade server. Runtime keys can be replaced or cleared without editing files.</p>
          <div className="connect__quick-fill">
            {CLOUD_QUICK_FILL.map(item => (
              <button key={item.provider} className="btn btn--ghost" type="button" disabled={cloudBusy} onClick={() => { setProviderName(item.provider); setProviderBaseUrl(item.baseUrl); }}>
                {item.label}
              </button>
            ))}
          </div>
          <div className="connect__provider-form">
            <label className="sr-only" htmlFor="cloud-provider-name">Provider name</label>
            <input id="cloud-provider-name" value={providerName} onChange={e => setProviderName(e.target.value)} placeholder="provider name, e.g. fireworks" />
            <label className="sr-only" htmlFor="cloud-provider-url">Base URL</label>
            <input id="cloud-provider-url" value={providerBaseUrl} onChange={e => setProviderBaseUrl(e.target.value)} placeholder="https://api.example.com/v1" aria-describedby="cloud-provider-url-hint" />
            <label className="sr-only" htmlFor="cloud-provider-key">Provider API key (optional)</label>
            <input id="cloud-provider-key" value={providerApiKey} onChange={e => setProviderApiKey(e.target.value)} type="password" placeholder="API key (optional)" />
            <button className="btn btn--primary connect__add-provider" type="button" onClick={() => { void handleInstallCloudProvider(); }} disabled={status !== 'connected' || cloudBusy}>Add provider</button>
          </div>
          <span id="cloud-provider-url-hint" className="sr-only">Full https:// base URL of the OpenAI-compatible provider endpoint.</span>
          {cloudError && <div className="connect__error">{cloudError}</div>}
          {cloudNotice && <div className="connect__notice">{cloudNotice}</div>}
          <div className="connect__provider-list">
            {providers.length === 0 ? (
              <div className="connect__empty">{status === 'connected' ? (cloudLoading && !cloudLoadedOnce ? 'Loading cloud providers...' : 'No cloud providers configured yet.') : 'Connect to a server to manage cloud providers.'}</div>
            ) : providers.map(provider => {
              const authed = provider.env_var_set || provider.runtime_key_set;
              return (
                <div key={provider.name} className="connect__provider-row">
                  <div>
                    <strong>{provider.name}</strong>
                    <span>{provider.models_discovered} models · {provider.base_url || 'no URL'}</span>
                    <span>{authed ? `Auth configured${provider.env_var_set ? ` via ${provider.env_var}` : ''}` : `No API key${provider.env_var ? ` (${provider.env_var})` : ''}`}</span>
                  </div>
                  <div className="connect__provider-actions">
                    {editingProvider === provider.name ? (
                      <>
                        <input type="password" value={editingApiKey} onChange={e => setEditingApiKey(e.target.value)} placeholder="New API key" aria-label={`New API key for ${editingProvider ?? 'provider'}`} />
                        <button className="btn btn--primary" type="button" disabled={cloudBusy || !editingApiKey.trim()} onClick={() => { void handleSaveProviderKey(provider.name); }}>Save key</button>
                        <button className="btn btn--ghost" type="button" onClick={() => { setEditingProvider(null); setEditingApiKey(''); }}>Cancel</button>
                      </>
                    ) : (
                      <>
                        {!provider.env_var_set && <button className="btn btn--ghost" type="button" onClick={() => setEditingProvider(provider.name)}>Set key</button>}
                        {provider.runtime_key_set && !provider.env_var_set && <button className="btn btn--ghost" type="button" onClick={() => { void handleClearProviderKey(provider.name); }}>Clear key</button>}
                        <button className="btn btn--ghost" type="button" onClick={() => { void handleRemoveProvider(provider.name); }}>Remove</button>
                      </>
                    )}
                  </div>
                </div>
              );
            })}
          </div>
        </section>

        <McpPanel connectionStatus={status} />

        <section className="connect__section connect__section--marketplace">
          <div className="connect__section-head">
            <h2>Marketplace</h2>
            <input className="connect__marketplace-search" value={marketplaceSearch} onChange={e => setMarketplaceSearch(e.target.value)} placeholder="Search apps..." aria-label="Search marketplace apps" />
          </div>
          {marketplaceLoading ? <div className="connect__empty">Loading marketplace...</div> : marketplaceError ? <div className="connect__error">Marketplace unavailable: {marketplaceError}</div> : (
            <div className="connect__marketplace-grid">
              {filteredMarketplaceApps.slice(0, 12).map(app => (
                <article key={app.id || app.name} className="connect__marketplace-card">
                  <div className="connect__marketplace-head">
                    {app.logo ? <img src={app.logo} alt="" /> : <span>{app.name.slice(0, 1).toUpperCase()}</span>}
                    <div>
                      <strong>{app.name}</strong>
                      {app.category?.[0] && <span>{app.category[0]}</span>}
                    </div>
                  </div>
                  <p>{app.description || 'No description available.'}</p>
                  <div className="connect__marketplace-actions">
                    {app.links?.app && <button className="btn btn--ghost" type="button" onClick={() => openExternal(app.links?.app)}>Visit</button>}
                    {app.links?.guide && <button className="btn btn--ghost" type="button" onClick={() => openExternal(app.links?.guide)}>Guide</button>}
                    {app.links?.video && <button className="btn btn--ghost" type="button" onClick={() => openExternal(app.links?.video)}>Video</button>}
                  </div>
                </article>
              ))}
              {filteredMarketplaceApps.length === 0 && <div className="connect__empty">No marketplace apps match your search.</div>}
            </div>
          )}
        </section>
      </div>
    </div>
  );
};

export default ConnectView;
