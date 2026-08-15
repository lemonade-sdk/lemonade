import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useI18n } from '../i18n';
import { copyTextToClipboard } from '../clipboard';
import type { TranslationParams } from '../i18n/types';
import api, {
  ConnectionStatus,
  McpServerConfig,
  McpServerState,
  friendlyErrorMessage,
} from '../api';

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

type McpTranslate = (key: string, params?: TranslationParams) => string;

function toolInputMetadata(inputSchema: Record<string, unknown> | undefined, t: McpTranslate): string {
  if (!inputSchema) return t('toolMeta.schemaUnavailable');
  const properties = inputSchema.properties;
  if (!properties || typeof properties !== 'object' || Array.isArray(properties)) {
    return t('toolMeta.noInputs');
  }

  const inputCount = Object.keys(properties).length;
  if (inputCount === 0) return t('toolMeta.noInputs');

  const required = Array.isArray(inputSchema.required)
    ? inputSchema.required.filter(value => typeof value === 'string').length
    : 0;
  const inputs = t('toolMeta.inputs', { count: inputCount });
  return required ? `${inputs} · ${t('toolMeta.required', { count: required })}` : inputs;
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

function parseEnv(text: string, t: McpTranslate): Record<string, string> {
  const env: Record<string, string> = {};
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line) continue;
    const equals = line.indexOf('=');
    if (equals < 1) throw new Error(t('validation.invalidEnvLine', { line }));
    const key = line.slice(0, equals).trim();
    const value = line.slice(equals + 1).trim();
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(key)) {
      throw new Error(t('validation.invalidEnvVariable', { name: key }));
    }
    if (value !== `\${${key}}`) {
      throw new Error(t('validation.safeReference', { name: key }));
    }
    env[key] = value;
  }
  return env;
}

function environmentReferenceName(reference?: string): string {
  const match = /^\$\{([A-Za-z_][A-Za-z0-9_]*)\}$/.exec(reference || '');
  return match?.[1] || '';
}

function validateEnvironmentVariableName(value: string, label: string, t: McpTranslate): string {
  const name = value.trim();
  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) {
    throw new Error(t('validation.envName', { label }));
  }
  return name;
}

function isLoopbackHostname(hostname: string): boolean {
  const normalized = hostname.toLowerCase().replace(/^\[|\]$/g, '');
  return normalized === 'localhost' || normalized === '127.0.0.1' || normalized === '::1';
}

