import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import api, { ConnectionStatus, McpServerState, friendlyErrorMessage } from '../api';

const MCP_PROTOCOL_VERSION = '2025-06-18';
const CLIENT_VERSION = '0.1.0';

type GatewayStatus = 'idle' | 'checking' | 'connected' | 'unavailable';

interface GatewayTool {
  name: string;
  description?: string;
  inputSchema?: Record<string, unknown>;
}

interface ServerDraft {
  id: string;
  name: string;
  command: string;
  args: string;
  workingDir: string;
  timeoutMs: string;
  env: string;
}

const EMPTY_DRAFT: ServerDraft = {
  id: '',
  name: '',
  command: '',
  args: '',
  workingDir: '',
  timeoutMs: '30000',
  env: '',
};

export interface McpPanelProps {
  connectionStatus: ConnectionStatus;
  isActive: boolean;
}

function mcpHeaders(sessionId?: string, includeProtocol = false): Record<string, string> {
  const headers: Record<string, string> = { 'Content-Type': 'application/json' };
  const credential = api.apiKey || api.adminApiKey;
  if (credential) headers.Authorization = `Bearer ${credential}`;
  if (includeProtocol) headers['MCP-Protocol-Version'] = MCP_PROTOCOL_VERSION;
  if (sessionId) headers['Mcp-Session-Id'] = sessionId;
  return headers;
}

function requestSignal(parent: AbortSignal, timeoutMs: number): AbortSignal {
  const timeout = AbortSignal.timeout(timeoutMs);
  const any = (AbortSignal as unknown as { any?: (signals: AbortSignal[]) => AbortSignal }).any;
  return any ? any([parent, timeout]) : parent;
}

function parseEnv(text: string): Record<string, string> {
  const env: Record<string, string> = {};
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line) continue;
    const equals = line.indexOf('=');
    if (equals < 1) throw new Error(`Invalid environment line: ${line}`);
    const key = line.slice(0, equals).trim();
    const value = line.slice(equals + 1).trim();
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(key)) throw new Error(`Invalid environment variable: ${key}`);
    if (value !== `\${${key}}`) throw new Error(`${key} must use the safe reference \${${key}}; raw secrets are not stored.`);
    env[key] = value;
  }
  return env;
}

function draftFromServer(server: McpServerState): ServerDraft {
  return {
    id: server.id,
    name: server.name,
    command: server.command || '',
    args: (server.args || []).join('\n'),
    workingDir: server.working_dir || '',
    timeoutMs: String(server.timeout_ms || 30000),
    env: Object.entries(server.env || {}).map(([key, value]) => `${key}=${value}`).join('\n'),
  };
}

