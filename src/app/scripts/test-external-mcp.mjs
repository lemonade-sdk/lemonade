#!/usr/bin/env node
import { fileURLToPath } from 'node:url';

const baseUrl = (process.env.LEMONADE_BASE_URL || 'http://127.0.0.1:13305').replace(/\/+$/, '');
const apiKey = process.env.LEMONADE_API_KEY || '';
const adminApiKey = process.env.LEMONADE_ADMIN_API_KEY || apiKey;
const serverId = `gui3-echo-${process.pid}`;
const mockPath = fileURLToPath(new URL('./mock-mcp-server.mjs', import.meta.url));

async function request(path, init = {}) {
  const headers = { 'Content-Type': 'application/json', ...(adminApiKey ? { Authorization: `Bearer ${adminApiKey}` } : {}), ...(init.headers || {}) };
  const response = await fetch(`${baseUrl}${path}`, { ...init, headers, body: init.body === undefined ? undefined : JSON.stringify(init.body) });
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

function assert(condition, message) { if (!condition) throw new Error(message); }

let created = false;
try {
  console.log(`Testing external MCP client host at ${baseUrl}`);
  await request('/internal/mcp/servers', { method: 'POST', body: { server: { id: serverId, name: 'GUI3 Echo Test', transport: 'stdio', command: process.execPath, args: [mockPath], timeout_ms: 10000, enabled: true } } });
  created = true;
  const connected = await request(`/internal/mcp/servers/${serverId}/connect`, { method: 'POST' });
  assert(connected.server?.connected === true, 'Server did not reach connected state.');
  const tools = await request('/internal/mcp/tools');
  const echo = tools.tools?.find(tool => tool.server_id === serverId && tool.name === 'echo');
  assert(echo, 'Echo tool was not discovered.');
  assert(echo.chat_name && echo.chat_name !== 'echo', 'External tool was not assigned a namespaced chat_name.');
  const called = await request(`/internal/mcp/servers/${serverId}/tools/call`, { method: 'POST', body: { name: 'echo', arguments: { text: 'hello-mcp' }, timeout_ms: 10000 } });
  const text = called.result?.content?.find(block => block.type === 'text')?.text;
  assert(text === 'echo:hello-mcp', `Unexpected tool result: ${JSON.stringify(called)}`);
  console.log(`PASS: discovered ${echo.chat_name} and received ${text}`);
} finally {
  if (created) {
    await request(`/internal/mcp/servers/${serverId}/disconnect`, { method: 'POST' }).catch(() => undefined);
    await request(`/internal/mcp/servers/${serverId}`, { method: 'DELETE' }).catch(() => undefined);
  }
}