function validateHttpEndpoint(value: string, allowInsecureHttp: boolean, t: McpTranslate): string {
  const endpoint = value.trim();
  if (!endpoint) throw new Error(t('validation.endpointRequired'));

  let parsed: URL;
  try {
    parsed = new URL(endpoint);
  } catch {
    throw new Error(t('validation.endpointValid'));
  }
  if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
    throw new Error(t('validation.endpointProtocol'));
  }
  if (parsed.username || parsed.password) {
    throw new Error(t('validation.endpointCredentials'));
  }
  if (parsed.hash) throw new Error(t('validation.endpointFragment'));
  if (parsed.protocol === 'http:' && !isLoopbackHostname(parsed.hostname) && !allowInsecureHttp) {
    throw new Error(t('validation.insecureHttp'));
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

function serverPayload(draft: ServerDraft, t: McpTranslate): SaveableMcpServer {
  if (!draft.name.trim()) throw new Error(t('validation.nameRequired'));
  const timeout = Number(draft.timeoutMs);
  if (!Number.isInteger(timeout) || timeout < 1000 || timeout > 300000) {
    throw new Error(t('validation.timeout'));
  }

  const common = {
    ...(draft.id ? { id: draft.id } : {}),
    name: draft.name.trim(),
    transport: draft.transport,
    timeout_ms: timeout,
    enabled: true,
  } as const;

  if (draft.transport === 'streamable-http') {
    const url = validateHttpEndpoint(draft.url, draft.allowInsecureHttp, t);
    const bearerToken = draft.authentication === 'bearer-env'
      ? `\${${validateEnvironmentVariableName(draft.tokenEnvironmentVariable, t('validation.bearerVariable'), t)}}`
      : '';
    return {
      ...common,
      transport: 'streamable-http',
      url,
      bearer_token: bearerToken,
      allow_insecure_http: draft.allowInsecureHttp,
    };
  }

  if (!draft.command.trim()) throw new Error(t('validation.commandRequired'));
  return {
    ...common,
    transport: 'stdio',
    command: draft.command.trim(),
    args: draft.args.split(/\r?\n/).map(value => value.trim()).filter(Boolean),
    env: parseEnv(draft.env, t),
    working_dir: draft.workingDir.trim(),
  };
}

function transportLabel(server: McpServerState, t: McpTranslate): string {
  return server.transport === 'streamable-http' ? t('external.http') : t('external.local');
}

const McpPanel: React.FC<McpPanelProps> = ({ connectionStatus, isActive }) => {
  const { t } = useI18n('mcp');
  const mcpTranslateRef = useRef(t);
  mcpTranslateRef.current = t;
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
      ? mcpTranslateRef.current('external.unreachableHttp', { status: result.status })
      : mcpTranslateRef.current('external.unreachable'));

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

  const gatewayLabel = gatewayStatus === 'connected' ? t('gateway.status.connected')
    : gatewayStatus === 'checking' ? t('gateway.status.checking')
      : gatewayStatus === 'unavailable' ? t('gateway.status.unavailable') : t('gateway.status.idle');
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
    if (outcome === 'ok') setAdminKeyNotice(t('external.adminApplied'));
    else if (outcome === 'needs-admin') setHostError(t('external.adminRejected'));
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
      const tested = await api.testMcpServer(serverPayload(draft, t));
      const toolCount = tested.tools?.length || 0;
      setTestNotice(t('external.testSuccess', { count: toolCount, protocol: tested.protocol_version || t('external.unknown') }));
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
      const saved = await api.saveMcpServer(serverPayload(draft, t));
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
      await copyTextToClipboard(mcpUrl);
      setCopyNotice(t('gateway.copied'));
    } catch {
      setCopyNotice(t('gateway.copyManual'));
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
    <section className="connect__section connect__section--mcp" aria-label={t('gateway.aria')} data-mcp-panel>
      <p className="connect__hint">{t('gateway.intro')}</p>

      <div className="mcp-panel">
        <section className="mcp-panel__card" aria-labelledby="lemonade-mcp-title">
          <div className="mcp-panel__card-header">
            <div><h3 id="lemonade-mcp-title">{t('gateway.title')}</h3><p>{t('gateway.description')}</p></div>
            <div className={`mcp-panel__status mcp-panel__status--${gatewayStatus}`} role="status" aria-live="polite" aria-atomic="true" data-mcp-status><span className="mcp-panel__status-dot" />{gatewayLabel}</div>
          </div>
          <div className="mcp-panel__url-copy-row">
            <input id="mcp-endpoint-display" className="mcp-panel__url-input" value={mcpUrl} readOnly aria-label={t('gateway.endpointAria')} />
            <button type="button" className="btn btn--ghost mcp-panel__copy-btn" aria-label={t('gateway.copyAria')} onClick={() => void copyEndpoint()}>{t('gateway.copy')}</button>
            <button type="button" className="btn btn--ghost" aria-label={t('gateway.refreshAria')} onClick={() => void loadGatewayTools()} disabled={gatewayStatus === 'checking'}>{t('gateway.refresh')}</button>
          </div>
          <div className="sr-only" role="status" aria-live="polite" aria-atomic="true" data-mcp-copy-live>{copyNotice}</div>
          {gatewayError && <div className="connect__error" role="alert" data-mcp-tools-error>{t('gateway.loadError', { error: gatewayError })}</div>}
          {gatewayStatus === 'connected' && gatewayTools.length > 0 ? (
            <details className="mcp-panel__tool-disclosure">
              <summary>{t('gateway.tools', { count: gatewayTools.length })}</summary>
              <ul className="mcp-panel__tool-list" aria-label={t('gateway.toolsAria')} data-mcp-tools-list>
                {gatewayTools.map(tool => (
                  <li key={tool.name} className="mcp-panel__tool-row">
                    <div className="mcp-panel__tool-heading">
                      <code className="mcp-panel__tool-name">{tool.name}</code>
                      <span className="mcp-panel__tool-meta">{toolInputMetadata(tool.inputSchema, t)}</span>
                    </div>
                    {tool.description && <p className="mcp-panel__tool-description">{tool.description}</p>}
                  </li>
                ))}
              </ul>
            </details>
          ) : gatewayStatus === 'connected' ? <p className="connect__empty">{t('gateway.empty')}</p> : null}
        </section>

        <section className="mcp-panel__card" aria-labelledby="external-mcp-title">
          <div className="mcp-panel__card-header">
            <div>
              <h3 id="external-mcp-title">{t('external.title')}</h3>
              <p>{t('external.summary', { connected: connectedExternal, total: servers.length })}</p>
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
                {showForm ? t('external.cancel') : t('external.add')}
              </button>
            )}
          </div>

          {secure === null || (secure === true && adminAccess === 'checking') ? (
            <p className="connect__empty">{t('external.checking')}</p>
          ) : secure === false ? (
            <div className="connect__notice mcp-panel__security-warning" role="note" data-mcp-security-warning>
              <p><strong>{t('external.securityTitle')}</strong></p>
              <p>
                {t('external.securityPrefix')} (<code>LEMONADE_API_KEY</code>) {t('external.securityOr')} (<code>LEMONADE_ADMIN_API_KEY</code>).
                {' '}{t('external.securitySuffix')}
              </p>
            </div>
          ) : adminAccess === 'unavailable' ? (
            <div className="connect__notice mcp-panel__host-unavailable" role="alert" data-mcp-host-unavailable>
              <p>{hostError || t('external.adminUnavailable')}</p>
              <button type="button" className="btn btn--ghost" onClick={() => void probeAccess()} disabled={connectionStatus !== 'connected' || hostLoading}>
                {hostLoading ? t('external.retrying') : t('external.retry')}
              </button>
            </div>
          ) : adminAccess === 'needs-admin' ? (
            <div className="mcp-panel__admin-auth" data-mcp-admin-auth>
              <div>
                <label htmlFor="mcp-admin-key">{t('external.adminKey')}</label>
                <p>{t('external.adminHint')}</p>
              </div>
              <div className="mcp-panel__admin-auth-controls">
                <input
                  id="mcp-admin-key"
                  type="password"
                  autoComplete="off"
                  value={adminKeyDraft}
                  onChange={event => setAdminKeyDraft(event.target.value)}
                  onKeyDown={event => { if (event.key === 'Enter') { event.preventDefault(); void applyAdminKey(); } }}
                  placeholder={t('external.adminPlaceholder')}
                />
                <button type="button" className="btn btn--primary" onClick={() => void applyAdminKey()} disabled={connectionStatus !== 'connected' || hostLoading || !adminKeyDraft.trim()}>{t('external.apply')}</button>
              </div>
              {adminKeyNotice && <div className="connect__notice" role="status">{adminKeyNotice}</div>}
              {hostError && <div className="connect__error" role="alert">{hostError}</div>}
            </div>
          ) : (
            <>
              {showForm && (
                <form className="mcp-server-form" onSubmit={saveServer}>
                  <fieldset className="mcp-server-form__transport mcp-server-form__wide">
                    <legend>{t('external.connectionType')}</legend>
                    <div className="mcp-transport-options">
                      <label className={`mcp-transport-option${draft.transport === 'streamable-http' ? ' is-selected' : ''}`}>
                        <input
                          type="radio"
                          name="mcp-transport"
                          value="streamable-http"
                          checked={draft.transport === 'streamable-http'}
                          onChange={() => setDraft(current => ({ ...current, transport: 'streamable-http' }))}
                        />
                        <span><strong>{t('external.http')}</strong><small>{t('external.httpRecommendation')}</small></span>
                      </label>
                      <label className={`mcp-transport-option${draft.transport === 'stdio' ? ' is-selected' : ''}`}>
                        <input
                          type="radio"
                          name="mcp-transport"
                          value="stdio"
                          checked={draft.transport === 'stdio'}
                          onChange={() => setDraft(current => ({ ...current, transport: 'stdio' }))}
                        />
                        <span><strong>{t('external.local')}</strong><small>{t('external.localHint')}</small></span>
                      </label>
                    </div>
                  </fieldset>

                  <label><span>{t('external.name')}</span><input value={draft.name} onChange={event => setDraft(current => ({ ...current, name: event.target.value }))} placeholder={draft.transport === 'streamable-http' ? t('external.namePlaceholderHttp') : t('external.namePlaceholderLocal')} /></label>
                  <label><span>{t('external.timeout')}</span><input type="text" inputMode="numeric" pattern="[0-9]*" value={draft.timeoutMs} onChange={event => setDraft(current => ({ ...current, timeoutMs: event.target.value.replace(/\D/g, '') }))} /></label>

                  {draft.transport === 'streamable-http' ? (
                    <>
                      <label className="mcp-server-form__wide">
                        <span>{t('external.endpointUrl')}</span>
                        <input
                          type="url"
                          value={draft.url}
                          onChange={event => setDraft(current => ({ ...current, url: event.target.value }))}
                          placeholder="http://127.0.0.1:3000/mcp"
                          autoComplete="url"
                        />
                        <small>{t('external.endpointHint')}</small>
                      </label>
                      <label>
                        <span>{t('external.authentication')}</span>
                        <select value={draft.authentication} onChange={event => setDraft(current => ({ ...current, authentication: event.target.value as HttpAuthentication }))}>
                          <option value="none">{t('external.none')}</option>
                          <option value="bearer-env">{t('external.bearerEnv')}</option>
                        </select>
                      </label>
                      {draft.authentication === 'bearer-env' && (
                        <label>
                          <span>{t('external.tokenEnv')}</span>
                          <input
                            value={draft.tokenEnvironmentVariable}
                            onChange={event => setDraft(current => ({ ...current, tokenEnvironmentVariable: event.target.value }))}
                            placeholder="MCP_API_TOKEN"
                            autoComplete="off"
                          />
                        </label>
                      )}
                      <details className="mcp-server-form__advanced mcp-server-form__wide">
                        <summary>{t('external.advancedHttp')}</summary>
                        <label className="mcp-server-form__checkbox">
                          <input
                            type="checkbox"
                            checked={draft.allowInsecureHttp}
                            onChange={event => setDraft(current => ({ ...current, allowInsecureHttp: event.target.checked }))}
                          />
                          <span>{t('external.allowHttp')}</span>
                        </label>
                        <p>{t('external.allowHttpHint')}</p>
                      </details>
                      {nonLocalPlainHttp && !draft.allowInsecureHttp && (
                        <div className="connect__notice mcp-server-form__wide" role="note">{t('external.httpsNeeded')}</div>
                      )}
                      <p className="mcp-server-form__note">{t('external.bearerNotePrefix')} <code>mcp_servers.json</code>.</p>
                    </>
                  ) : (
                    <>
                      <label><span>{t('external.command')}</span><input value={draft.command} onChange={event => setDraft(current => ({ ...current, command: event.target.value }))} placeholder="npx" /></label>
                      <label><span>{t('external.workingDir')}</span><input value={draft.workingDir} onChange={event => setDraft(current => ({ ...current, workingDir: event.target.value }))} /></label>
                      <label className="mcp-server-form__wide"><span>{t('external.arguments')}</span><textarea value={draft.args} onChange={event => setDraft(current => ({ ...current, args: event.target.value }))} placeholder={'-y\n@modelcontextprotocol/server-filesystem\n/home/user/projects'} rows={4} /></label>
                      <label className="mcp-server-form__wide"><span>{t('external.envRefsPrefix')} <code>{'KEY=${KEY}'}</code> {t('external.envRefsSuffix')}</span><textarea value={draft.env} onChange={event => setDraft(current => ({ ...current, env: event.target.value }))} placeholder="GITHUB_TOKEN=${GITHUB_TOKEN}" rows={3} /></label>
                      <p className="mcp-server-form__note">{t('external.localNote')}</p>
                    </>
                  )}

                  {formError && <div className="connect__error mcp-server-form__wide" role="alert">{formError}</div>}
                  {testNotice && <div className="connect__notice mcp-server-form__wide" role="status">{testNotice}</div>}
                  <div className="mcp-server-form__actions mcp-server-form__wide">
                    <button className="btn btn--ghost" type="button" onClick={() => void testServer()} disabled={Boolean(busyId)}>{busyId === '__test__' ? t('external.testing') : t('external.testConnection')}</button>
                    <button className="btn btn--primary" type="submit" disabled={Boolean(busyId)}>{busyId && busyId !== '__test__' ? t('external.saving') : t('external.saveConnect')}</button>
                  </div>
                </form>
              )}

              {hostError && <div className="connect__error" role="alert">{hostError}</div>}
              {hostLoading ? <p className="connect__empty">{t('external.loading')}</p> : servers.length === 0 ? (
                <p className="connect__empty">{t('external.empty')}</p>
              ) : (
                <div className="mcp-server-list">
                  {servers.map(server => (
                    <article className="mcp-server-card" key={server.id}>
                      <div className="mcp-server-card__main">
                        <span className={`mcp-panel__status-dot${server.connected ? ' is-connected' : ''}`} aria-hidden="true" />
                        <div>
                          <div className="mcp-server-card__heading"><strong>{server.name}</strong><span>{transportLabel(server, t)}</span></div>
                          <code>{server.transport === 'streamable-http' ? server.url : [server.command, ...(server.args || [])].filter(Boolean).join(' ')}</code>
                          <small>{server.connected ? t('external.connectedMeta', { count: server.tools?.length || 0, protocol: server.protocol_version || t('external.unknown') }) : server.last_error || server.status}</small>
                        </div>
                      </div>
                      {server.tools && server.tools.length > 0 && (
                        <details className="mcp-panel__tool-disclosure">
                          <summary>{t('external.tools', { count: server.tools.length })}</summary>
                          <ul className="mcp-panel__tool-list" aria-label={t('external.toolsAria', { name: server.name })}>
                            {server.tools.map(tool => (
                              <li key={tool.name} className="mcp-panel__tool-row">
                                <div className="mcp-panel__tool-heading">
                                  <code className="mcp-panel__tool-name">{tool.name}</code>
                                  <span className="mcp-panel__tool-meta">{toolInputMetadata(tool.inputSchema, t)}</span>
                                </div>
                                {tool.title && tool.title !== tool.name && <strong className="mcp-panel__tool-title">{tool.title}</strong>}
                                {tool.description && <p className="mcp-panel__tool-description">{tool.description}</p>}
                              </li>
                            ))}
                          </ul>
                        </details>
                      )}
                      <div className="mcp-server-card__actions">
                        <button type="button" className="btn btn--ghost" onClick={() => { resetForm(draftFromServer(server)); setShowForm(true); }}>{t('external.edit')}</button>
                        {server.connected ? (
                          <><button type="button" className="btn btn--ghost" onClick={() => void runServerAction(server.id, 'refresh')} disabled={busyId === server.id}>{t('external.refreshTools')}</button><button type="button" className="btn btn--ghost" onClick={() => void runServerAction(server.id, 'disconnect')} disabled={busyId === server.id}>{t('external.disconnect')}</button></>
                        ) : <button type="button" className="btn btn--primary" onClick={() => void runServerAction(server.id, 'connect')} disabled={busyId === server.id}>{t('external.connect')}</button>}
                        <button type="button" className="btn btn--danger" onClick={() => void runServerAction(server.id, 'remove')} disabled={busyId === server.id}>{t('external.remove')}</button>
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
