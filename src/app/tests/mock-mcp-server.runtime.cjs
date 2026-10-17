const assert = require('node:assert/strict');
const path = require('node:path');
const { spawn } = require('node:child_process');

function line(message) {
  return `${JSON.stringify(message)}\n`;
}

function reader(stream) {
  let buffer = '';
  const pending = [];
  stream.setEncoding('utf8');
  stream.on('data', chunk => {
    buffer += chunk;
    while (true) {
      const newline = buffer.indexOf('\n');
      if (newline < 0) break;
      const text = buffer.slice(0, newline).trim();
      buffer = buffer.slice(newline + 1);
      if (!text) continue;
      const waiter = pending.shift();
      if (!waiter) throw new Error(`Unexpected MCP response: ${text}`);
      waiter.resolve(JSON.parse(text));
    }
  });
  stream.on('error', error => pending.splice(0).forEach(waiter => waiter.reject(error)));
  return () => new Promise((resolve, reject) => pending.push({ resolve, reject }));
}

(async () => {
  const child = spawn(process.execPath, [path.resolve(__dirname, '../scripts/mock-mcp-server.mjs')], { stdio: ['pipe', 'pipe', 'inherit'] });
  const next = reader(child.stdout);
  try {
    child.stdin.write(line({ jsonrpc: '2.0', id: 1, method: 'initialize', params: { protocolVersion: '2025-06-18' } }));
    const initialized = await next();
    assert.equal(initialized.result.serverInfo.name, 'lemonade-gui3-echo');
    child.stdin.write(line({ jsonrpc: '2.0', method: 'notifications/initialized' }));
    child.stdin.write(line({ jsonrpc: '2.0', id: 2, method: 'tools/list', params: {} }));
    const listed = await next();
    assert.equal(listed.result.tools[0].name, 'echo');
    child.stdin.write(line({ jsonrpc: '2.0', id: 3, method: 'tools/call', params: { name: 'echo', arguments: { text: 'contract' } } }));
    const called = await next();
    assert.equal(called.result.content[0].text, 'echo:contract');
    assert.deepEqual(called.result.structuredContent, { echoed: 'contract' });
    console.log('Mock MCP stdio protocol test passed.');
  } finally {
    child.kill('SIGTERM');
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
