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
    { id: 'fast-chat', name: 'fast-chat', downloaded: true, labels: ['chat'], recipe: 'llamacpp', type: 'chat' },
    { id: 'smart-chat', name: 'smart-chat', downloaded: true, labels: ['chat'], recipe: 'llamacpp', type: 'chat' },
    {
      id: 'user.router', name: 'user.router', display_name: 'Test Router', downloaded: true, labels: ['custom', 'router', 'chat'],
      recipe: 'collection.router', type: 'chat', components: ['fast-chat', 'smart-chat'],
      routing: {
        candidates: ['fast-chat', 'smart-chat'],
        default_model: 'smart-chat',
        rules: [{ route_to: 'fast-chat', match: { max_chars: 300 } }],
      },
    },
  ];
  Object.defineProperty(api, 'loadedModels', { configurable: true, get: () => [] });
  Object.defineProperty(api, 'allModels', { configurable: true, get: () => modelInfos });
  (api as any).health = async () => ({ status: 'ok', version: 'test', all_models_loaded: [{ model_name: 'image-model', recipe: 'sd-cpp', type: 'image', device: 'gpu' }] });
  (api as any).models = async () => ({ data: modelInfos });
  (api as any).modelDetail = async (name: string) => modelInfos.find(model => model.id === name || model.name === name) || { id: name, name };
  const calls: string[] = [];
  const loadRequests: Array<{ name: string; options?: Record<string, unknown> }> = [];
  const unloadRequests: string[] = [];
  const registrationRequests: Array<{ name: string; options?: Record<string, unknown> }> = [];
  (api as any).loadModel = async (name: string, options?: Record<string, unknown>) => {
    calls.push(`load:${name}`);
    loadRequests.push({ name, options });
  };
  (api as any).unloadModel = async (name?: string) => {
    unloadRequests.push(name || '');
    return { unloaded: name };
  };
  (api as any).registerModelDefinition = async (name: string, options?: Record<string, unknown>) => {
    registrationRequests.push({ name, options });
    return { registered: name };
  };
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
  assert.match(lemonade!.systemPrompt || '', /collection\.router/, 'builtin Lemonade guidance must explain Router semantics');

  const routerList = await lemonade!.execute({
    id: 'call-router-list', type: 'function',
    function: { name: 'list_models', arguments: JSON.stringify({ capability: 'router', status: 'local' }) },
  });
  assert.notEqual(routerList.error, true);
  const routerListData = JSON.parse(routerList.content);
  assert.deepEqual(routerListData.loaded, []);
  assert.deepEqual(routerListData.downloaded.map((model: any) => model.id), ['user.router']);
  assert.equal(routerListData.downloaded[0].capability, 'router');
  assert.equal(routerListData.downloaded[0].deployment_capability, 'chat');
  assert.equal(routerListData.downloaded[0].mode, 'router');

  const chatList = await lemonade!.execute({
    id: 'call-chat-list', type: 'function',
    function: { name: 'list_models', arguments: JSON.stringify({ capability: 'chat', status: 'local' }) },
  });
  const chatListData = JSON.parse(chatList.content);
  assert.deepEqual(chatListData.loaded, [], 'loaded models from other capabilities must not leak through the filter');
  const routerInChat = chatListData.downloaded.find((model: any) => model.id === 'user.router');
  assert.equal(routerInChat?.deployment_capability, 'chat', 'Router must remain deployable through chat while retaining Router identity');
  assert.ok(chatListData.downloaded.filter((model: any) => model.id !== 'user.router').every((model: any) => model.capability === 'chat'), 'chat filtering must not leak unrelated modes');

  const routerInfo = await lemonade!.execute({
    id: 'call-router-info', type: 'function',
    function: { name: 'get_model_info', arguments: JSON.stringify({ model_name: 'user.router' }) },
  });
  const routerInfoData = JSON.parse(routerInfo.content);
  assert.equal(routerInfoData.is_router, true);
  assert.equal(routerInfoData.capability, 'router');
  assert.equal(routerInfoData.deployment_capability, 'chat');
  assert.equal(routerInfoData.mode, 'router');
  assert.equal(routerInfoData.router.mode, 'rules');
  assert.equal(routerInfoData.router.default_model, 'smart-chat');
  assert.deepEqual(routerInfoData.router.candidates, ['fast-chat', 'smart-chat']);

  const routerLoad = await lemonade!.execute({
    id: 'call-router-load', type: 'function',
    function: { name: 'load_model', arguments: JSON.stringify({ model_name: 'user.router', recipe: 'llamacpp-cpu', backend: 'cpu', n_ctx: 8192 }) },
  });
  const routerLoadData = JSON.parse(routerLoad.content);
  assert.notEqual(routerLoad.error, true);
  assert.equal(routerLoadData.mode, 'router');
  assert.equal(routerLoadData.status, 'ready');
  assert.equal(routerLoadData.virtual, true);
  assert.equal(loadRequests.some(request => request.name === 'user.router'), false, 'Router readiness must never POST /load for collection.router');
  assert.equal(routerLoadData.ignored_router_options.backend, 'cpu');
  assert.ok(Array.isArray(routerLoadData.dependencies));
  assert.ok(routerLoadData.dependencies.some((dependency: any) => dependency.name === 'fast-chat' && dependency.present));

  const routerUnload = await lemonade!.execute({
    id: 'call-router-unload', type: 'function',
    function: { name: 'unload_model', arguments: JSON.stringify({ model_name: 'user.router' }) },
  });
  const routerUnloadData = JSON.parse(routerUnload.content);
  assert.notEqual(routerUnload.error, true);
  assert.equal(routerUnloadData.status, 'not_applicable');
  assert.equal(routerUnloadData.mode, 'router');
  assert.equal(unloadRequests.length, 0, 'Router unload must not call the runtime unload endpoint');

  const routerPull = await lemonade!.execute({
    id: 'call-router-pull', type: 'function',
    function: { name: 'pull_model', arguments: JSON.stringify({ model_name: 'user.router' }) },
  });
  assert.equal(routerPull.error, true);
  assert.match(routerPull.content, /registered Router definition/);

  let deletedModel = '';
  (api as any).deleteModel = async (name: string) => {
    deletedModel = name;
    return { deleted: name };
  };
  const routerDelete = await lemonade!.execute({
    id: 'call-router-delete', type: 'function',
    function: { name: 'delete_model', arguments: JSON.stringify({ model_name: 'user.router' }) },
  });
  const routerDeleteData = JSON.parse(routerDelete.content);
  assert.notEqual(routerDelete.error, true);
  assert.equal(routerDeleteData.mode, 'router');
  assert.equal(deletedModel, 'user.router', 'Router deletion must call the server definition delete endpoint');

  const generate3d = lemonade!.tools.find(tool => (tool as any).function?.name === 'generate_3d');
  assert.ok(generate3d);
  calls.length = 0;
  const generated = await lemonade!.execute({ id: 'call-3d', type: 'function', function: { name: 'generate_3d', arguments: JSON.stringify({ prompt: 'a small turbine' }) } });
  assert.notEqual(generated.error, true);
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
  assert.notEqual(edited.error, true);
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
