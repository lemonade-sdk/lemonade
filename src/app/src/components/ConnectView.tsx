import React, { useState, useEffect } from 'react';
import api, { ConnectionStatus } from '../api';

interface ConnectViewProps {
  status: ConnectionStatus;
}

const ConnectView: React.FC<ConnectViewProps> = ({ status }) => {
  const [host, setHost] = useState(api.baseUrl);
  const [apiKey, setApiKey] = useState(api.apiKey);
  const [connecting, setConnecting] = useState(false);

  useEffect(() => {
    setHost(api.baseUrl);
    setApiKey(api.apiKey);
  }, []);

  const handleConnect = async () => {
    setConnecting(true);
    api.baseUrl = host;
    api.apiKey = apiKey;
    await api.connect();
    setConnecting(false);
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
          />
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
        </div>

        <button
          type="submit"
          className="btn btn--primary"
          disabled={connecting || !host.trim()}
        >
          {connecting ? 'Connecting…' : 'Connect'}
        </button>

        <div className="connect__status">
          <span className={`connect__status-dot ${
            status === 'connected' ? 'connect__status-dot--connected' :
            status === 'connecting' ? 'connect__status-dot--connecting' : ''
          }`} />
          <span>
            {status === 'connected' ? 'Connected' :
             status === 'connecting' ? 'Connecting…' :
             'Disconnected'}
          </span>
        </div>
      </form>
    </div>
  );
};

export default ConnectView;
