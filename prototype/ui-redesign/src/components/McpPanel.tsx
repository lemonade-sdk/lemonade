import React, { useCallback, useEffect, useRef, useState } from 'react';
import api, { ConnectionStatus } from '../api';

const MCP_PROTOCOL_VERSION = '2025-06-18';
// Matches package.json "version"; declared as a constant to avoid a
// dynamic require() in the browser bundle.
const CLIENT_VERSION = '0.1.0';

interface McpTool {
  name: string;
  description?: string;
  inputSchema?: Record<string, unknown>;
}

type McpStatus = 'idle' | 'checking' | 'connected' | 'unavailable';

export interface McpPanelProps {
  connectionStatus: ConnectionStatus;
}

function buildMcpHeaders(
  opts: { sessionId?: string; includeProtocolVersion?: boolean } = {},
): Record<string, string> {
  const h: Record<string, string> = { 'Content-Type': 'application/json' };
  const key = api.apiKey;
  if (key) h['Authorization'] = `Bearer ${key}`;
  if (opts.includeProtocolVersion) h['MCP-Protocol-Version'] = MCP_PROTOCOL_VERSION;
  if (opts.sessionId) h['Mcp-Session-Id'] = opts.sessionId;
  return h;
}

/** Combine an AbortController signal with a per-request timeout signal. */
function makeSignal(staleSignal: AbortSignal, timeoutMs: number): AbortSignal {
  const timeout = AbortSignal.timeout(timeoutMs);
  if (typeof (AbortSignal as unknown as Record<string, unknown>).any === 'function') {
    return (AbortSignal as unknown as { any: (s: AbortSignal[]) => AbortSignal }).any([
      staleSignal,
      timeout,
    ]);
  }
  return staleSignal;
}

