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
  const observedClientResponses = [];
  const pendingClientResponses = new Map();
  let initializeCount = 0;
  let activeSessionId = '';
  let expireNextToolCall = false;

  function waitForClientResponse(id) {
    const key = JSON.stringify(id);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        pendingClientResponses.delete(key);
        reject(new Error(`Timed out waiting for MCP client response ${key}.`));
      }, 5000);
      timer.unref();
      pendingClientResponses.set(key, {
        resolve(message) {
          clearTimeout(timer);
          resolve(message);
        },
      });
    });
  }

  const server = http.createServer(async (req, res) => {
    try {
      if (req.url !== '/mcp') {
        res.writeHead(404).end();
        return;
      }
      if (req.method === 'DELETE') {
        assert(activeSessionId, 'DELETE arrived without an active MCP session.');
        assert(
          req.headers['mcp-session-id'] === activeSessionId,
          'DELETE did not carry the active MCP session id.',
        );
        activeSessionId = '';
        res.writeHead(204).end();
        return;
      }
      if (req.method !== 'POST') {
        res.writeHead(405, { Allow: 'POST, DELETE' }).end();
        return;
      }

      const message = await readJsonBody(req);
      const isInitialize = message.method === 'initialize';
      if (!isInitialize) {
        const label = message.method || 'JSON-RPC response';
        assert(activeSessionId, `${label} arrived without an active session.`);
        assert(
          req.headers['mcp-session-id'] === activeSessionId,
          `${label} did not carry the active MCP session id.`,
        );
        assert(
          req.headers['mcp-protocol-version'] === protocolVersion,
          `${label} did not carry MCP-Protocol-Version.`,
        );
      }

      const isClientResponse = !message.method
        && message.id !== undefined
        && (
          Object.prototype.hasOwnProperty.call(message, 'result')
          || Object.prototype.hasOwnProperty.call(message, 'error')
        );
      if (isClientResponse) {
        observedClientResponses.push(message);
        const key = JSON.stringify(message.id);
        const pending = pendingClientResponses.get(key);
        assert(pending, `Unexpected MCP client response id: ${key}`);
        pendingClientResponses.delete(key);
        pending.resolve(message);
        res.writeHead(202).end();
        return;
      }

      observedMethods.push(message.method || '');
      if (isInitialize) {
        initializeCount += 1;
        activeSessionId = `${sessionId}-${initializeCount}`;
        res.writeHead(200, {
          'Content-Type': 'application/json',
          'Mcp-Session-Id': activeSessionId,
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
        // Send a server request with the same id as the outstanding
        // client request. Matching the id alone would misclassify it.
        const pingResponse = waitForClientResponse(message.id);
        const rootsRequestId = `server-roots-${initializeCount}-${message.id}`;
        const rootsResponse = waitForClientResponse(rootsRequestId);

        res.writeHead(200, {
          'Content-Type': 'text/event-stream',
          'Cache-Control': 'no-cache',
          'Mcp-Session-Id': activeSessionId,
        });
        res.write(`event: message\ndata: ${JSON.stringify({
          jsonrpc: '2.0',
          id: message.id,
          method: 'ping',
        })}\n\n`);
        res.write(`event: message\ndata: ${JSON.stringify({
          jsonrpc: '2.0',
          id: rootsRequestId,
          method: 'roots/list',
          params: {},
        })}\n\n`);

        const [pingReply, rootsReply] = await Promise.all([
          pingResponse,
          rootsResponse,
        ]);
        assert(
          pingReply.result && Object.keys(pingReply.result).length === 0,
          'Client did not answer ping with an empty result.',
        );
        assert(
          rootsReply.error?.code === -32601,
          'Client did not reject an unsupported request with Method not found.',
        );

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
        if (expireNextToolCall) {
          expireNextToolCall = false;
          activeSessionId = '';
          res.writeHead(404, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ error: 'MCP session expired' }));
          return;
        }

        const text = String(message.params?.arguments?.text || '');
        res.writeHead(200, {
          'Content-Type': 'application/json',
          'Mcp-Session-Id': activeSessionId,
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
      if (!res.headersSent) {
        res.writeHead(500, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: String(error?.message || error) }));
      } else {
        res.destroy(error instanceof Error ? error : undefined);
      }
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
    observedClientResponses,
    get initializeCount() { return initializeCount; },
    expireCurrentSessionOnNextToolCall() { expireNextToolCall = true; },
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

  const initializeCountBeforeExpiry = httpMock.initializeCount;
  httpMock.expireCurrentSessionOnNextToolCall();
  let expiryError;
  try {
    await request(`/internal/mcp/servers/${httpServerId}/tools/call`, {
      method: 'POST',
      body: {
        name: 'echo',
        arguments: { text: 'expire-me' },
        timeout_ms: 10000,
      },
    });
  } catch (error) {
    expiryError = error;
  }
  assert(
    expiryError && /404|expired/i.test(String(expiryError.message)),
    'Expired MCP session did not surface the HTTP 404.',
  );

  const recoveredCall = await request(
    `/internal/mcp/servers/${httpServerId}/tools/call`,
    {
      method: 'POST',
      body: {
        name: 'echo',
        arguments: { text: 'after-expiry' },
        timeout_ms: 10000,
      },
    },
  );
  assert(
    textResult(recoveredCall.result) === 'http-echo:after-expiry',
    'Client did not recover by establishing a new MCP session.',
  );
  assert(
    httpMock.initializeCount > initializeCountBeforeExpiry,
    'Expired session was reused instead of reinitializing.',
  );
  assert(
    httpMock.observedClientResponses.some(
      message => message.result && Object.keys(message.result).length === 0,
    ),
    'HTTP mock did not observe the client ping response.',
  );
  assert(
    httpMock.observedClientResponses.some(message => message.error?.code === -32601),
    'HTTP mock did not observe Method not found for an unsupported server request.',
  );
  console.log('PASS HTTP: server requests are answered and expired sessions recover on the next call.');

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
