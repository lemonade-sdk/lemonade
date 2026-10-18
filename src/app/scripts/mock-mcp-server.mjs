#!/usr/bin/env node
let buffer = '';

function send(message) {
  process.stdout.write(`${JSON.stringify(message)}\n`);
}

function handle(message) {
  if (!message || typeof message !== 'object') return;
  if (message.method === 'notifications/initialized') return;
  const id = message.id ?? null;
  if (message.method === 'initialize') {
    send({ jsonrpc: '2.0', id, result: { protocolVersion: '2025-06-18', capabilities: { tools: {} }, serverInfo: { name: 'lemonade-gui3-echo', version: '1.0.0' } } });
  } else if (message.method === 'tools/list') {
    send({ jsonrpc: '2.0', id, result: { tools: [{ name: 'echo', description: 'Return the supplied text.', inputSchema: { type: 'object', properties: { text: { type: 'string' } }, required: ['text'], additionalProperties: false } }] } });
  } else if (message.method === 'tools/call') {
    const text = String(message.params?.arguments?.text ?? '');
    send({ jsonrpc: '2.0', id, result: { content: [{ type: 'text', text: `echo:${text}` }], structuredContent: { echoed: text }, isError: false } });
  } else if (message.method === 'ping') {
    send({ jsonrpc: '2.0', id, result: {} });
  } else if (id !== null) {
    send({ jsonrpc: '2.0', id, error: { code: -32601, message: `Unknown method: ${message.method}` } });
  }
}

function consume() {
  while (true) {
    const newline = buffer.indexOf('\n');
    if (newline < 0) return;
    const line = buffer.slice(0, newline).trim();
    buffer = buffer.slice(newline + 1);
    if (!line) continue;
    try {
      handle(JSON.parse(line));
    } catch (error) {
      send({ jsonrpc: '2.0', id: null, error: { code: -32700, message: String(error) } });
    }
  }
}

process.stdin.setEncoding('utf8');
process.stdin.on('data', chunk => {
  buffer += chunk;
  consume();
});
process.stdin.on('end', () => process.exit(0));
process.on('SIGTERM', () => process.exit(0));
