import assert from 'node:assert/strict';
import api from '../../src/api';
import {
  LEMONADE_MCP_TOOLS,
  buildSelectedMcpRuntime,
  composeMcpRuntimes,
} from '../../src/tools/mcpRuntime';
import type { ChatToolRuntime } from '../../src/hooks/useChatStreaming';

async function main(): Promise<void> {
  api.setSessionApiKey('regular-key');
  api.setSessionAdminApiKey('');
  assert.equal(api.adminApiKey, 'regular-key', 'admin auth must fall back to the regular API key');
  api.setSessionAdminApiKey('admin-key');
  assert.equal(api.adminApiKey, 'admin-key', 'an explicit admin key must override the regular API key');

  const originalFetch = globalThis.fetch;
  const authHeaders: string[] = [];
  globalThis.fetch = (async (_input: string | URL | Request, init?: RequestInit) => {
    const headers = new Headers(init?.headers);
    authHeaders.push(headers.get('Authorization') || '');
    return new Response(JSON.stringify({ servers: [] }), {
      status: 200,
      headers: { 'Content-Type': 'application/json' },
    });
  }) as typeof fetch;
  try {
    api.setSessionApiKey('');
    api.setSessionAdminApiKey('');
    assert.deepEqual(await api.listMcpServers(), []);
    assert.deepEqual(authHeaders, [], 'default MCP discovery must not probe a fail-closed admin endpoint without credentials');

    api.setSessionAdminApiKey('admin-key');
    assert.deepEqual(await api.listMcpServers(), []);
    assert.deepEqual(authHeaders, ['Bearer admin-key'], 'credentialed MCP discovery must use admin auth directly');
  } finally {
    globalThis.fetch = originalFetch;
  }

  const toolNames = LEMONADE_MCP_TOOLS.map(tool => tool.function.name);
  for (const required of ['list_models', 'load_model', 'generate_image', 'transcribe_audio', 'generate_3d']) {
    assert.ok(toolNames.includes(required), `missing built-in Lemon-Tools MCP tool: ${required}`);
  }

  const modelInfos = [
    { id: 'image-model', name: 'image-model', downloaded: true, labels: ['image'], recipe: 'sd-cpp' },
    { id: 'image-edit-model', name: 'image-edit-model', downloaded: true, labels: ['image', 'image-edit'], recipe: 'sd-cpp' },
    { id: 'remote-image-model', name: 'remote-image-model', downloaded: false, labels: ['image'], recipe: 'sd-cpp' },
    { id: '3d-model', name: '3d-model', downloaded: true, labels: ['3d'], recipe: 'trellis' },
  ];
  Object.defineProperty(api, 'loadedModels', { configurable: true, get: () => [] });
  Object.defineProperty(api, 'allModels', { configurable: true, get: () => modelInfos });
  (api as any).health = async () => ({ status: 'ok', version: 'test', all_models_loaded: [] });
  (api as any).models = async () => ({ data: modelInfos });
  const calls: string[] = [];
  (api as any).loadModel = async (name: string) => { calls.push(`load:${name}`); };
  (api as any).imageGeneration = async (name: string, prompt: string) => {
    calls.push(`image:${name}`);
    assert.match(prompt, /studio lighting/);
    return ['data:image/png;base64,cmVmZXJlbmNl'];
  };
  (api as any).model3dGeneration = async (name: string, image: string) => {
    calls.push(`3d:${name}`);
    assert.equal(image, 'data:image/png;base64,cmVmZXJlbmNl');
    return { url: 'blob:test-glb', blob: new Blob(['glb'], { type: 'model/gltf-binary' }), filename: 'asset.glb' };
  };

  const lemonade = await buildSelectedMcpRuntime(['lemonade']);
  assert.ok(lemonade);
  const generate3d = lemonade!.tools.find(tool => (tool as any).function?.name === 'generate_3d');
  assert.ok(generate3d);
  const generated = await lemonade!.execute({ id: 'call-3d', type: 'function', function: { name: 'generate_3d', arguments: JSON.stringify({ prompt: 'a small turbine' }) } });
  assert.equal(generated.error, undefined);
  assert.deepEqual(generated.artifacts?.map(item => item.type), ['image', 'model3d']);
  assert.deepEqual(calls, ['load:image-model', 'image:image-model', 'load:3d-model', '3d:3d-model']);

  (api as any).imageEdit = async (name: string, _prompt: string, image: string) => {
    assert.equal(name, 'image-edit-model');
    assert.equal(image, 'data:image/png;base64,YXR0YWNoZWQ=');
    return ['data:image/png;base64,ZWRpdGVk'];
  };
  const lemonadeWithImage = await buildSelectedMcpRuntime(
    ['lemonade'],
    { attachedImages: ['data:image/png;base64,YXR0YWNoZWQ='] },
  );
  const edited = await lemonadeWithImage!.execute({
    id: 'call-edit',
    type: 'function',
    function: { name: 'edit_image', arguments: JSON.stringify({ prompt: 'make it blue' }) },
  });
  assert.equal(edited.error, undefined);
  assert.equal(edited.artifacts?.[0]?.url, 'data:image/png;base64,ZWRpdGVk');

  const wrongCapability = await lemonade!.execute({
    id: 'call-wrong-capability',
    type: 'function',
    function: { name: 'generate_audio', arguments: JSON.stringify({ prompt: 'tone', model: 'image-model' }) },
  });
  assert.equal(wrongCapability.error, true);
  assert.match(wrongCapability.content, /not audio-generation/);

  const notDownloaded = await lemonade!.execute({
    id: 'call-not-downloaded',
    type: 'function',
    function: { name: 'generate_image', arguments: JSON.stringify({ prompt: 'test', model: 'remote-image-model' }) },
  });
  assert.equal(notDownloaded.error, true);
  assert.match(notDownloaded.content, /not downloaded/);

  (api as any).listMcpServers = async () => [{ id: 'srv', name: 'Echo', transport: 'stdio', enabled: true, connected: true, status: 'connected', tools: [{ name: 'echo' }] }];
  (api as any).listMcpTools = async () => [{
    server_id: 'srv', server_name: 'Echo', name: 'echo', chat_name: 'srv__echo', description: 'Echo text',
    inputSchema: { type: 'object', properties: { text: { type: 'string' } } },
    openai_tool: { type: 'function', function: { name: 'echo', description: 'unsafe raw name', parameters: {} } },
  }];
  (api as any).callMcpTool = async (_server: string, _name: string, args: Record<string, unknown>) => ({ server_id: 'srv', tool: 'echo', result: { content: [{ type: 'text', text: `echo:${args.text}` }], isError: false } });
  const external = await buildSelectedMcpRuntime(['srv']);
  assert.ok(external);
  assert.equal((external!.tools[0] as any).function.name, 'srv__echo', 'chat tool name must remain namespaced');
  const echoed = await external!.execute({ id: 'call-echo', type: 'function', function: { name: 'srv__echo', arguments: '{"text":"hello"}' } });
  assert.equal(echoed.content, 'echo:hello');

  const first: ChatToolRuntime = { tools: [{ type: 'function', function: { name: 'same', parameters: {} } }], execute: async call => ({ tool_call_id: call.id, role: 'tool', content: 'first' }) };
  const second: ChatToolRuntime = { tools: [{ type: 'function', function: { name: 'same', parameters: {} } }], execute: async call => ({ tool_call_id: call.id, role: 'tool', content: 'second' }) };
  const composed = composeMcpRuntimes([first, second]);
  assert.equal(composed?.tools.length, 1);
  assert.equal((await composed!.execute({ id: 'dup', type: 'function', function: { name: 'same', arguments: '{}' } })).content, 'first');

  console.log('MCP runtime contract tests passed.');
}

export default main();