const McpPanel: React.FC<McpPanelProps> = ({ connectionStatus, isActive }) => {
  const [gatewayStatus, setGatewayStatus] = useState<GatewayStatus>('idle');
  const [gatewayTools, setGatewayTools] = useState<GatewayTool[]>([]);
  const [gatewayError, setGatewayError] = useState('');
  const [servers, setServers] = useState<McpServerState[]>([]);
  const [hostError, setHostError] = useState('');
  const [hostLoading, setHostLoading] = useState(false);
  const [secure, setSecure] = useState<boolean | null>(null);
  const [adminAccess, setAdminAccess] = useState<'checking' | 'ok' | 'needs-admin' | 'unavailable'>('checking');
  const [adminKeyDraft, setAdminKeyDraft] = useState(() => api.explicitAdminApiKey);
  const [adminKeyNotice, setAdminKeyNotice] = useState('');
  const [busyId, setBusyId] = useState('');
  const [showForm, setShowForm] = useState(false);
  const [draft, setDraft] = useState<ServerDraft>(EMPTY_DRAFT);
  const [formError, setFormError] = useState('');
  const [copyNotice, setCopyNotice] = useState('');
  const abortRef = useRef<AbortController | null>(null);
  const copyTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const mcpUrl = `${api.baseUrl}/mcp`;

  const loadGatewayTools = useCallback(async () => {
    abortRef.current?.abort();
    const controller = new AbortController();
    abortRef.current = controller;
    const { signal } = controller;
    setGatewayStatus('checking');
    setGatewayError('');
    try {
      const init = await fetch(mcpUrl, {
        method: 'POST',
        headers: mcpHeaders(),
        body: JSON.stringify({
          jsonrpc: '2.0', id: 1, method: 'initialize',
          params: {
            protocolVersion: MCP_PROTOCOL_VERSION,
            capabilities: { tools: {} },
            clientInfo: { name: 'lemonade-gui3', version: CLIENT_VERSION },
          },
        }),
        signal: requestSignal(signal, 8000),
      });
      if (!init.ok) throw new Error(`initialize HTTP ${init.status}`);
      const initBody = await init.json() as { result?: { protocolVersion?: string }; error?: { message?: string } };
      if (initBody.error || !initBody.result?.protocolVersion) throw new Error(initBody.error?.message || 'Invalid initialize response');
      const sessionId = init.headers.get('Mcp-Session-Id') || undefined;
      await fetch(mcpUrl, {
        method: 'POST',
        headers: mcpHeaders(sessionId, true),
        body: JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }),
        signal,
      }).catch(() => undefined);
      const list = await fetch(mcpUrl, {
        method: 'POST',
        headers: mcpHeaders(sessionId, true),
        body: JSON.stringify({ jsonrpc: '2.0', id: 2, method: 'tools/list' }),
        signal: requestSignal(signal, 8000),
      });
      if (!list.ok) throw new Error(`tools/list HTTP ${list.status}`);
      const listBody = await list.json() as { result?: { tools?: GatewayTool[] }; error?: { message?: string } };
      if (listBody.error) throw new Error(listBody.error.message || 'tools/list failed');
      if (!signal.aborted) {
        setGatewayTools(Array.isArray(listBody.result?.tools) ? listBody.result!.tools! : []);
        setGatewayStatus('connected');
      }
    } catch (error) {
      if ((error as { name?: string }).name !== 'AbortError') {
        setGatewayError(friendlyErrorMessage(error));
        setGatewayStatus('unavailable');
      }
    }
  }, [mcpUrl]);

  const probeAccess = useCallback(async (): Promise<'ok' | 'needs-admin' | 'unavailable'> => {
    setHostLoading(true);
    setHostError('');
    const result = await api.probeMcpAccess();
    setHostLoading(false);
    if (result.ok) {
      setServers(result.servers);
      setAdminAccess('ok');
      return 'ok';
    }
    setServers([]);
    if (result.status === 401 || result.status === 403) {
      setAdminAccess('needs-admin');
      return 'needs-admin';
    }
    setAdminAccess('unavailable');
    setHostError(result.status
      ? `Could not reach MCP administration (HTTP ${result.status}).`
      : 'Could not reach MCP administration. Check that the server is running and reachable.');
    return 'unavailable';
  }, []);

  useEffect(() => {
    if (!isActive || connectionStatus !== 'connected') {
      abortRef.current?.abort();
      setGatewayStatus('idle');
      setGatewayTools([]);
      setServers([]);
      setSecure(null);
      setAdminAccess('checking');
      return;
    }
    const flag = api.highSecurity;
    setSecure(flag === false ? false : true);
    void loadGatewayTools();
    if (flag !== false) {
      setAdminAccess('checking');
      void probeAccess();
    }
    return () => abortRef.current?.abort();
  }, [connectionStatus, isActive, loadGatewayTools, probeAccess]);

  const gatewayLabel = gatewayStatus === 'connected' ? 'Connected'
    : gatewayStatus === 'checking' ? 'Checking…'
      : gatewayStatus === 'unavailable' ? 'Unavailable' : 'Not checked';

  const connectedExternal = useMemo(() => servers.filter(server => server.connected).length, [servers]);

  const applyAdminKey = async () => {
    api.setSessionAdminApiKey(adminKeyDraft);
    setAdminKeyNotice('');
    setAdminAccess('checking');
    const outcome = await probeAccess();
    if (outcome === 'ok') {
      setAdminKeyNotice('Admin key applied for this app session.');
    } else if (outcome === 'needs-admin') {
      setHostError('Admin API key was rejected.');
    }
  };

  const runServerAction = async (id: string, action: 'connect' | 'disconnect' | 'refresh' | 'remove') => {
    setBusyId(id);
    setHostError('');
    try {
      if (action === 'connect') await api.connectMcpServer(id);
      else if (action === 'disconnect') await api.disconnectMcpServer(id);
      else if (action === 'refresh') await api.refreshMcpServerTools(id);
      else await api.removeMcpServer(id);
      await probeAccess();
    } catch (error) {
      setHostError(friendlyErrorMessage(error));
    } finally {
      setBusyId('');
    }
  };

  const saveServer = async (event: React.FormEvent) => {
    event.preventDefault();
    setFormError('');
    setBusyId(draft.id || '__new__');
    try {
      if (!draft.name.trim()) throw new Error('Name is required.');
      if (!draft.command.trim()) throw new Error('Command is required.');
      const timeout = Number(draft.timeoutMs);
      if (!Number.isInteger(timeout) || timeout < 1000 || timeout > 300000) throw new Error('Timeout must be between 1000 and 300000 ms.');
      const saved = await api.saveMcpServer({
        ...(draft.id ? { id: draft.id } : {}),
        name: draft.name.trim(),
        command: draft.command.trim(),
        args: draft.args.split(/\r?\n/).map(value => value.trim()).filter(Boolean),
        env: parseEnv(draft.env),
        working_dir: draft.workingDir.trim(),
        timeout_ms: timeout,
        enabled: true,
      });
      await api.connectMcpServer(saved.id);
      await probeAccess();
      setDraft(EMPTY_DRAFT);
      setShowForm(false);
    } catch (error) {
      setFormError(friendlyErrorMessage(error));
    } finally {
      setBusyId('');
    }
  };

  const copyEndpoint = async () => {
    try {
      await navigator.clipboard.writeText(mcpUrl);
      setCopyNotice('Copied');
    } catch {
      setCopyNotice('Select and copy the URL manually');
    }
    if (copyTimerRef.current) clearTimeout(copyTimerRef.current);
    copyTimerRef.current = setTimeout(() => setCopyNotice(''), 2500);
  };

  return (
    <section className="connect__section connect__section--mcp" aria-labelledby="mcp-section-title" data-mcp-panel>
      <h2 id="mcp-section-title">MCP Gateway</h2>
      <p className="connect__hint">
        Lemonade works in both directions: its built-in tools are exposed as a Streamable HTTP MCP server,
        while the Chat UI can host external stdio MCP servers. External commands run on the same machine as lemond.
      </p>

      <div className="mcp-panel">
        <section className="mcp-panel__card" aria-labelledby="lemonade-mcp-title">
          <div className="mcp-panel__card-header">
            <div><h3 id="lemonade-mcp-title">Lemon-Tools MCP server</h3><p>Use Lemonade from Claude, VS Code, Cursor, MCP Inspector, or another MCP client.</p></div>
            <div className={`mcp-panel__status mcp-panel__status--${gatewayStatus}`} role="status" aria-live="polite" aria-atomic="true" data-mcp-status><span className="mcp-panel__status-dot" />{gatewayLabel}</div>
          </div>
          <div className="mcp-panel__url-copy-row">
            <input id="mcp-endpoint-display" className="mcp-panel__url-input" value={mcpUrl} readOnly aria-label="Lemon-Tools MCP endpoint URL" />
            <button type="button" className="btn btn--ghost mcp-panel__copy-btn" aria-label="Copy MCP endpoint URL to clipboard" onClick={() => void copyEndpoint()}>Copy</button>
            <button type="button" className="btn btn--ghost" aria-label="Refresh MCP tools list" onClick={() => void loadGatewayTools()} disabled={gatewayStatus === 'checking'}>Refresh</button>
          </div>
          <div className="sr-only" role="status" aria-live="polite" aria-atomic="true" data-mcp-copy-live>{copyNotice}</div>
          {gatewayError && <div className="connect__error" role="alert" data-mcp-tools-error>Could not load MCP tools: {gatewayError}</div>}
          {gatewayStatus === 'connected' && gatewayTools.length > 0 ? (
            <div className="mcp-panel__tool-chips" aria-label="Lemon-Tools MCP tools" data-mcp-tools-list>
              {gatewayTools.map(tool => <span key={tool.name} className="mcp-panel__tool-name" title={tool.description || tool.name}>{tool.name}</span>)}
            </div>
          ) : gatewayStatus === 'connected' ? <p className="connect__empty">No tools returned.</p> : null}
        </section>

        <section className="mcp-panel__card" aria-labelledby="external-mcp-title">
          <div className="mcp-panel__card-header">
            <div>
              <h3 id="external-mcp-title">External MCP servers</h3>
              <p>{connectedExternal}/{servers.length} connected · stdio transport · select up to four per preset.</p>
            </div>
            {secure === true && adminAccess === 'ok' && (
              <button
                type="button"
                className="btn btn--primary"
                onClick={() => { setDraft(EMPTY_DRAFT); setFormError(''); setShowForm(value => !value); }}
                disabled={connectionStatus !== 'connected'}
              >
                {showForm ? 'Cancel' : 'Add server'}
              </button>
            )}
          </div>

          {secure === null || (secure === true && adminAccess === 'checking') ? (
            <p className="connect__empty">Checking MCP administration access…</p>
          ) : secure === false ? (
            <div className="connect__notice mcp-panel__security-warning" role="note" data-mcp-security-warning>
              <p><strong>External MCP servers are unavailable on this server.</strong></p>
              <p>
                Due to security constraints, using external MCP servers requires your server to be set up with either a
                general API key (<code>LEMONADE_API_KEY</code>) or a dedicated admin API key (<code>LEMONADE_ADMIN_API_KEY</code>).
                Please set the respective environment variable in your Lemonade Server launch script, then restart the
                server to use this feature.
              </p>
            </div>
          ) : adminAccess === 'unavailable' ? (
            <div className="connect__notice mcp-panel__host-unavailable" role="alert" data-mcp-host-unavailable>
              <p>{hostError || 'MCP administration is currently unavailable.'}</p>
              <button type="button" className="btn btn--ghost" onClick={() => void probeAccess()} disabled={connectionStatus !== 'connected' || hostLoading}>
                {hostLoading ? 'Retrying…' : 'Retry'}
              </button>
            </div>
          ) : adminAccess === 'needs-admin' ? (
            <div className="mcp-panel__admin-auth" data-mcp-admin-auth>
              <div>
                <label htmlFor="mcp-admin-key">Admin API key</label>
                <p>Server requires admin API key to access external MCP feature setup.</p>
              </div>
              <div className="mcp-panel__admin-auth-controls">
                <input
                  id="mcp-admin-key"
                  type="password"
                  autoComplete="off"
                  value={adminKeyDraft}
                  onChange={event => setAdminKeyDraft(event.target.value)}
                  onKeyDown={event => { if (event.key === 'Enter') { event.preventDefault(); void applyAdminKey(); } }}
                  placeholder="Admin API key"
                />
                <button type="button" className="btn btn--primary" onClick={() => void applyAdminKey()} disabled={connectionStatus !== 'connected' || hostLoading || !adminKeyDraft.trim()}>Apply</button>
              </div>
              {adminKeyNotice && <div className="connect__notice" role="status">{adminKeyNotice}</div>}
              {hostError && <div className="connect__error" role="alert">{hostError}</div>}
            </div>
          ) : (
            <>
          {showForm && (
            <form className="mcp-server-form" onSubmit={saveServer}>
              <label><span>Name</span><input value={draft.name} onChange={event => setDraft(current => ({ ...current, name: event.target.value }))} placeholder="Filesystem" /></label>
              <label><span>Command</span><input value={draft.command} onChange={event => setDraft(current => ({ ...current, command: event.target.value }))} placeholder="npx" /></label>
              <label className="mcp-server-form__wide"><span>Arguments · one per line</span><textarea value={draft.args} onChange={event => setDraft(current => ({ ...current, args: event.target.value }))} placeholder={'-y\n@modelcontextprotocol/server-filesystem\n/home/user/projects'} rows={4} /></label>
              <label><span>Working directory · optional</span><input value={draft.workingDir} onChange={event => setDraft(current => ({ ...current, workingDir: event.target.value }))} /></label>
              <label><span>Timeout (ms)</span><input type="number" min={1000} max={300000} value={draft.timeoutMs} onChange={event => setDraft(current => ({ ...current, timeoutMs: event.target.value }))} /></label>
              <label className="mcp-server-form__wide"><span>Environment references · one <code>{'KEY=${KEY}'}</code> per line</span><textarea value={draft.env} onChange={event => setDraft(current => ({ ...current, env: event.target.value }))} placeholder="GITHUB_TOKEN=${GITHUB_TOKEN}" rows={3} /></label>
              <p className="mcp-server-form__note">Secrets are never persisted as raw values. The referenced variable must exist in the lemond process environment.</p>
              {formError && <div className="connect__error mcp-server-form__wide" role="alert">{formError}</div>}
              <div className="mcp-server-form__actions mcp-server-form__wide"><button className="btn btn--primary" type="submit" disabled={busyId === '__new__' || busyId === draft.id}>{busyId ? 'Saving…' : 'Save and connect'}</button></div>
            </form>
          )}

          {hostError && <div className="connect__error" role="alert">{hostError}</div>}
          {hostLoading ? <p className="connect__empty">Loading MCP servers…</p> : servers.length === 0 ? (
            <p className="connect__empty">No external MCP server configured. Lemon-Tools MCP remains available in presets.</p>
          ) : (
            <div className="mcp-server-list">
              {servers.map(server => (
                <article className="mcp-server-card" key={server.id}>
                  <div className="mcp-server-card__main">
                    <span className={`mcp-panel__status-dot${server.connected ? ' is-connected' : ''}`} aria-hidden="true" />
                    <div>
                      <strong>{server.name}</strong>
                      <code>{[server.command, ...(server.args || [])].filter(Boolean).join(' ')}</code>
                      <small>{server.connected ? `${server.tools?.length || 0} tools · protocol ${server.protocol_version || 'unknown'}` : server.last_error || server.status}</small>
                    </div>
                  </div>
                  {server.tools && server.tools.length > 0 && <div className="mcp-panel__tool-chips">{server.tools.map(tool => <span key={tool.name}>{tool.name}</span>)}</div>}
                  <div className="mcp-server-card__actions">
                    <button type="button" className="btn btn--ghost" onClick={() => { setDraft(draftFromServer(server)); setShowForm(true); setFormError(''); }}>Edit</button>
                    {server.connected ? (
                      <><button type="button" className="btn btn--ghost" onClick={() => void runServerAction(server.id, 'refresh')} disabled={busyId === server.id}>Refresh tools</button><button type="button" className="btn btn--ghost" onClick={() => void runServerAction(server.id, 'disconnect')} disabled={busyId === server.id}>Disconnect</button></>
                    ) : <button type="button" className="btn btn--primary" onClick={() => void runServerAction(server.id, 'connect')} disabled={busyId === server.id}>Connect</button>}
                    <button type="button" className="btn btn--danger" onClick={() => void runServerAction(server.id, 'remove')} disabled={busyId === server.id}>Remove</button>
                  </div>
                </article>
              ))}
            </div>
          )}
            </>
          )}
        </section>
      </div>
    </section>
  );
};

export default McpPanel;
