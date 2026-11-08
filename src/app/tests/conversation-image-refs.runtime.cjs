const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = process.env.LEMONADE_TEST_ROOT || path.resolve(__dirname, '..');
function transpile(source) {
  const result = ts.transpileModule(source, {
    fileName: 'images.ts',
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
    reportDiagnostics: true,
  });
  assert.equal((result.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error).length, 0);
  return result.outputText;
}

let fileReads = 0;
class ImageReader {
  readAsDataURL(blob) {
    fileReads++;
    blob.arrayBuffer().then(bytes => {
      this.result = `data:${blob.type};base64,${Buffer.from(bytes).toString('base64')}`;
      this.onload();
    }).catch(error => { this.error = error; this.onerror(); });
  }
}
function registryModule() {
  const src = fs.readFileSync(path.join(root, 'src/features/images/imageRegistry.ts'), 'utf8');
  const module = { exports: {} };
  new Function('module', 'exports', 'fetch', 'FileReader', 'createImageBitmap', 'document', transpile(src))(
    module, module.exports, (...args) => global.fetch(...args), ImageReader, undefined, undefined,
  );
  return module.exports;
}
function tinyPng(byte) {
  return new File([Uint8Array.of(byte, byte + 1, byte + 2)], `original-${byte}.png`, { type: 'image/png' });
}
function imageDataUrl(byte) {
  return `data:image/png;base64,${Buffer.from([byte, byte + 1]).toString('base64')}`;
}

async function testRegistry() {
  const { ConversationImageRegistry } = registryModule();
  const registry = new ConversationImageRegistry();
  const originals = [tinyPng(1), tinyPng(3), tinyPng(5)];
  const first = registry.addUploads('garden', originals, 'message-1');
  assert.equal(first.length, 3);
  assert.equal(new Set(first).size, 3);
  assert.deepEqual(registry.defaultsForMessage('garden', 'message-1'), first);
  assert.deepEqual(registry.selectForEdit('garden', undefined, 'message-1').images, originals);
  assert.ok(registry.listForPlanner('garden', 'message-1').includes(`image 2 -> ${first[1]}`));

  const other = registry.addUploads('house', [tinyPng(9)], 'message-house')[0];
  const largeReferences = Array.from({ length: 8 }, () => ({ type: 'image/png', size: 64 * 1024 * 1024 }));
  const limited = new ConversationImageRegistry();
  assert.equal(limited.addUploads('limit', largeReferences, 'bulky').length, 8);
  assert.throws(() => limited.addUploads('limit', [tinyPng(61)], 'overflow'), /memory limit/);
  const refreshed = new ConversationImageRegistry();
  assert.notEqual(refreshed.addUploads('garden', [tinyPng(99)], 'new-session')[0], first[0], 'Old session IDs must not resurrect');
  assert.ok(!first.includes(other), 'Conversation IDs must not collide');
  assert.throws(() => registry.resolve('house', [first[0]]), /unavailable/);
  assert.throws(() => registry.resolve('garden', [other]), /unavailable/);
  assert.throws(() => registry.selectForEdit('garden', [first[0], other]), /unavailable/);
  assert.throws(() => registry.selectForEdit('garden', []), /nonempty/);
  const epoch = first[0].match(/^img_u([0-9a-f]{12})_/)[1];
  assert.throws(() => registry.selectForEdit('garden', [`img_g${epoch}_99999`]), /unavailable/);
  assert.deepEqual(registry.selectForEdit('garden', [first[1], first[0]], 'message-1').ids, [first[1], first[0]]);

  const result = await registry.addOutputs('garden', [imageDataUrl(20), imageDataUrl(22)], 'edited', [first[1], first[0]], 'older summer tree');
  assert.equal(result.length, 2);
  assert.notEqual(result[0], result[1]);
  assert.equal(registry.resolve('garden', [result[0]])[0].origin, 'edited');
  assert.deepEqual(registry.resolve('garden', [result[0]])[0].sourceImageIds, [first[1], first[0]]);
  assert.deepEqual(registry.defaultsForMessage('garden', 'next-message'), [result[1]]);
  const newestAnalysis = await registry.getAnalysisInput('garden', undefined, 'message-1');
  assert.equal(newestAnalysis, imageDataUrl(22), 'Inspection defaults to the latest generated result after an edit within this user turn');
  assert.deepEqual(registry.selectForEdit('garden', [first[0]]).images, [originals[0]], 'Original bytes must survive edits');
  assert.ok(registry.listForPlanner('garden', 'next-message').includes('older summer tree'));

  const newUpload = registry.addUploads('garden', [tinyPng(7)], 'message-2');
  assert.notEqual(newUpload[0], first[0]);
  assert.deepEqual(registry.defaultsForMessage('garden', 'message-2'), newUpload);
  assert.equal(await registry.getAnalysisInput('garden', undefined, 'message-2'), await registry.getAnalysisInput('garden', newUpload[0]));
  assert.deepEqual(registry.defaultsForMessage('garden', 'later-message'), [newUpload[0]]);

  const before = fileReads;
  const firstVision = await registry.getAnalysisInput('garden', result[0]);
  const secondVision = await registry.getAnalysisInput('garden', result[0]);
  assert.equal(firstVision, secondVision);
  assert.equal(fileReads - before, 1, 'Analysis representation should be lazily cached');
  assert.deepEqual(registry.resolve('garden', [result[0]])[0].original.size, 2, 'Analysis must not overwrite the original');
  await assert.rejects(registry.getAnalysisInput('house', result[0]), /unavailable/);
  await assert.rejects(registry.getAnalysisInput('garden', `img_g${epoch}_999`), /unavailable/);

  registry.clearConversation('garden');
  assert.throws(() => registry.resolve('garden', [first[0]]), /unavailable/);
  assert.throws(() => registry.addUploads('garden', [tinyPng(11)], 'message-new'), /closed/);
  const afterDelete = registry.addUploads('new-garden', [tinyPng(11)], 'message-new')[0];
  assert.ok(!first.includes(afterDelete), 'IDs are not reused after deletion');
  assert.ok(registry.resolve('house', [other])[0], 'Deleting one conversation must not affect another');
  registry.clear();
  assert.throws(() => registry.resolve('house', [other]), /unavailable/);

  const pending = new ConversationImageRegistry();
  const originalFetch = global.fetch;
  let release;
  try {
    global.fetch = () => new Promise(resolve => { release = resolve; });
    const unfinished = pending.addOutputs('cancelled', [imageDataUrl(8)], 'generated');
    pending.clearConversation('cancelled');
    release(new Response(tinyPng(17)));
    await assert.rejects(unfinished, /closed/);
  } finally {
    global.fetch = originalFetch;
  }

  const cancellation = new ConversationImageRegistry();
  const controller = new AbortController();
  try {
    let releaseCancelled;
    global.fetch = () => new Promise(resolve => { releaseCancelled = resolve; });
    const inFlight = cancellation.addOutputs('stopped', [imageDataUrl(44)], 'generated', [], 'stopped job', controller.signal);
    controller.abort();
    releaseCancelled(new Response(tinyPng(46)));
    await assert.rejects(inFlight, /cancelled/);
    assert.deepEqual(cancellation.defaultsForMessage('stopped'), [], 'Cancelled inference must not create a hidden latest image');
  } finally {
    global.fetch = originalFetch;
  }
}

