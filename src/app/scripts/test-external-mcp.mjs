#!/usr/bin/env node
import http from 'node:http';
import { fileURLToPath } from 'node:url';

const baseUrl = (process.env.LEMONADE_BASE_URL || 'http://127.0.0.1:13305').replace(/\/+$/, '');
const apiKey = process.env.LEMONADE_API_KEY || '';
const adminApiKey = process.env.LEMONADE_ADMIN_API_KEY || apiKey;
const stdioServerId = `gui3-stdio-echo-${process.pid}`;
const httpServerId = `gui3-http-echo-${process.pid}`;
const mockPath = fileURLToPath(new URL('./mock-mcp-server.mjs', import.meta.url));
const protocolVersion = '2025-11-25';
const sessionId = `gui3-http-session-${process.pid}`;

async function request(path, init = {}) {
  const headers = {
    'Content-Type': 'application/json',
    ...(adminApiKey ? { Authorization: `Bearer ${adminApiKey}` } : {}),
    ...(init.headers || {}),
  };
  const response = await fetch(`${baseUrl}${path}`, {
    ...init,
    headers,
    body: init.body === undefined ? undefined : JSON.stringify(init.body),
  });
  const text = await response.text();
  let data = {};
  try { data = text ? JSON.parse(text) : {}; } catch { data = { raw: text }; }
  if (!response.ok) {
    const hint = path.startsWith('/internal/mcp') && (response.status === 401 || response.status === 403)
      ? ' Configure LEMONADE_ADMIN_API_KEY (or LEMONADE_API_KEY) in the lemond process and pass the matching key to this test.'
      : '';
    throw new Error(`${init.method || 'GET'} ${path} -> ${response.status}: ${JSON.stringify(data)}${hint}`);
  }
  return data;
}

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function textResult(result) {
  return result?.content?.find(block => block.type === 'text')?.text;
}

async function readJsonBody(req) {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  return JSON.parse(Buffer.concat(chunks).toString('utf8') || '{}');
}

async function startHttpMcpServer() {
  const observedMethods = [];
  const server = http.createServer(async (req, res) => {
    try {
      if (req.url !== '/mcp') {
        res.writeHead(404).end();
        return;
      }
      if (req.method === 'DELETE') {
        assert(req.headers['mcp-session-id'] === sessionId, 'DELETE did not carry the MCP session id.');
        res.writeHead(204).end();
        return;
      }
      if (req.method !== 'POST') {
        res.writeHead(405, { Allow: 'POST, DELETE' }).end();
        return;
      }

      const message = await readJsonBody(req);
      observedMethods.push(message.method || '');
      const isInitialize = message.method === 'initialize';
      if (!isInitialize) {
        assert(req.headers['mcp-session-id'] === sessionId, `${message.method} did not carry the MCP session id.`);
        assert(req.headers['mcp-protocol-version'] === protocolVersion, `${message.method} did not carry MCP-Protocol-Version.`);
      }

      if (isInitialize) {
        res.writeHead(200, {
          'Content-Type': 'application/json',
          'Mcp-Session-Id': sessionId,
        });
        res.end(JSON.stringify({
          jsonrpc: '2.0',
          id: message.id,
          result: {
            protocolVersion,
            capabilities: { tools: { listChanged: false } },
            serverInfo: { name: 'GUI3 HTTP Echo Test', version: '1.0.0' },
          },
        }));
        return;
      }

      if (message.method === 'notifications/initialized') {
        res.writeHead(202).end();
        return;
      }

      if (message.method === 'tools/list') {
        // Deliberately keep the SSE response open after the matching response.
        // A correct Streamable HTTP client must return as soon as that event is
        // received instead of waiting for the server to close the stream.
        res.writeHead(200, {
          'Content-Type': 'text/event-stream',
          'Cache-Control': 'no-cache',
          'Mcp-Session-Id': sessionId,
        });
        res.write(`event: message\ndata: ${JSON.stringify({
          jsonrpc: '2.0',
          id: message.id,
          result: {
            tools: [{
              name: 'echo',
              title: 'HTTP Echo',
              description: 'Echo text through Streamable HTTP.',
              inputSchema: {
                type: 'object',
                properties: { text: { type: 'string' } },
                required: ['text'],
              },
            }],
          },
        })}\n\n`);
        const timer = setTimeout(() => res.end(), 15000);
        timer.unref();
        req.on('close', () => clearTimeout(timer));
        return;
      }

      if (message.method === 'tools/call') {
        const text = String(message.params?.arguments?.text || '');
        res.writeHead(200, {
          'Content-Type': 'application/json',
          'Mcp-Session-Id': sessionId,
        });
        res.end(JSON.stringify({
          jsonrpc: '2.0',
          id: message.id,
          result: { content: [{ type: 'text', text: `http-echo:${text}` }] },
        }));
        return;
      }

      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({
        jsonrpc: '2.0',
        id: message.id ?? null,
        error: { code: -32601, message: `Unsupported test method: ${message.method}` },
      }));
    } catch (error) {
      res.writeHead(500, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: String(error?.message || error) }));
    }
  });

  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const address = server.address();
  if (!address || typeof address === 'string') throw new Error('Could not resolve mock MCP HTTP port.');

  return {
    url: `http://127.0.0.1:${address.port}/mcp`,
    observedMethods,
    async close() {
      server.closeAllConnections?.();
      await new Promise(resolve => server.close(resolve));
    },
  };
}

