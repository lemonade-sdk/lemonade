import React, { useCallback, useEffect, useMemo, useState } from 'react';
import api, { CloudProviderRow, ConnectionStatus, DirectorySettings, friendlyErrorMessage, normalizeBaseUrl, type LoadedModel, type ModelInfo } from '../api';
import { clearClientStorage } from '../storage';
import { Icon, IconName } from './Icon';
import GlobalModelSettingsPanel from './GlobalModelSettingsPanel';
import McpPanel from './McpPanel';
import WorkspaceSectionRail from './WorkspaceSectionRail';
import { WORKSPACE_NAVIGATION, type ConnectSection } from '../features/navigation/workspaceNavigation';
import { useI18n } from '../i18n';
import type { TranslationParams } from '../i18n/types';
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

const HELP_LINKS: { labelKey: string; href: string; icon: IconName; descriptionKey: string }[] = [
  { labelKey: 'help.documentation', href: 'https://lemonade-server.ai/docs/', icon: 'book-open', descriptionKey: 'help.documentationDescription' },
  { labelKey: 'help.releaseNotes', href: 'https://github.com/lemonade-sdk/lemonade/releases', icon: 'newspaper', descriptionKey: 'help.releaseNotesDescription' },
  { labelKey: 'help.github', href: 'https://github.com/lemonade-sdk/lemonade', icon: 'github', descriptionKey: 'help.githubDescription' },
  { labelKey: 'help.discord', href: 'https://discord.gg/5xXzkMu8Zk', icon: 'discord', descriptionKey: 'help.discordDescription' },
];

const CLOUD_QUICK_FILL = [
  { label: 'Fireworks', provider: 'fireworks', baseUrl: 'https://api.fireworks.ai/inference/v1' },
  { label: 'OpenAI', provider: 'openai', baseUrl: 'https://api.openai.com/v1' },
  { label: 'OpenRouter', provider: 'openrouter', baseUrl: 'https://openrouter.ai/api/v1' },
  { label: 'Together', provider: 'together', baseUrl: 'https://api.together.xyz/v1' },
];

const emptyDirectorySettings: DirectorySettings = { modelsDir: '', extraModelsDir: '', canPersist: false };

type ConnectMessage =
  | { key: string; params?: TranslationParams }
  | { text: string };

function connectMessageText(
  message: ConnectMessage | null,
  translate: (key: string, params?: TranslationParams) => string,
): string | null {
  if (!message) return null;
  return 'key' in message ? translate(message.key, message.params) : message.text;
}