function extractImageEdit() {
  const source = fs.readFileSync(path.join(root, 'src/api.ts'), 'utf8');
  const from = source.indexOf('  async imageEdit(');
  const to = source.indexOf('  async audioTranscription(', from);
  assert.ok(from >= 0 && to > from);
  const module = { exports: {} };
  const method = source.slice(from, to);
  const code = transpile(`class TestClient { ${method} }\nexports.TestClient = TestClient;`);
  const fromData = input => {
    const match = /^data:(image\/[^;]+);base64,([^\s]+)$/.exec(input);
    if (!match) throw new Error('Invalid test data URL');
    return new Blob([Buffer.from(match[2], 'base64')], { type: match[1] });
  };
  new Function('module', 'exports', 'FormData', 'Blob', 'File', 'blobFromDataUrl', code)(
    module, module.exports, FormData, Blob, File, fromData,
  );
  return module.exports.TestClient;
}

async function testImageEditTransport() {
  const TestClient = extractImageEdit();
  const client = new TestClient();
  const calls = [];
  client._json = async (url, request) => {
    calls.push({ url, request });
    return { data: [{ b64_json: Buffer.from('ok').toString('base64') }] };
  };
  const first = new File([Uint8Array.of(11, 22, 33)], 'original.jpeg', { type: 'image/jpeg' });
  const second = new File([Uint8Array.of(44, 55)], 'reference.png', { type: 'image/png' });
  const third = new File([Uint8Array.of(66)], 'third.webp', { type: 'image/webp' });
  assert.deepEqual(await client.imageEdit('model', 'edit', first), [imageDataUrlFromBytes('ok')]);
  let form = calls.at(-1).request.body;
  assert.deepEqual([...form.keys()].filter(k => k.startsWith('image')), ['image']);
  assert.equal(form.get('size'), null, 'Default image edit must not force canvas size');
  assert.equal(form.get('image').name, first.name);
  assert.equal(form.get('image').type, first.type);
  assert.deepEqual(new Uint8Array(await form.get('image').arrayBuffer()), Uint8Array.of(11, 22, 33));

  await client.imageEdit('model', 'composite', [first, second, third], { size: '1536x1024' });
  form = calls.at(-1).request.body;
  assert.deepEqual([...form.keys()].filter(k => k.startsWith('image')), ['image[]', 'image[]', 'image[]']);
  assert.equal(form.get('size'), '1536x1024');
  assert.deepEqual(form.getAll('image[]').map(file => file.name), [first.name, second.name, third.name]);
  assert.deepEqual(await Promise.all(form.getAll('image[]').map(file => file.arrayBuffer().then(bytes => [...new Uint8Array(bytes)]))), [[11, 22, 33], [44, 55], [66]]);
  assert.equal(form.has('image'), false, 'Do not mix field names');

  await client.imageEdit('model', 'legacy', imageDataUrl(71));
  form = calls.at(-1).request.body;
  assert.deepEqual([...form.keys()].filter(k => k.startsWith('image')), ['image']);
  assert.equal(form.get('image').type, 'image/png');
  await assert.rejects(client.imageEdit('model', 'empty', []), /at least one/);
  await assert.rejects(client.imageEdit('model', 'invalid', [new Blob(['hello'], { type: 'text/plain' })]), /Invalid image/);
  // Reusing an image ID must count every multipart copy, not just stored bytes.
  // Report a large size without allocating 192 MiB in the regression test.
  const largeImage = new Blob([Uint8Array.of(1)], { type: 'image/png' });
  Object.defineProperty(largeImage, 'size', { value: 64 * 1024 * 1024 });
  const callCount = calls.length;
  await assert.rejects(
    client.imageEdit('model', 'duplicate reference', [largeImage, largeImage, largeImage]),
    /128 MiB per-request limit/,
  );
  assert.equal(calls.length, callCount, 'Oversized edits must not reach the server');
  assert.ok(calls.every(call => call.url === '/api/v1/images/edits' && call.request.method === 'POST'));
}
function imageDataUrlFromBytes(s) { return `data:image/png;base64,${Buffer.from(s).toString('base64')}`; }

