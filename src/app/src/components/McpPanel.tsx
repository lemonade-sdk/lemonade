import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import api, {
  ConnectionStatus,
  McpServerConfig,
  McpServerState,
  friendlyErrorMessage,
} from '../api';
import { translate, useI18n } from '../i18n';

const MCP_PROTOCOL_VERSION = '2025-11-25';
const CLIENT_VERSION = '0.1.0';

type GatewayStatus = 'idle' | 'checking' | 'connected' | 'unavailable';
type ExternalMcpTransport = 'streamable-http' | 'stdio';
type HttpAuthentication = 'none' | 'bearer-env';
type SaveableMcpServer = Omit<McpServerConfig, 'id' | 'transport'> & {
  id?: string;
  transport: ExternalMcpTransport;
};

interface GatewayTool {
  name: string;
  description?: string;
  inputSchema?: Record<string, unknown>;
}

interface ServerDraft {
  id: string;
  name: string;
  transport: ExternalMcpTransport;
  url: string;
  authentication: HttpAuthentication;
  tokenEnvironmentVariable: string;
  allowInsecureHttp: boolean;
  command: string;
  args: string;
  workingDir: string;
  timeoutMs: string;
  env: string;
}

const EMPTY_DRAFT: ServerDraft = {
  id: '',
  name: '',
  transport: 'streamable-http',
  url: '',
  authentication: 'none',
  tokenEnvironmentVariable: '',
  allowInsecureHttp: false,
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

function toolInputMetadata(inputSchema: Record<string, unknown> | undefined, locale: Parameters<typeof translate>[1]): string {
  if (!inputSchema) return translate('Input schema unavailable', locale);
  const properties = inputSchema.properties;
  if (!properties || typeof properties !== 'object' || Array.isArray(properties)) {
    return translate('No input parameters', locale);
  }

  const inputCount = Object.keys(properties).length;
  if (inputCount === 0) return translate('No input parameters', locale);

  const required = Array.isArray(inputSchema.required)
    ? inputSchema.required.filter(value => typeof value === 'string').length
    : 0;
  return translate(inputCount === 1 ? '1 input' : '{count} input parameters', locale, { count: inputCount })
    + (required ? ` · ${translate('{count} required', locale, { count: required })}` : '');
}

function mcpHeaders(sessionId?: string, protocolVersion?: string): Record<string, string> {
  const headers: Record<string, string> = {
    Accept: 'application/json, text/event-stream',
    'Content-Type': 'application/json',
  };
  const credential = api.apiKey || api.adminApiKey;
  if (credential) headers.Authorization = `Bearer ${credential}`;
  if (protocolVersion) headers['MCP-Protocol-Version'] = protocolVersion;
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
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(key)) {
      throw new Error(`Invalid environment variable: ${key}`);
    }
    if (value !== `\${${key}}`) {
      throw new Error(`${key} must use the safe reference \${${key}}; raw secrets are not stored.`);
    }
    env[key] = value;
  }
  return env;
}

function environmentReferenceName(reference?: string): string {
  const match = /^\$\{([A-Za-z_][A-Za-z0-9_]*)\}$/.exec(reference || '');
  return match?.[1] || '';
}

function validateEnvironmentVariableName(value: string, label: string): string {
  const name = value.trim();
  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) {
    throw new Error(`${label} must be an environment variable name such as MCP_API_TOKEN.`);
  }
  return name;
}

function isLoopbackHostname(hostname: string): boolean {
  const normalized = hostname.toLowerCase().replace(/^\[|\]$/g, '');
  return normalized === 'localhost' || normalized === '127.0.0.1' || normalized === '::1';
}

function validateHttpEndpoint(value: string, allowInsecureHttp: boolean): string {
  const endpoint = value.trim();
  if (!endpoint) throw new Error('Endpoint URL is required.');

  let parsed: URL;
  try {
    parsed = new URL(endpoint);
  } catch {
    throw new Error('Endpoint URL must be a valid http:// or https:// URL.');
  }
  if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
    throw new Error('Endpoint URL must use HTTP or HTTPS.');
  }
  if (parsed.username || parsed.password) {
    throw new Error('Do not embed credentials in the endpoint URL.');
  }
  if (parsed.hash) throw new Error('Endpoint URL must not contain a fragment.');
  if (parsed.protocol === 'http:' && !isLoopbackHostname(parsed.hostname) && !allowInsecureHttp) {
    throw new Error('Plain HTTP is restricted to localhost. Use HTTPS or enable insecure HTTP in Advanced settings.');
  }
  return endpoint;
}

