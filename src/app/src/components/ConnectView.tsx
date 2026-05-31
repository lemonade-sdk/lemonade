import React, { useState, useEffect } from 'react';
import api, { ConnectionStatus, friendlyErrorMessage, normalizeBaseUrl } from '../api';

interface ConnectViewProps {
  status: ConnectionStatus;
}

const ConnectView: React.FC<ConnectViewProps> = ({ status }) => {
  const [host, setHost] = useState(api.baseUrl);
  const [apiKey, setApiKey] = useState(api.apiKey);
  const [rememberApiKey, setRememberApiKey] = useState(Boolean(api.apiKey));
  const [connecting, setConnecting] = useState(false);
  const [error, setError] = useState<string | null>(api.lastConnectionError);
  const [notice, setNotice] = useState<string | null>(null);

  useEffect(() => {
    setHost(api.baseUrl);
    setApiKey(api.apiKey);
    setRememberApiKey(Boolean(api.apiKey));
    setError(api.lastConnectionError);
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
      if (rememberApiKey) api.apiKey = apiKey;
      else {
        api.setSessionApiKey(apiKey);
        api.clearStoredApiKey();
      }
      const connected = await api.connect();
      if (!connected) {
        setError(api.lastConnectionError || `Could not connect to ${normalized}.`);
      } else {
        setHost(normalized);
        setNotice(`Connected to ${normalized}.`);
      }
    } catch (err) {
      setError(friendlyErrorMessage(err));
    } finally {
      setConnecting(false);
    }
  };

  const handleClearLocalData = () => {
    const ok = window.confirm('Clear all Lemonade local data on this device? This removes conversations, active chat, presets, server URL, API key, tool settings, and UI preferences from browser storage.');
    if (!ok) return;
    for (const store of [localStorage, sessionStorage]) {
      Object.keys(store).filter(k => k.startsWith('lemonade_')).forEach(k => store.removeItem(k));
    }
    api.setSessionApiKey('');
    setHost(api.baseUrl);
    setApiKey('');
    setRememberApiKey(false);
    setNotice('Local Lemonade data was cleared on this device.');
    setError(null);
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
            type="password"
            value={apiKey}
            onChange={e => setApiKey(e.target.value)}
            onKeyDown={handleKeyDown}
            placeholder="sk-…"
          />
          <label className="connect__checkbox">
            <input
              type="checkbox"
              checked={rememberApiKey}
              onChange={e => setRememberApiKey(e.target.checked)}
            />
            <span>Remember API key in local browser storage</span>
          </label>
          <span className="form-field__hint">When unchecked, the key is only kept in memory for this browser session.</span>
        </div>

        {error && <div className="connect__error">⚠ {error}</div>}
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
          onClick={handleClearLocalData}
        >
          Clear all local data
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