async function verifyDiscoveredTool(serverId, expectedText) {
  const tools = await request('/internal/mcp/tools');
  const echo = tools.tools?.find(tool => tool.server_id === serverId && tool.name === 'echo');
  assert(echo, `Echo tool was not discovered for ${serverId}.`);
  assert(echo.chat_name && echo.chat_name !== 'echo', 'External tool was not assigned a namespaced chat_name.');
  const called = await request(`/internal/mcp/servers/${serverId}/tools/call`, {
    method: 'POST',
    body: { name: 'echo', arguments: { text: 'hello-mcp' }, timeout_ms: 10000 },
  });
  const text = textResult(called.result);
  assert(text === expectedText, `Unexpected tool result for ${serverId}: ${JSON.stringify(called)}`);
  return { chatName: echo.chat_name, text };
}

const createdIds = [];
let httpMock;
try {
  console.log(`Testing external MCP client host at ${baseUrl}`);
  httpMock = await startHttpMcpServer();

  const draft = {
    name: 'GUI3 HTTP Draft Test',
    transport: 'streamable-http',
    url: httpMock.url,
    timeout_ms: 10000,
    enabled: true,
  };
  const tested = await request('/internal/mcp/servers/test', { method: 'POST', body: { server: draft } });
  assert(tested.server?.connected === true, 'HTTP draft test did not reach connected state.');
  assert(tested.server?.tools?.some(tool => tool.name === 'echo'), 'HTTP draft test did not discover echo.');

  await request('/internal/mcp/servers', {
    method: 'POST',
    body: { server: { ...draft, id: httpServerId, name: 'GUI3 HTTP Echo Test' } },
  });
  createdIds.push(httpServerId);
  const httpConnected = await request(`/internal/mcp/servers/${httpServerId}/connect`, { method: 'POST' });
  assert(httpConnected.server?.connected === true, 'HTTP server did not reach connected state.');
  const httpResult = await verifyDiscoveredTool(httpServerId, 'http-echo:hello-mcp');
  console.log(`PASS HTTP: discovered ${httpResult.chatName} and received ${httpResult.text}`);

  await request('/internal/mcp/servers', {
    method: 'POST',
    body: {
      server: {
        id: stdioServerId,
        name: 'GUI3 stdio Echo Test',
        transport: 'stdio',
        command: process.execPath,
        args: [mockPath],
        timeout_ms: 10000,
        enabled: true,
      },
    },
  });
  createdIds.push(stdioServerId);
  const stdioConnected = await request(`/internal/mcp/servers/${stdioServerId}/connect`, { method: 'POST' });
  assert(stdioConnected.server?.connected === true, 'stdio server did not reach connected state.');
  const stdioResult = await verifyDiscoveredTool(stdioServerId, 'echo:hello-mcp');
  console.log(`PASS stdio: discovered ${stdioResult.chatName} and received ${stdioResult.text}`);

  const requiredHttpSequence = ['initialize', 'notifications/initialized', 'tools/list', 'tools/call'];
  for (const method of requiredHttpSequence) {
    assert(httpMock.observedMethods.includes(method), `HTTP mock did not observe ${method}.`);
  }
  console.log('PASS: Streamable HTTP and local stdio transports both completed end-to-end.');
} finally {
  for (const id of createdIds.reverse()) {
    await request(`/internal/mcp/servers/${id}/disconnect`, { method: 'POST' }).catch(() => undefined);
    await request(`/internal/mcp/servers/${id}`, { method: 'DELETE' }).catch(() => undefined);
  }
  await httpMock?.close().catch(() => undefined);
}