function draftFromServer(server: McpServerState): ServerDraft {
  const transport: ExternalMcpTransport = server.transport === 'streamable-http'
    ? 'streamable-http'
    : 'stdio';
  const tokenEnvironmentVariable = environmentReferenceName(server.bearer_token);
  return {
    id: server.id,
    name: server.name,
    transport,
    url: server.url || '',
    authentication: tokenEnvironmentVariable ? 'bearer-env' : 'none',
    tokenEnvironmentVariable,
    allowInsecureHttp: Boolean(server.allow_insecure_http),
    command: server.command || '',
    args: (server.args || []).join('\n'),
    workingDir: server.working_dir || '',
    timeoutMs: String(server.timeout_ms || 30000),
    env: Object.entries(server.env || {}).map(([key, value]) => `${key}=${value}`).join('\n'),
  };
}

function serverPayload(draft: ServerDraft): SaveableMcpServer {
  if (!draft.name.trim()) throw new Error('Name is required.');
  const timeout = Number(draft.timeoutMs);
  if (!Number.isInteger(timeout) || timeout < 1000 || timeout > 300000) {
    throw new Error('Timeout must be between 1000 and 300000 ms.');
  }

  const common = {
    ...(draft.id ? { id: draft.id } : {}),
    name: draft.name.trim(),
    transport: draft.transport,
    timeout_ms: timeout,
    enabled: true,
  } as const;

  if (draft.transport === 'streamable-http') {
    const url = validateHttpEndpoint(draft.url, draft.allowInsecureHttp);
    const bearerToken = draft.authentication === 'bearer-env'
      ? `\${${validateEnvironmentVariableName(draft.tokenEnvironmentVariable, 'Bearer token variable')}}`
      : '';
    return {
      ...common,
      transport: 'streamable-http',
      url,
      bearer_token: bearerToken,
      allow_insecure_http: draft.allowInsecureHttp,
    };
  }

  if (!draft.command.trim()) throw new Error('Command is required for a local process.');
  return {
    ...common,
    transport: 'stdio',
    command: draft.command.trim(),
    args: draft.args.split(/\r?\n/).map(value => value.trim()).filter(Boolean),
    env: parseEnv(draft.env),
    working_dir: draft.workingDir.trim(),
  };
}

function transportLabel(server: McpServerState): string {
  return server.transport === 'streamable-http' ? 'HTTP endpoint' : 'Local process';
}