const McpPanel: React.FC<McpPanelProps> = ({ connectionStatus }) => {
  const [mcpStatus, setMcpStatus] = useState<McpStatus>('idle');
  const [tools, setTools] = useState<McpTool[]>([]);
  const [toolsError, setToolsError] = useState<string | null>(null);
  const [toolsLoading, setToolsLoading] = useState(false);
  const [copyNotice, setCopyNotice] = useState('');
  const copyTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  // AbortController for stale-async guard: aborted on disconnect, new connect, or unmount.
  const abortRef = useRef<AbortController | null>(null);

  const mcpUrl = `${api.baseUrl}/mcp`;

  const fetchTools = useCallback(async () => {
    // Cancel any in-flight sequence from a prior call.
    if (abortRef.current) abortRef.current.abort();
    const controller = new AbortController();
    abortRef.current = controller;
    const { signal } = controller;

    setToolsLoading(true);
    setToolsError(null);
    setMcpStatus('checking');
    try {
      // ── Step 1: initialize handshake (spec 2025-06-18) ───────────────────
      const initRes = await fetch(mcpUrl, {
        method: 'POST',
        headers: buildMcpHeaders(),
        body: JSON.stringify({
          jsonrpc: '2.0',
          id: 1,
          method: 'initialize',
          params: {
            protocolVersion: MCP_PROTOCOL_VERSION,
            capabilities: { tools: {} },
            clientInfo: { name: 'lemonade-gui3', version: CLIENT_VERSION },
          },
        }),
        signal: makeSignal(signal, 8000),
      });
      if (!initRes.ok) throw new Error(`initialize HTTP ${initRes.status}`);

      const initData = (await initRes.json()) as {
        result?: { protocolVersion?: string };
        error?: { message?: string };
      };
      if (initData.error) {
        throw new Error(initData.error.message || 'initialize JSON-RPC error');
      }
      if (!initData.result?.protocolVersion) {
        throw new Error('initialize response missing protocolVersion');
      }

      // Capture optional session ID; include in all subsequent requests.
      const sessionId = initRes.headers.get('Mcp-Session-Id') ?? undefined;

      if (signal.aborted) return;

      const postInitHeaders = buildMcpHeaders({ sessionId, includeProtocolVersion: true });

      // ── Step 2: notifications/initialized (no id — it is a notification) ─
      try {
        await fetch(mcpUrl, {
          method: 'POST',
          headers: postInitHeaders,
          body: JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }),
          signal,
        });
      } catch {
        // Notifications are fire-and-forget; network/parse errors are acceptable.
      }

      if (signal.aborted) return;

      // ── Step 3: tools/list ────────────────────────────────────────────────
      const res = await fetch(mcpUrl, {
        method: 'POST',
        headers: postInitHeaders,
        body: JSON.stringify({ jsonrpc: '2.0', id: 2, method: 'tools/list' }),
        signal: makeSignal(signal, 8000),
      });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const data = (await res.json()) as {
        result?: { tools?: McpTool[] };
        error?: { message?: string };
      };
      if (data.error) throw new Error(data.error.message || 'JSON-RPC error');

      if (signal.aborted) return;

      setTools(Array.isArray(data.result?.tools) ? data.result!.tools : []);
      setMcpStatus('connected');
    } catch (err) {
      // Discard stale results from aborted requests.
      if ((err as { name?: string }).name === 'AbortError') return;
      setToolsError(err instanceof Error ? err.message : String(err));
      setMcpStatus('unavailable');
    } finally {
      if (!signal.aborted) setToolsLoading(false);
    }
  }, [mcpUrl]);

  useEffect(() => {
    if (connectionStatus === 'connected') {
      void fetchTools();
    } else {
      // Abort in-flight sequence and reset state.
      if (abortRef.current) abortRef.current.abort();
      setMcpStatus('idle');
      setTools([]);
      setToolsError(null);
      setToolsLoading(false);
    }
    return () => {
      // Abort on unmount to prevent stale state updates.
      if (abortRef.current) abortRef.current.abort();
    };
  }, [connectionStatus, fetchTools]);

  const handleCopy = () => {
    if (!navigator.clipboard?.writeText) {
      setCopyNotice('Copy not supported \u2014 select and copy manually');
      if (copyTimerRef.current) clearTimeout(copyTimerRef.current);
      copyTimerRef.current = setTimeout(() => setCopyNotice(''), 2500);
      return;
    }
    navigator.clipboard
      .writeText(mcpUrl)
      .then(() => {
        setCopyNotice('Copied');
      })
      .catch(() => {
        setCopyNotice('Copy not supported \u2014 select and copy manually');
      })
      .finally(() => {
        if (copyTimerRef.current) clearTimeout(copyTimerRef.current);
        copyTimerRef.current = setTimeout(() => setCopyNotice(''), 2500);
      });
  };

  const statusLabel =
    mcpStatus === 'connected' ? 'Connected' :
    mcpStatus === 'checking' ? 'Checking\u2026' :
    mcpStatus === 'unavailable' ? 'Unavailable' :
    'Not checked';

  return (
    <section
      className="connect__section connect__section--mcp"
      aria-labelledby="mcp-section-title"
      data-mcp-panel
    >
      <h2 id="mcp-section-title">MCP Gateway</h2>
      <p className="connect__hint">
        Lemonade exposes a{' '}
        <a href="https://modelcontextprotocol.io/" target="_blank" rel="noopener noreferrer">
          Model Context Protocol
        </a>{' '}
        (Streamable HTTP, POST-only, protocol&nbsp;2025-06-18) endpoint. Connect any
        MCP-compatible client or agent framework to call Lemonade&rsquo;s tools
        directly.
      </p>

      <div className="mcp-panel">
        {/* ── Endpoint URL + copy ── */}
        <div className="mcp-panel__url-row">
          <div className="mcp-panel__url-field">
            <label className="mcp-panel__url-label" htmlFor="mcp-endpoint-display">
              Endpoint URL
            </label>
            <div className="mcp-panel__url-copy-row">
              <input
                id="mcp-endpoint-display"
                className="mcp-panel__url-input"
                type="text"
                value={mcpUrl}
                readOnly
                aria-label="MCP endpoint URL"
              />
              <button
                className="btn btn--ghost mcp-panel__copy-btn"
                type="button"
                onClick={handleCopy}
                aria-label="Copy MCP endpoint URL to clipboard"
              >
                Copy
              </button>
            </div>
            {/* Always-present live region — empty until copy fires (NVDA pattern) */}
            <div
              className="sr-only"
              role="status"
              aria-live="polite"
              aria-atomic="true"
              data-mcp-copy-live
            >
              {copyNotice}
            </div>
          </div>

          {/* ── Health/status indicator ── */}
          <div className="mcp-panel__status-cell">
            <span className="mcp-panel__status-label" id="mcp-status-label">
              Status
            </span>
            <div
              className={`mcp-panel__status mcp-panel__status--${mcpStatus}`}
              role="status"
              aria-live="polite"
              aria-atomic="true"
              aria-labelledby="mcp-status-label"
              data-mcp-status
            >
              <span className="mcp-panel__status-dot" aria-hidden="true" />
              <span>{statusLabel}</span>
            </div>
          </div>
        </div>

        {/* ── Tools list ── */}
        <div className="mcp-panel__tools">
          <div className="mcp-panel__tools-header">
            <h3>Available tools</h3>
            <button
              className="btn btn--ghost"
              type="button"
              onClick={() => { void fetchTools(); }}
              disabled={toolsLoading || connectionStatus !== 'connected'}
              aria-label="Refresh MCP tools list"
            >
              {toolsLoading ? 'Loading\u2026' : 'Refresh'}
            </button>
          </div>

          {connectionStatus !== 'connected' ? (
            <p className="connect__empty">Connect to a server to view MCP tools.</p>
          ) : toolsLoading ? (
            <p className="connect__empty" aria-live="polite" aria-busy="true">
              Loading tools\u2026
            </p>
          ) : toolsError ? (
            <div className="connect__error" role="alert" data-mcp-tools-error>
              Could not load MCP tools: {toolsError}
            </div>
          ) : tools.length === 0 ? (
            <p className="connect__empty">No tools returned by the MCP gateway.</p>
          ) : (
            <ul
              className="mcp-panel__tool-list"
              aria-label="MCP tools"
              data-mcp-tools-list
            >
              {tools.map(tool => (
                <li key={tool.name} className="mcp-panel__tool-item">
                  <span className="mcp-panel__tool-name">{tool.name}</span>
                  {tool.description && (
                    <span className="mcp-panel__tool-desc">{tool.description}</span>
                  )}
                </li>
              ))}
            </ul>
          )}
        </div>
      </div>
    </section>
  );
};

export default McpPanel;
