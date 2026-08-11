import React, { useCallback, useEffect, useMemo, useState } from 'react';
import api, { CloudProviderRow, ConnectionStatus, DirectorySettings, friendlyErrorMessage, normalizeBaseUrl, type LoadedModel, type ModelInfo } from '../api';
import { clearClientStorage } from '../storage';
import { Icon, IconName } from './Icon';
import GlobalModelSettingsPanel from './GlobalModelSettingsPanel';
import McpPanel from './McpPanel';
import WorkspaceSectionRail from './WorkspaceSectionRail';
import { WORKSPACE_NAVIGATION, type ConnectSection } from '../features/navigation/workspaceNavigation';
import {
  WorkspaceActionButton,
  WorkspaceActionGroup,
  WorkspacePaneHeader,
  WorkspaceResourceList,
  WorkspaceResourceRow,
} from './WorkspacePanels';

interface ConnectViewProps {
  status: ConnectionStatus;
  isActive: boolean;
  activeSection: ConnectSection;
  onSectionChange: (section: ConnectSection) => void;
  onLocalDataReset: () => void;
  models: ModelInfo[];
  loadedModels: LoadedModel[];
}

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

const ConnectView: React.FC<ConnectViewProps> = ({ status, isActive, activeSection, onSectionChange, onLocalDataReset, models, loadedModels }) => {
  const [railCollapsed, setRailCollapsed] = useState(false);
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
    if (!isActive) return;
    if (status === 'connected') void loadCloudProviders();
    else setProviders([]);
  }, [isActive, status, loadCloudProviders]);

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
    const ok = window.confirm('Clear Lemonade browser data and connection settings on this device?');
    if (!ok) return;

    clearClientStorage();

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
    setNotice('Local Lemonade data and global connection settings were cleared.');
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

  const openExternal = (url?: string) => {
    if (!url) return;
    const hostApi = (window as unknown as { api?: { openExternal?: (url: string) => void } }).api;
    if (hostApi?.openExternal) {
      hostApi.openExternal(url);
      return;
    }
    window.open(url, '_blank', 'noopener,noreferrer');
  };

  const section = WORKSPACE_NAVIGATION.connect.sections.find(item => item.id === activeSection)
    ?? WORKSPACE_NAVIGATION.connect.sections[0];

  return (
    <div className={`connect connect--workspace${railCollapsed ? ' workspace--rail-collapsed' : ''}`} data-view="connect">
      <WorkspaceSectionRail
        sections={WORKSPACE_NAVIGATION.connect.sections}
        activeSection={activeSection}
        onSectionChange={onSectionChange}
        collapsed={railCollapsed}
        onCollapsedChange={setRailCollapsed}
        panelId="connect-settings-panel"
        railLabel="Connection settings"
        navigationLabel="Connect sections"
        railClassName="connect__rail"
        headerTitle="Settings"
        sidebarLabel="connection settings"
        headerIcon="settings"
        mobileMenuLabel="Open connection settings"
        footer={<div className="workspace-rail__footer">
          <div className="workspace-status" data-status={status} aria-live="polite">
            <span className={`connect__status-dot ${
              status === 'connected' ? 'connect__status-dot--connected' :
              status === 'connecting' ? 'connect__status-dot--connecting' : ''
            }`} />
            <span>
              <strong>{status === 'connected' ? 'Server online' : status === 'connecting' ? 'Connecting' : 'Server offline'}</strong>
              <small>{status === 'connected' ? api.baseUrl : host || api.baseUrl}</small>
            </span>
          </div>
        </div>}
      />

      <section className="workspace-pane connect__main" aria-labelledby="connect-pane-title">
        <WorkspacePaneHeader
          title={section.label}
          subtitle={section.description}
          titleId="connect-pane-title"
        />

        <div className="connect__layout workspace-pane__scroll">
        {activeSection === 'server' && (
        <section className="connect__section connect__section--server">
          <form className="connect__form" onSubmit={e => { e.preventDefault(); handleConnect(); }}>
            <div className="form-field">
              <label className="form-field__label" htmlFor="host-input">Server URL</label>
              <input
                className="input"
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
                className="input"
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

            <WorkspaceActionGroup className="connect__actions" label="Server connection actions">
              <WorkspaceActionButton type="submit" appearance="primary" icon="plug" disabled={connecting || !host.trim()}>
                {connecting ? 'Connecting...' : 'Connect'}
              </WorkspaceActionButton>
              <WorkspaceActionButton appearance="quiet" onClick={() => { void handleClearLocalData(); }}>
                Clear permitted local data
              </WorkspaceActionButton>
            </WorkspaceActionGroup>
          </form>
        </section>
        )}

        {activeSection === 'chat' && (
        <section className="connect__section global-model-settings">
          <GlobalModelSettingsPanel section="chat" models={models} loadedModels={loadedModels} />
        </section>
        )}

        {activeSection === 'memory' && (
        <section className="connect__section global-model-settings">
          <GlobalModelSettingsPanel section="memory" models={models} loadedModels={loadedModels} />
        </section>
        )}

        {activeSection === 'help-and-support' && (
        <section className="connect__section connect__section--help">
          <p className="connect__hint">Quick access to project support, documentation, and community channels.</p>
          <WorkspaceResourceList label="Help links">
            {HELP_LINKS.map(link => (
              <WorkspaceResourceRow
                key={link.href}
                title={link.label}
                description={link.description}
                leading={<Icon name={link.icon} size={18} />}
                onClick={() => openExternal(link.href)}
              />
            ))}
          </WorkspaceResourceList>
        </section>
        )}

        {activeSection === 'model-storage' && (
        <section className="connect__section connect__section--directories">
          <p className="connect__hint">Keep the normal Lemonade model cache separate from an external GGUF directory scanned as extra custom models.</p>
          <div className="connect__directory-grid">
            <label className="form-field"><span className="form-field__label">Models directory</span>
              <input className="input" value={directories.modelsDir} onChange={e => setDirectories(prev => ({ ...prev, modelsDir: e.target.value }))} placeholder="Default Lemonade model cache" />
            </label>
            <label className="form-field"><span className="form-field__label">External custom models directory</span>
              <input className="input" value={directories.extraModelsDir} onChange={e => setDirectories(prev => ({ ...prev, extraModelsDir: e.target.value }))} placeholder="/path/to/llama.cpp/models" />
            </label>
          </div>
          <WorkspaceActionGroup className="connect__actions" label="Directory actions">
            <WorkspaceActionButton appearance="primary" icon="check" onClick={() => { void handleSaveDirectories(); }} disabled={savingDirectories}>
              {savingDirectories ? 'Saving...' : 'Save directories'}
            </WorkspaceActionButton>
          </WorkspaceActionGroup>
          {directoryNotice && <div className="connect__notice">{directoryNotice}</div>}
          {directoryError && <div className="connect__error">{directoryError}</div>}
          <GlobalModelSettingsPanel section="updates" models={models} loadedModels={loadedModels} />
        </section>
        )}

        {activeSection === 'cloud-providers' && (
        <section className="connect__section connect__section--cloud">
          <div className="connect__section-head">
            <WorkspaceActionButton appearance="quiet" size="small" icon="rotate-ccw" onClick={() => { void loadCloudProviders(); }} disabled={status !== 'connected' || cloudBusy || cloudLoading}>{cloudLoading ? 'Refreshing...' : 'Refresh'}</WorkspaceActionButton>
          </div>
          <p className="connect__hint">Register OpenAI-compatible providers on the connected Lemonade server. Runtime keys can be replaced or cleared without editing files.</p>
          <WorkspaceActionGroup className="connect__quick-fill" label="Provider templates">
            {CLOUD_QUICK_FILL.map(item => (
              <WorkspaceActionButton key={item.provider} appearance="secondary" size="small" disabled={cloudBusy} onClick={() => { setProviderName(item.provider); setProviderBaseUrl(item.baseUrl); }}>
                {item.label}
              </WorkspaceActionButton>
            ))}
          </WorkspaceActionGroup>
          <div className="connect__provider-form">
            <label className="sr-only" htmlFor="cloud-provider-name">Provider name</label>
            <input className="input" id="cloud-provider-name" value={providerName} onChange={e => setProviderName(e.target.value)} placeholder="provider name, e.g. fireworks" />
            <label className="sr-only" htmlFor="cloud-provider-url">Base URL</label>
            <input className="input" id="cloud-provider-url" value={providerBaseUrl} onChange={e => setProviderBaseUrl(e.target.value)} placeholder="https://api.example.com/v1" aria-describedby="cloud-provider-url-hint" />
            <label className="sr-only" htmlFor="cloud-provider-key">Provider API key (optional)</label>
            <input className="input" id="cloud-provider-key" value={providerApiKey} onChange={e => setProviderApiKey(e.target.value)} type="password" placeholder="API key (optional)" />
            <WorkspaceActionButton className="connect__add-provider" appearance="primary" icon="plus" onClick={() => { void handleInstallCloudProvider(); }} disabled={status !== 'connected' || cloudBusy}>Add provider</WorkspaceActionButton>
          </div>
          <span id="cloud-provider-url-hint" className="sr-only">Full https:// base URL of the OpenAI-compatible provider endpoint.</span>
          {cloudError && <div className="connect__error">{cloudError}</div>}
          {cloudNotice && <div className="connect__notice">{cloudNotice}</div>}
          <WorkspaceResourceList className="connect__provider-list" label="Cloud providers">
            {providers.length === 0 ? (
              <div className="connect__empty">{status === 'connected' ? (cloudLoading && !cloudLoadedOnce ? 'Loading cloud providers...' : 'No cloud providers configured yet.') : 'Connect to a server to manage cloud providers.'}</div>
            ) : providers.map(provider => {
              const authed = provider.env_var_set || provider.runtime_key_set;
              return (
                <WorkspaceResourceRow
                  key={provider.name}
                  title={provider.name}
                  description={`${provider.models_discovered} models · ${provider.base_url || 'no URL'}`}
                  metadata={authed ? `Auth configured${provider.env_var_set ? ` via ${provider.env_var}` : ''}` : `No API key${provider.env_var ? ` (${provider.env_var})` : ''}`}
                  actions={<WorkspaceActionGroup className="connect__provider-actions" label={`Actions for ${provider.name}`}>
                    {editingProvider === provider.name ? (
                      <>
                        <input className="input" type="password" value={editingApiKey} onChange={e => setEditingApiKey(e.target.value)} placeholder="New API key" aria-label={`New API key for ${editingProvider ?? 'provider'}`} />
                        <WorkspaceActionButton appearance="primary" size="small" disabled={cloudBusy || !editingApiKey.trim()} onClick={() => { void handleSaveProviderKey(provider.name); }}>Save key</WorkspaceActionButton>
                        <WorkspaceActionButton appearance="quiet" size="small" onClick={() => { setEditingProvider(null); setEditingApiKey(''); }}>Cancel</WorkspaceActionButton>
                      </>
                    ) : (
                      <>
                        {!provider.env_var_set && <WorkspaceActionButton appearance="secondary" size="small" onClick={() => setEditingProvider(provider.name)}>Set key</WorkspaceActionButton>}
                        {provider.runtime_key_set && !provider.env_var_set && <WorkspaceActionButton appearance="quiet" size="small" onClick={() => { void handleClearProviderKey(provider.name); }}>Clear key</WorkspaceActionButton>}
                        <WorkspaceActionButton appearance="danger" size="small" icon="trash" onClick={() => { void handleRemoveProvider(provider.name); }}>Remove</WorkspaceActionButton>
                      </>
                    )}
                  </WorkspaceActionGroup>}
                />
              );
            })}
          </WorkspaceResourceList>
        </section>
        )}

        <div hidden={activeSection !== 'mcp-gateway'}>
          <McpPanel connectionStatus={status} isActive={isActive && activeSection === 'mcp-gateway'} />
        </div>

      </div>
      </section>
    </div>
  );
};

export default ConnectView;