function testIntegrationGuards() {
  const read = name => fs.readFileSync(path.join(root, name), 'utf8');
  const chat = read('src/components/ChatView.tsx');
  const omni = read('src/tools/omniTools.ts');
  const mcp = read('src/tools/mcpRuntime.ts');
  const persistence = read('src/features/chatHistory/toolCallPersistence.ts');
  assert.match(chat, /imageRegistry\.addUploads\(/);
  assert.match(chat, /pendingImages\.map\(image => image\.original\)/);
  assert.match(chat, /imageIds: undefined,/);
  assert.match(chat, /imageMessageId: undefined,/);
  assert.match(chat, /if \(hasImages && !supportsChatImageInput\) \{/);
  assert.match(chat, /requestImages = undefined;[\s\S]*?if \(!runtimeHasImageTools\)/);
  assert.match(chat, /m\.images\?\.length && supportsChatImageInput && !omniRuntime/);
  assert.match(omni, /imageRegistry\.selectForEdit/);
  assert.match(omni, /acceptsImageReferences/);
  assert.match(omni, /Image URLs are not accepted from the planner/);
  assert.match(omni, /Vision model returned no analysis/);
  assert.match(omni, /imageRegistry\.getAnalysisInput/);
  assert.match(mcp, /imageRegistry\.selectForEdit/);
  assert.match(mcp, /imageRegistry\.getAnalysisInput/);
  assert.match(mcp, /name: 'analyze_image'/);
  assert.match(mcp, /acceptsImageReferences/);
  assert.match(mcp, /Image data URLs are not accepted from the planner/);
  assert.match(mcp, /Vision model returned no analysis/);
  assert.match(persistence, /artifacts: _artifacts/);
  assert.match(read('src/hooks/useChatStreaming.ts'), /acceptsImageReferences\?: boolean/);
  assert.match(read('src/hooks/useChatStreaming.ts'), /runtime!\.execute\(tc as ToolCall, controller\.signal\)/);
}

(async () => {
  await testRegistry();
  await testImageEditTransport();
  testIntegrationGuards();
  console.log('Conversation image references: registry, transport, and integration guards passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
