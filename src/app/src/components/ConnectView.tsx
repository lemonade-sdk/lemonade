import React, { useState, useEffect } from 'react';
import api, { ConnectionStatus, friendlyErrorMessage, normalizeBaseUrl } from '../api';
import { AccountSession, clearAllAccountsAndScopedData, clearCurrentSessionData, describeSession } from '../features/accounts/accountStore';

interface ConnectViewProps {
  status: ConnectionStatus;
  accountSession: AccountSession;
  onLocalDataReset: () => void;
  onSessionChange: (session: AccountSession) => void;
}

const ConnectView: React.FC<ConnectViewProps> = ({ status, accountSession, onLocalDataReset, onSessionChange }) => {
  const [host, setHost] = useState(api.baseUrl);
  const [apiKey, setApiKey] = useState(api.apiKey);
  const [canPersistApiKey, setCanPersistApiKey] = useState(api.canPersistApiKey);
  const [rememberApiKey, setRememberApiKey] = useState(api.canPersistApiKey && Boolean(api.apiKey));
  const [connecting, setConnecting] = useState(false);
  const [error, setError] = useState<string | null>(api.lastConnectionError);
  const [notice, setNotice] = useState<string | null>(null);

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

  return (
    <div className="connect">
      <h1>Connect to server</h1>

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
            placeholder="sk-…"
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

        <button
          type="submit"
          className="btn btn--primary"
          disabled={connecting || !host.trim()}
        >
          {connecting ? 'Connecting…' : 'Connect'}
        </button>

        <button
          type="button"
          className="btn btn--ghost"
          onClick={() => { void handleClearLocalData(); }}
        >
          Clear permitted local data
        </button>

        <div className="connect__status">
          <span className={`connect__status-dot ${
            status === 'connected' ? 'connect__status-dot--connected' :
            status === 'connecting' ? 'connect__status-dot--connecting' : ''
          }`} />
          <span>
            {status === 'connected' ? `Connected to ${api.baseUrl}` :
             status === 'connecting' ? `Connecting to ${host || api.baseUrl}…` :
             'Disconnected'}
          </span>
        </div>
      </form>
    </div>
  );
};

export default ConnectView;
