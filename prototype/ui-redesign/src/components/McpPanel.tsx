import React, { useCallback, useEffect, useRef, useState } from 'react';
import api, { ConnectionStatus } from '../api';

interface McpTool {
  name: string;
  description?: string;
  inputSchema?: Record<string, unknown>;
}

type McpStatus = 'idle' | 'checking' | 'connected' | 'unavailable';

export interface McpPanelProps {
  connectionStatus: ConnectionStatus;
}

function buildMcpHeaders(): Record<string, string> {
  const h: Record<string, string> = { 'Content-Type': 'application/json' };
  const key = api.apiKey;
  if (key) h['Authorization'] = `Bearer ${key}`;
  return h;
}

const McpPanel: React.FC<McpPanelProps> = ({ connectionStatus }) => {
  const [mcpStatus, setMcpStatus] = useState<McpStatus>('idle');
  const [tools, setTools] = useState<McpTool[]>([]);
  const [toolsError, setToolsError] = useState<string | null>(null);
  const [toolsLoading, setToolsLoading] = useState(false);
  const [copyNotice, setCopyNotice] = useState('');
  const copyTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  const mcpUrl = `${api.baseUrl}/mcp`;

  const fetchTools = useCallback(async () => {
    setToolsLoading(true);
    setToolsError(null);
    setMcpStatus('checking');
    try {
      const headers = buildMcpHeaders();

      // MCP Streamable HTTP: initialize handshake first (protocol v2025-06-18)
      await fetch(mcpUrl, {
        method: 'POST',
        headers,
        body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'initialize', params: {} }),
        signal: AbortSignal.timeout(8000),
      });

      // Then request the tools list
      const res = await fetch(mcpUrl, {
        method: 'POST',
        headers,
        body: JSON.stringify({ jsonrpc: '2.0', id: 2, method: 'tools/list' }),
        signal: AbortSignal.timeout(8000),
      });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const data = (await res.json()) as {
        result?: { tools?: McpTool[] };
        error?: { message?: string };
      };
      if (data.error) throw new Error(data.error.message || 'JSON-RPC error');
      setTools(Array.isArray(data.result?.tools) ? data.result!.tools : []);
      setMcpStatus('connected');
    } catch (err) {
      setToolsError(err instanceof Error ? err.message : String(err));
      setMcpStatus('unavailable');
    } finally {
      setToolsLoading(false);
    }
  }, [mcpUrl]);

  useEffect(() => {
    if (connectionStatus === 'connected') {
      void fetchTools();
    } else {
      setMcpStatus('idle');
      setTools([]);
      setToolsError(null);
    }
  }, [connectionStatus, fetchTools]);

  const handleCopy = () => {
    navigator.clipboard
      .writeText(mcpUrl)
      .then(() => {
        setCopyNotice('Copied');
      })
      .catch(() => {
        setCopyNotice('Copy failed');
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