const McpPanel: React.FC<McpPanelProps> = ({ connectionStatus, isActive }) => {
  const { locale, t } = useI18n();
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
  const [testNotice, setTestNotice] = useState('');
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
      if (initBody.error || !initBody.result?.protocolVersion) {
        throw new Error(initBody.error?.message || 'Invalid initialize response');
      }
      const negotiatedProtocolVersion = initBody.result.protocolVersion;
      const sessionId = init.headers.get('Mcp-Session-Id') || undefined;
      await fetch(mcpUrl, {
        method: 'POST',
        headers: mcpHeaders(sessionId, negotiatedProtocolVersion),
        body: JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }),
        signal,
      }).catch(() => undefined);
      const list = await fetch(mcpUrl, {
        method: 'POST',
        headers: mcpHeaders(sessionId, negotiatedProtocolVersion),
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
    if (result.status === 401) {
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
    setAdminAccess('unavailable');
    setHostError('');
    return () => abortRef.current?.abort();
  }, [connectionStatus, isActive, loadGatewayTools]);

  const gatewayLabel = gatewayStatus === 'connected' ? t('Connected')
    : gatewayStatus === 'checking' ? t('Checking…')
      : gatewayStatus === 'unavailable' ? t('Unavailable') : t('Not checked');
  const connectedExternal = useMemo(() => servers.filter(server => server.connected).length, [servers]);

  const resetForm = (nextDraft: ServerDraft = EMPTY_DRAFT) => {
    setDraft(nextDraft);
    setFormError('');
    setTestNotice('');
  };

  const applyAdminKey = async () => {
    api.setSessionAdminApiKey(adminKeyDraft);
    setAdminKeyNotice('');
    setAdminAccess('checking');
    const outcome = await probeAccess();
    if (outcome === 'ok') setAdminKeyNotice('Admin key applied for this app session.');
    else if (outcome === 'needs-admin') setHostError('Admin API key was rejected.');
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

  const testServer = async () => {
    setFormError('');
    setTestNotice('');
    setBusyId('__test__');
    try {
      const tested = await api.testMcpServer(serverPayload(draft));
      const toolCount = tested.tools?.length || 0;
      setTestNotice(`Connection successful · ${toolCount} tool${toolCount === 1 ? '' : 's'} · protocol ${tested.protocol_version || 'unknown'}`);
    } catch (error) {
      setFormError(friendlyErrorMessage(error));
    } finally {
      setBusyId('');
    }
  };

  const saveServer = async (event: React.FormEvent) => {
    event.preventDefault();
    setFormError('');
    setTestNotice('');
    setBusyId(draft.id || '__new__');
    try {
      const saved = await api.saveMcpServer(serverPayload(draft));
      await api.connectMcpServer(saved.id);
      await probeAccess();
      resetForm();
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

  const nonLocalPlainHttp = useMemo(() => {
    if (draft.transport !== 'streamable-http' || !draft.url.trim()) return false;
    try {
      const parsed = new URL(draft.url.trim());
      return parsed.protocol === 'http:' && !isLoopbackHostname(parsed.hostname);
    } catch {
      return false;
    }
  }, [draft.transport, draft.url]);

  return (
    <section className="connect__section connect__section--mcp" aria-label={t('MCP Gateway')} data-mcp-panel>
      <p className="connect__hint">
        {t('Lemonade works in both directions: its built-in tools are exposed as a Streamable HTTP MCP server, while Chat can use tools from connected HTTP endpoints or local MCP processes.')}
      </p>

      <div className="mcp-panel">
        <section className="mcp-panel__card" aria-labelledby="lemonade-mcp-title">
          <div className="mcp-panel__card-header">
            <div><h3 id="lemonade-mcp-title">{t('Lemon-Tools MCP server')}</h3><p>{t('Use Lemonade from Claude, VS Code, Cursor, MCP Inspector, or another MCP client.')}</p></div>
            <div className={`mcp-panel__status mcp-panel__status--${gatewayStatus}`} role="status" aria-live="polite" aria-atomic="true" data-mcp-status><span className="mcp-panel__status-dot" />{gatewayLabel}</div>
          </div>
          <div className="mcp-panel__url-copy-row">
            <input id="mcp-endpoint-display" className="mcp-panel__url-input" value={mcpUrl} readOnly aria-label={t('Lemon-Tools MCP endpoint URL')} />
            <button type="button" className="btn btn--ghost mcp-panel__copy-btn" aria-label={t('Copy MCP endpoint URL to clipboard')} onClick={() => void copyEndpoint()}>{t('Copy')}</button>
            <button type="button" className="btn btn--ghost" aria-label={t('Refresh MCP tools list')} onClick={() => void loadGatewayTools()} disabled={gatewayStatus === 'checking'}>{t('Refresh')}</button>
          </div>
          <div className="sr-only" role="status" aria-live="polite" aria-atomic="true" data-mcp-copy-live>{copyNotice}</div>
          {gatewayError && <div className="connect__error" role="alert" data-mcp-tools-error>{t('Could not load MCP tools: {error}', { error: gatewayError })}</div>}
          {gatewayStatus === 'connected' && gatewayTools.length > 0 ? (
            <details className="mcp-panel__tool-disclosure">
              <summary>{t('Tools ({count})', { count: gatewayTools.length })}</summary>
              <ul className="mcp-panel__tool-list" aria-label={t('Lemon-Tools MCP tools')} data-mcp-tools-list>
                {gatewayTools.map(tool => (
                  <li key={tool.name} className="mcp-panel__tool-row">
                    <div className="mcp-panel__tool-heading">
                      <code className="mcp-panel__tool-name">{tool.name}</code>
                      <span className="mcp-panel__tool-meta">{toolInputMetadata(tool.inputSchema, locale)}</span>
                    </div>
                    {tool.description && <p className="mcp-panel__tool-description">{tool.description}</p>}
                  </li>
                ))}
              </ul>
            </details>
          ) : gatewayStatus === 'connected' ? <p className="connect__empty">{t('No tools returned.')}</p> : null}
        </section>

        <section className="mcp-panel__card" aria-labelledby="external-mcp-title">
          <div className="mcp-panel__card-header">
            <div>
              <h3 id="external-mcp-title">{t('External MCP servers')}</h3>
              <p>{t('{connected} of {total} connected · HTTP endpoints and local processes · select up to four for Chat.', { connected: connectedExternal, total: servers.length })}</p>
            </div>
            {secure === true && adminAccess === 'ok' && (
              <button
                type="button"
                className="btn btn--primary"
                onClick={() => {
                  if (showForm) setShowForm(false);
                  else { resetForm(); setShowForm(true); }
                }}
                disabled={connectionStatus !== 'connected'}
              >
                {showForm ? t('Cancel') : t('Add server')}
              </button>
            )}
          </div>

          {secure === null || (secure === true && adminAccess === 'checking') ? (
            <p className="connect__empty">{t('Checking MCP administration access…')}</p>
          ) : secure === false ? (
            <div className="connect__notice mcp-panel__security-warning" role="note" data-mcp-security-warning>
              <p><strong>{t('External MCP servers are unavailable on this server.')}</strong></p>
              <p>
                {t('Due to security constraints, using external MCP servers requires your server to be set up with either a general API key (LEMONADE_API_KEY) or a dedicated admin API key (LEMONADE_ADMIN_API_KEY). Please set the respective environment variable in your Lemonade Server launch script, then restart the server to use this feature.')}
              </p>
            </div>
          ) : adminAccess === 'unavailable' ? (
            <div className="connect__notice mcp-panel__host-unavailable" role="alert" data-mcp-host-unavailable>
              <p>{hostError || t('External MCP connections are temporarily unavailable while the local GUI client is being introduced.')}</p>
            </div>
          ) : adminAccess === 'needs-admin' ? (
            <div className="mcp-panel__admin-auth" data-mcp-admin-auth>
              <div>
                <label htmlFor="mcp-admin-key">{t('Admin API key')}</label>
                <p>{t('Server requires admin API key to access external MCP feature setup.')}</p>
              </div>
              <div className="mcp-panel__admin-auth-controls">
                <input
                  id="mcp-admin-key"
                  type="password"
                  autoComplete="off"
                  value={adminKeyDraft}
                  onChange={event => setAdminKeyDraft(event.target.value)}
                  onKeyDown={event => { if (event.key === 'Enter') { event.preventDefault(); void applyAdminKey(); } }}
                  placeholder={t('Admin API key')}
                />
                <button type="button" className="btn btn--primary" onClick={() => void applyAdminKey()} disabled={connectionStatus !== 'connected' || hostLoading || !adminKeyDraft.trim()}>{t('Apply')}</button>
              </div>
              {adminKeyNotice && <div className="connect__notice" role="status">{adminKeyNotice}</div>}
              {hostError && <div className="connect__error" role="alert">{hostError}</div>}
            </div>
          ) : (
            <>
              {showForm && (
                <form className="mcp-server-form" onSubmit={saveServer}>
                  <fieldset className="mcp-server-form__transport mcp-server-form__wide">
                    <legend>{t('Connection type')}</legend>
                    <div className="mcp-transport-options">
                      <label className={`mcp-transport-option${draft.transport === 'streamable-http' ? ' is-selected' : ''}`}>
                        <input
                          type="radio"
                          name="mcp-transport"
                          value="streamable-http"
                          checked={draft.transport === 'streamable-http'}
                          onChange={() => setDraft(current => ({ ...current, transport: 'streamable-http' }))}
                        />
                        <span><strong>{t('HTTP endpoint')}</strong><small>{t('Recommended · connect to an MCP server already running in another app or service.')}</small></span>
                      </label>
                      <label className={`mcp-transport-option${draft.transport === 'stdio' ? ' is-selected' : ''}`}>
                        <input
                          type="radio"
                          name="mcp-transport"
                          value="stdio"
                          checked={draft.transport === 'stdio'}
                          onChange={() => setDraft(current => ({ ...current, transport: 'stdio' }))}
                        />
                        <span><strong>{t('Local process')}</strong><small>{t('Let Lemonade start and supervise a command on this machine.')}</small></span>
                      </label>
                    </div>
                  </fieldset>

                  <label><span>{t('Name')}</span><input value={draft.name} onChange={event => setDraft(current => ({ ...current, name: event.target.value }))} placeholder={draft.transport === 'streamable-http' ? 'MyMCP' : 'Filesystem'} /></label>
                  <label><span>{t('Timeout (ms)')}</span><input type="text" inputMode="numeric" pattern="[0-9]*" value={draft.timeoutMs} onChange={event => setDraft(current => ({ ...current, timeoutMs: event.target.value.replace(/\D/g, '') }))} /></label>

                  {draft.transport === 'streamable-http' ? (
                    <>
                      <label className="mcp-server-form__wide">
                        <span>{t('Endpoint URL')}</span>
                        <input
                          type="url"
                          value={draft.url}
                          onChange={event => setDraft(current => ({ ...current, url: event.target.value }))}
                          placeholder="http://127.0.0.1:3000/mcp"
                          autoComplete="url"
                        />
                        <small>{t('Use the single Streamable HTTP endpoint exposed by the external application.')}</small>
                      </label>
                      <label>
                        <span>{t('Authentication')}</span>
                        <select value={draft.authentication} onChange={event => setDraft(current => ({ ...current, authentication: event.target.value as HttpAuthentication }))}>
                          <option value="none">{t('None')}</option>
                          <option value="bearer-env">{t('Bearer token from environment')}</option>
                        </select>
                      </label>
                      {draft.authentication === 'bearer-env' && (
                        <label>
                          <span>{t('Token environment variable')}</span>
                          <input
                            value={draft.tokenEnvironmentVariable}
                            onChange={event => setDraft(current => ({ ...current, tokenEnvironmentVariable: event.target.value }))}
                            placeholder="MCP_API_TOKEN"
                            autoComplete="off"
                          />
                        </label>
                      )}
                      <details className="mcp-server-form__advanced mcp-server-form__wide">
                        <summary>{t('Advanced HTTP settings')}</summary>
                        <label className="mcp-server-form__checkbox">
                          <input
                            type="checkbox"
                            checked={draft.allowInsecureHttp}
                            onChange={event => setDraft(current => ({ ...current, allowInsecureHttp: event.target.checked }))}
                          />
                          <span>{t('Allow unencrypted HTTP to a non-local address')}</span>
                        </label>
                        <p>{t('Keep this off unless the endpoint is on a trusted private network. Localhost HTTP remains allowed.')}</p>
                      </details>
                      {nonLocalPlainHttp && !draft.allowInsecureHttp && (
                        <div className="connect__notice mcp-server-form__wide" role="note">{t('This endpoint needs HTTPS, or the explicit insecure HTTP option above.')}</div>
                      )}
                      <p className="mcp-server-form__note">{t('Bearer tokens are read from the desktop app environment when connecting. Raw credentials are never stored.')}</p>
                    </>
                  ) : (
                    <>
                      <label><span>{t('Command')}</span><input value={draft.command} onChange={event => setDraft(current => ({ ...current, command: event.target.value }))} placeholder="npx" /></label>
                      <label><span>{t('Working directory · optional')}</span><input value={draft.workingDir} onChange={event => setDraft(current => ({ ...current, workingDir: event.target.value }))} /></label>
                      <label className="mcp-server-form__wide"><span>{t('Arguments · one per line')}</span><textarea value={draft.args} onChange={event => setDraft(current => ({ ...current, args: event.target.value }))} placeholder={'-y\n@modelcontextprotocol/server-filesystem\n/home/user/projects'} rows={4} /></label>
                      <label className="mcp-server-form__wide"><span>{t('Environment references · one')} <code>{'KEY=${KEY}'}</code> {t('per line')}</span><textarea value={draft.env} onChange={event => setDraft(current => ({ ...current, env: event.target.value }))} placeholder="GITHUB_TOKEN=${GITHUB_TOKEN}" rows={3} /></label>
                      <p className="mcp-server-form__note">{t('The desktop app starts this command locally. Environment values must use references, and the referenced variables must exist in the desktop app environment.')}</p>
                    </>
                  )}

                  {formError && <div className="connect__error mcp-server-form__wide" role="alert">{formError}</div>}
                  {testNotice && <div className="connect__notice mcp-server-form__wide" role="status">{testNotice}</div>}
                  <div className="mcp-server-form__actions mcp-server-form__wide">
                    <button className="btn btn--ghost" type="button" onClick={() => void testServer()} disabled={Boolean(busyId)}>{busyId === '__test__' ? t('Testing…') : t('Test connection')}</button>
                    <button className="btn btn--primary" type="submit" disabled={Boolean(busyId)}>{busyId && busyId !== '__test__' ? t('Saving…') : t('Save and connect')}</button>
                  </div>
                </form>
              )}

              {hostError && <div className="connect__error" role="alert">{hostError}</div>}
              {hostLoading ? <p className="connect__empty">{t('Loading MCP servers…')}</p> : servers.length === 0 ? (
                <p className="connect__empty">{t('No external MCP server configured. Add an HTTP endpoint or a local process; built-in Lemonade tools remain available in Chat.')}</p>
              ) : (
                <div className="mcp-server-list">
                  {servers.map(server => (
                    <article className="mcp-server-card" key={server.id}>
                      <div className="mcp-server-card__main">
                        <span className={`mcp-panel__status-dot${server.connected ? ' is-connected' : ''}`} aria-hidden="true" />
                        <div>
                          <div className="mcp-server-card__heading"><strong>{server.name}</strong><span>{t(transportLabel(server))}</span></div>
                          <code>{server.transport === 'streamable-http' ? server.url : [server.command, ...(server.args || [])].filter(Boolean).join(' ')}</code>
                          <small>{server.connected ? `${server.tools?.length || 0} tools · protocol ${server.protocol_version || 'unknown'}` : server.last_error || server.status}</small>
                        </div>
                      </div>
                      {server.tools && server.tools.length > 0 && (
                        <details className="mcp-panel__tool-disclosure">
                          <summary>{t('Tools ({count})', { count: server.tools.length })}</summary>
                          <ul className="mcp-panel__tool-list" aria-label={`${server.name} tools`}>
                            {server.tools.map(tool => (
                              <li key={tool.name} className="mcp-panel__tool-row">
                                <div className="mcp-panel__tool-heading">
                                  <code className="mcp-panel__tool-name">{tool.name}</code>
                                  <span className="mcp-panel__tool-meta">{toolInputMetadata(tool.inputSchema, locale)}</span>
                                </div>
                                {tool.title && tool.title !== tool.name && <strong className="mcp-panel__tool-title">{tool.title}</strong>}
                                {tool.description && <p className="mcp-panel__tool-description">{tool.description}</p>}
                              </li>
                            ))}
                          </ul>
                        </details>
                      )}
                      <div className="mcp-server-card__actions">
                        <button type="button" className="btn btn--ghost" onClick={() => { resetForm(draftFromServer(server)); setShowForm(true); }}>{t('Edit')}</button>
                        {server.connected ? (
                          <><button type="button" className="btn btn--ghost" onClick={() => void runServerAction(server.id, 'refresh')} disabled={busyId === server.id}>{t('Refresh tools')}</button><button type="button" className="btn btn--ghost" onClick={() => void runServerAction(server.id, 'disconnect')} disabled={busyId === server.id}>{t('Disconnect')}</button></>
                        ) : <button type="button" className="btn btn--primary" onClick={() => void runServerAction(server.id, 'connect')} disabled={busyId === server.id}>{t('Connect')}</button>}
                        <button type="button" className="btn btn--danger" onClick={() => void runServerAction(server.id, 'remove')} disabled={busyId === server.id}>{t('Remove')}</button>
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
