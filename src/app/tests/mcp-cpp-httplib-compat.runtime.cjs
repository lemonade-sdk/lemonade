const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const sourcePath = path.resolve(__dirname, '../../cpp/server/mcp_client.cpp');
const source = fs.readFileSync(sourcePath, 'utf8');

assert.doesNotMatch(source, /httplib::stream/, 'MCP transport must compile against system cpp-httplib 0.26');
assert.doesNotMatch(source, /set_payload_max_length/, 'client-side payload limiting is unavailable in cpp-httplib 0.26');
assert.match(source, /httplib::Request request;/, 'MCP HTTP requests should use the stable Request API');
assert.match(source, /client\.send\(request\)/, 'MCP HTTP requests should use Client::send');
assert.match(source, /request\.response_handler/, 'response headers must be inspected before streaming body data');
assert.match(source, /request\.content_receiver/, 'response bodies must be consumed incrementally');
assert.match(source, /kMaxMessageBytes - received/, 'incremental reads must retain the response size limit');
assert.match(source, /matching_response_received/, 'SSE reads must stop once the requested JSON-RPC response arrives');
assert.match(source, /class McpHttpStatusError/, 'HTTP status errors must preserve their status code');
assert.match(source, /error\.status_code\(\) == 404/, 'expired HTTP sessions must be recognized explicitly');
assert.match(source, /invalidate_expired_http_session/, 'expired sessions must clear connected runtime state');
assert.match(source, /json_rpc_is_response/, 'matching IDs must only accept JSON-RPC responses');
assert.match(source, /json_rpc_is_request/, 'server-initiated JSON-RPC requests must be detected');
assert.match(source, /json_rpc_client_response/, 'server requests must receive a JSON-RPC response');
assert.match(source, /method == "ping"/, 'MCP ping requests must receive an empty success result');
assert.match(source, /http_mcp_post\([\s\S]*json_rpc_client_response\(candidate\)/, 'SSE server requests must be answered over Streamable HTTP');

console.log('MCP cpp-httplib 0.26 compatibility contract passed.');