const ConnectView: React.FC<ConnectViewProps> = ({ status, isActive, activeSection, onSectionChange, onLocalDataReset, models, loadedModels }) => {
  const [railCollapsed, setRailCollapsed] = useState(false);
  const { t, locale, preference, availableLocales, setLocalePreference, displayNameForLocale } = useI18n('settings');
  // Keep locale-dependent copy semantic in state and translate it only while rendering.
  const { t: tNavigation } = useI18n('navigation');
  const { t: tCommon } = useI18n('common');
  const [host, setHost] = useState(api.baseUrl);
  const [apiKey, setApiKey] = useState(api.apiKey);
  const [canPersistApiKey, setCanPersistApiKey] = useState(api.canPersistApiKey);
  const [rememberApiKey, setRememberApiKey] = useState(api.canPersistApiKey && Boolean(api.apiKey));
  const [connecting, setConnecting] = useState(false);
  const [error, setError] = useState<ConnectMessage | null>(() => api.lastConnectionError ? { text: api.lastConnectionError } : null);
  const [notice, setNotice] = useState<ConnectMessage | null>(null);
  const [providers, setProviders] = useState<CloudProviderRow[]>([]);
  const [providerName, setProviderName] = useState('');
  const [providerBaseUrl, setProviderBaseUrl] = useState('');
  const [providerApiKey, setProviderApiKey] = useState('');
  const [editingProvider, setEditingProvider] = useState<string | null>(null);
  const [editingApiKey, setEditingApiKey] = useState('');
  const [cloudBusy, setCloudBusy] = useState(false);
  const [cloudLoading, setCloudLoading] = useState(false);
  const [cloudLoadedOnce, setCloudLoadedOnce] = useState(false);
  const [cloudError, setCloudError] = useState<ConnectMessage | null>(null);
  const [cloudNotice, setCloudNotice] = useState<ConnectMessage | null>(null);

  const [directories, setDirectories] = useState<DirectorySettings>(emptyDirectorySettings);
  const [savingDirectories, setSavingDirectories] = useState(false);
  const [directoryNotice, setDirectoryNotice] = useState<ConnectMessage | null>(null);
  const [directoryError, setDirectoryError] = useState<ConnectMessage | null>(null);

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
      setCloudError({ key: 'cloud.unavailable', params: { message: friendlyErrorMessage(err) } });
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
        setError(api.lastConnectionError ? { text: api.lastConnectionError } : null);
      })
      .catch(err => {
        if (cancelled) return;
        setHost(api.baseUrl);
        setApiKey(api.apiKey);
        setCanPersistApiKey(api.canPersistApiKey);
        setRememberApiKey(false);
        setError({ text: friendlyErrorMessage(err) });
      });

    api.loadDirectorySettings()
      .then(settings => { if (!cancelled) setDirectories(settings); })
      .catch(err => { if (!cancelled) setDirectoryError({ text: friendlyErrorMessage(err) }); });

    return () => { cancelled = true; };
  }, []);

  useEffect(() => {
    if (!isActive) return;
    if (status === 'connected') void loadCloudProviders();
    else setProviders([]);
  }, [isActive, status, loadCloudProviders]);

  useEffect(() => {
    setDirectoryNotice(null);
    setDirectoryError(null);
  }, [isActive, activeSection]);

  const handleConnect = async () => {
    setConnecting(true);
    setError(null);
    setNotice(null);
    let normalized: string;
    try {
      normalized = normalizeBaseUrl(host);
    } catch (err) {
      const message = friendlyErrorMessage(err);
      const invalidUrl = /^Invalid server URL:\s*(.*)$/i.exec(message);
      setError(message === 'Server URL is required.'
        ? { key: 'server.urlRequired' }
        : message === 'Server URL must start with http:// or https://.'
          ? { key: 'server.urlProtocol' }
          : invalidUrl
            ? { key: 'server.invalidUrl', params: { url: invalidUrl[1] } }
            : { text: message });
      setConnecting(false);
      return;
    }

    try {
      api.baseUrl = normalized;
      api.setSessionApiKey(apiKey);

      const connected = await api.connect();
      if (!connected) {
        setError(api.lastConnectionError
          ? { text: api.lastConnectionError }
          : { key: 'server.connectFailed', params: { url: normalized } });
        return;
      }

      const saveResult = await api.saveConnectionSettings(normalized, apiKey, canPersistApiKey && rememberApiKey);
      setHost(normalized);
      setCanPersistApiKey(api.canPersistApiKey);
      setRememberApiKey(api.canPersistApiKey && saveResult.apiKeyPersisted);

      if (apiKey && saveResult.apiKeyPersisted) {
        setNotice({ key: 'server.connectedSaved', params: { url: normalized } });
      } else {
        setNotice({ key: 'server.connected', params: { url: normalized } });
      }
      await loadCloudProviders();
    } catch (err) {
      setError({ text: friendlyErrorMessage(err) });
    } finally {
      setConnecting(false);
    }
  };

  const handleClearLocalData = async () => {
    const ok = window.confirm(t('server.clearConfirm'));
    if (!ok) return;

    clearClientStorage();

    for (const store of [localStorage, sessionStorage]) {
      Object.keys(store)
        .filter(k => k === 'lemonade_base_url' || k === 'lemonade_api_key' || k === 'lemonade_current_view' || k === 'lemonade_theme')
        .forEach(k => store.removeItem(k));
    }

    let clearSettingsError: ConnectMessage | null = null;
    try {
      await api.clearConnectionSettings();
    } catch (err) {
      clearSettingsError = { key: 'server.clearPartialError', params: { message: friendlyErrorMessage(err) } };
    }

    api.setSessionApiKey('');
    setHost(api.baseUrl);
    setApiKey('');
    setCanPersistApiKey(api.canPersistApiKey);
    setRememberApiKey(false);
    onLocalDataReset();
    setNotice({ key: 'server.clearedNotice' });
    setError(clearSettingsError);
  };

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter') handleConnect();
  };

  const handleInstallCloudProvider = async () => {
    if (!providerName.trim() || !providerBaseUrl.trim()) {
      setCloudError({ key: 'cloud.providerRequired' });
      return;
    }
    setCloudBusy(true);
    setCloudError(null);
    setCloudNotice(null);
    try {
      const result = await api.installCloudProvider(providerName, providerBaseUrl, providerApiKey);
      setCloudNotice({ key: 'cloud.installed', params: { provider: providerName.trim(), count: Number(result.models_discovered || 0) } });
      setProviderName('');
      setProviderBaseUrl('');
      setProviderApiKey('');
      await loadCloudProviders();
    } catch (err) {
      setCloudError({ text: friendlyErrorMessage(err) });
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
      setCloudNotice({ key: 'cloud.keySaved', params: { provider, count: Number(result.models_discovered || 0) } });
      setEditingProvider(null);
      setEditingApiKey('');
      await loadCloudProviders();
    } catch (err) {
      setCloudError({ text: friendlyErrorMessage(err) });
    } finally {
      setCloudBusy(false);
    }
  };

  const handleClearProviderKey = async (provider: string) => {
    setCloudBusy(true);
    setCloudError(null);
    try {
      await api.clearCloudProviderAuth(provider);
      setCloudNotice({ key: 'cloud.keyCleared', params: { provider } });
      await loadCloudProviders();
    } catch (err) {
      setCloudError({ text: friendlyErrorMessage(err) });
    } finally {
      setCloudBusy(false);
    }
  };

  const handleRemoveProvider = async (provider: string) => {
    if (!window.confirm(t('cloud.removeConfirm', { provider }))) return;
    setCloudBusy(true);
    setCloudError(null);
    try {
      await api.uninstallCloudProvider(provider);
      setCloudNotice({ key: 'cloud.removed', params: { provider } });
      await loadCloudProviders();
    } catch (err) {
      setCloudError({ text: friendlyErrorMessage(err) });
    } finally {
      setCloudBusy(false);
    }
  };

  const handleDirectoryChange = (key: 'modelsDir' | 'extraModelsDir', value: string) => {
    setDirectories(prev => ({ ...prev, [key]: value }));
    setDirectoryNotice(null);
    setDirectoryError(null);
  };

  const handleSaveDirectories = async () => {
    setSavingDirectories(true);
    setDirectoryError(null);
    setDirectoryNotice(null);
    try {
      const saved = await api.saveDirectorySettings(directories.modelsDir, directories.extraModelsDir);
      setDirectories(saved);
      setDirectoryNotice({ key: saved.canPersist ? 'storage.saved' : 'storage.notPersisted' });
    } catch (err) {
      setDirectoryError({ text: friendlyErrorMessage(err) });
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
        railLabel={tNavigation('rail.connectionSettings')}
        navigationLabel={tNavigation('rail.connectSections')}
        railClassName="connect__rail"
        headerTitle={tNavigation('rail.settings')}
        sidebarLabel={tNavigation('rail.connectionSidebar')}
        headerIcon="settings"
        mobileMenuLabel={tNavigation('rail.openConnectionSettings')}
        footer={<div className="workspace-rail__footer">
          <div className="workspace-status" data-status={status} aria-live="polite">
            <span className={`connect__status-dot ${
              status === 'connected' ? 'connect__status-dot--connected' :
              status === 'connecting' ? 'connect__status-dot--connecting' : ''
            }`} />
            <span>
              <strong>{status === 'connected' ? t('status.online') : status === 'connecting' ? t('status.connecting') : t('status.offline')}</strong>
              <small>{status === 'connected' ? api.baseUrl : host || api.baseUrl}</small>
            </span>
          </div>
        </div>}
      />

      <section className="workspace-pane connect__main" aria-labelledby="connect-pane-title">
        <WorkspacePaneHeader
          title={tNavigation(section.labelKey)}
          subtitle={tNavigation(section.descriptionKey)}
          titleId="connect-pane-title"
        />

        <div className="connect__layout workspace-pane__scroll">
        {activeSection === 'server' && (
        <section className="connect__section connect__section--server">
          <form className="connect__form" onSubmit={e => { e.preventDefault(); handleConnect(); }}>
            <div className="form-field">
              <label className="form-field__label" htmlFor="host-input">{t('server.url')}</label>
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
              <span className="form-field__hint">{t('server.urlHint')}</span>
            </div>

            <div className="form-field">
              <label className="form-field__label" htmlFor="key-input">{t('server.apiKey')}</label>
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
                  <span>{t('server.rememberApiKey')}</span>
                </label>
              )}
            </div>

            {error && <div className="connect__error">{t('server.warning', { message: connectMessageText(error, t) || '' })}</div>}
            {notice && <div className="connect__notice">{connectMessageText(notice, t)}</div>}

            <WorkspaceActionGroup className="connect__actions" label={t('server.actions')}>
              <WorkspaceActionButton type="submit" appearance="primary" icon="plug" disabled={connecting || !host.trim()}>
                {connecting ? t('server.connecting') : tCommon('actions.connect')}
              </WorkspaceActionButton>
              <WorkspaceActionButton appearance="quiet" onClick={() => { void handleClearLocalData(); }}>
                {t('server.clearLocalData')}
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

        {activeSection === 'language' && (
        <section className="connect__section connect__section--language">
          <p className="connect__hint">{t('language.intro')}</p>
          <label className="form-field" htmlFor="interface-language">
            <span className="form-field__label">{t('language.label')}</span>
            <select
              className="input"
              id="interface-language"
              value={preference ?? ''}
              onChange={event => setLocalePreference(event.target.value || null)}
            >
              <option value="">
                {t('language.systemOption', { language: displayNameForLocale(locale, locale) })}
              </option>
              {availableLocales.map(option => (
                <option key={option.locale} value={option.locale}>
                  {displayNameForLocale(option.locale, option.locale)}
                </option>
              ))}
            </select>
          </label>
          <div className="connect__language-meta">
            <p>{t('language.active', { locale })}</p>
            <p>{t('language.fallback')}</p>
          </div>
        </section>
        )}

        {activeSection === 'help-and-support' && (
        <section className="connect__section connect__section--help">
          <p className="connect__hint">{t('help.intro')}</p>
          <WorkspaceResourceList label={t('help.links')}>
            {HELP_LINKS.map(link => (
              <WorkspaceResourceRow
                key={link.href}
                title={t(link.labelKey)}
                description={t(link.descriptionKey)}
                leading={<Icon name={link.icon} size={18} />}
                onClick={() => openExternal(link.href)}
              />
            ))}
          </WorkspaceResourceList>
        </section>
        )}

        {activeSection === 'model-storage' && (
        <section className="connect__section connect__section--directories">
          <p className="connect__hint">{t('storage.intro')}</p>
          <div className="connect__directory-grid">
            <label className="form-field"><span className="form-field__label">{t('storage.modelsDirectory')}</span>
              <input className="input" value={directories.modelsDir} onChange={e => handleDirectoryChange('modelsDir', e.target.value)} placeholder={t('storage.modelsPlaceholder')} />
            </label>
            <label className="form-field"><span className="form-field__label">{t('storage.externalDirectory')}</span>
              <input className="input" value={directories.extraModelsDir} onChange={e => handleDirectoryChange('extraModelsDir', e.target.value)} placeholder="/path/to/llama.cpp/models" />
            </label>
          </div>
          <WorkspaceActionGroup className="connect__actions" label={t('storage.directoryActions')}>
            <WorkspaceActionButton appearance="primary" icon="check" onClick={() => { void handleSaveDirectories(); }} disabled={savingDirectories}>
              {savingDirectories ? t('storage.saving') : t('storage.save')}
            </WorkspaceActionButton>
          </WorkspaceActionGroup>
          {directoryNotice && <div className="connect__notice">{connectMessageText(directoryNotice, t)}</div>}
          {directoryError && <div className="connect__error">{connectMessageText(directoryError, t)}</div>}
          <GlobalModelSettingsPanel section="updates" models={models} loadedModels={loadedModels} />
        </section>
        )}

        {activeSection === 'cloud-providers' && (
        <section className="connect__section connect__section--cloud">
          <div className="connect__section-head">
            <WorkspaceActionButton appearance="quiet" size="small" icon="rotate-ccw" onClick={() => { void loadCloudProviders(); }} disabled={status !== 'connected' || cloudBusy || cloudLoading}>{cloudLoading ? t('cloud.refreshing') : t('cloud.refresh')}</WorkspaceActionButton>
          </div>
          <p className="connect__hint">{t('cloud.intro')}</p>
          <WorkspaceActionGroup className="connect__quick-fill" label={t('cloud.templates')}>
            {CLOUD_QUICK_FILL.map(item => (
              <WorkspaceActionButton key={item.provider} appearance="secondary" size="small" disabled={cloudBusy} onClick={() => { setProviderName(item.provider); setProviderBaseUrl(item.baseUrl); }}>
                {item.label}
              </WorkspaceActionButton>
            ))}
          </WorkspaceActionGroup>
          <div className="connect__provider-form">
            <label className="sr-only" htmlFor="cloud-provider-name">{t('cloud.providerName')}</label>
            <input className="input" id="cloud-provider-name" value={providerName} onChange={e => setProviderName(e.target.value)} placeholder={t('cloud.providerNamePlaceholder')} />
            <label className="sr-only" htmlFor="cloud-provider-url">{t('cloud.baseUrl')}</label>
            <input className="input" id="cloud-provider-url" value={providerBaseUrl} onChange={e => setProviderBaseUrl(e.target.value)} placeholder="https://api.example.com/v1" aria-describedby="cloud-provider-url-hint" />
            <label className="sr-only" htmlFor="cloud-provider-key">{t('cloud.apiKeyOptional')}</label>
            <input className="input" id="cloud-provider-key" value={providerApiKey} onChange={e => setProviderApiKey(e.target.value)} type="password" placeholder={t('cloud.apiKeyPlaceholder')} />
            <WorkspaceActionButton className="connect__add-provider" appearance="primary" icon="plus" onClick={() => { void handleInstallCloudProvider(); }} disabled={status !== 'connected' || cloudBusy}>{t('cloud.addProvider')}</WorkspaceActionButton>
          </div>
          <span id="cloud-provider-url-hint" className="sr-only">{t('cloud.baseUrlHint')}</span>
          {cloudError && <div className="connect__error">{connectMessageText(cloudError, t)}</div>}
          {cloudNotice && <div className="connect__notice">{connectMessageText(cloudNotice, t)}</div>}
          <WorkspaceResourceList className="connect__provider-list" label={t('cloud.list')}>
            {providers.length === 0 ? (
              <div className="connect__empty">{status === 'connected' ? (cloudLoading && !cloudLoadedOnce ? t('cloud.loading') : t('cloud.empty')) : t('cloud.connectFirst')}</div>
            ) : providers.map(provider => {
              const authed = provider.env_var_set || provider.runtime_key_set;
              return (
                <WorkspaceResourceRow
                  key={provider.name}
                  title={provider.name}
                  description={t('cloud.models', { count: provider.models_discovered, url: provider.base_url || t('cloud.noUrl') })}
                  metadata={authed ? (provider.env_var_set ? t('cloud.authVia', { variable: provider.env_var || '' }) : t('cloud.authConfigured')) : (provider.env_var ? t('cloud.noApiKeyVariable', { variable: provider.env_var }) : t('cloud.noApiKey'))}
                  actions={<WorkspaceActionGroup className="connect__provider-actions" label={t('cloud.actionsFor', { name: provider.name })}>
                    {editingProvider === provider.name ? (
                      <>
                        <input className="input" type="password" value={editingApiKey} onChange={e => setEditingApiKey(e.target.value)} placeholder={t('cloud.newApiKey')} aria-label={t('cloud.newApiKeyFor', { name: editingProvider ?? t('cloud.providerFallback') })} />
                        <WorkspaceActionButton appearance="primary" size="small" disabled={cloudBusy || !editingApiKey.trim()} onClick={() => { void handleSaveProviderKey(provider.name); }}>{t('cloud.saveKey')}</WorkspaceActionButton>
                        <WorkspaceActionButton appearance="quiet" size="small" onClick={() => { setEditingProvider(null); setEditingApiKey(''); }}>{tCommon('actions.cancel')}</WorkspaceActionButton>
                      </>
                    ) : (
                      <>
                        {!provider.env_var_set && <WorkspaceActionButton appearance="secondary" size="small" onClick={() => setEditingProvider(provider.name)}>{t('cloud.setKey')}</WorkspaceActionButton>}
                        {provider.runtime_key_set && !provider.env_var_set && <WorkspaceActionButton appearance="quiet" size="small" onClick={() => { void handleClearProviderKey(provider.name); }}>{t('cloud.clearKey')}</WorkspaceActionButton>}
                        <WorkspaceActionButton appearance="danger" size="small" icon="trash" onClick={() => { void handleRemoveProvider(provider.name); }}>{t('cloud.remove')}</WorkspaceActionButton>
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
