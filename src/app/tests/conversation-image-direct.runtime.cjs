const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = process.env.LEMONADE_TEST_ROOT || path.resolve(__dirname, '..');
function source(file) { return fs.readFileSync(path.join(root, file), 'utf8'); }
function loadTypeScript(file, inject = {}) {
  const output = ts.transpileModule(source(file), {
    fileName: file,
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
    reportDiagnostics: true,
  });
  const diagnostics = (output.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error);
  assert.deepEqual(diagnostics, []);
  const module = { exports: {} };
  new Function('module', 'exports', ...Object.keys(inject), output.outputText)(
    module, module.exports, ...Object.values(inject),
  );
  return module.exports;
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
const { ConversationImageRegistry } = loadTypeScript('src/features/images/imageRegistry.ts', {
  FileReader: ImageReader,
  createImageBitmap: undefined,
  document: undefined,
});
const { prepareDirectImageAnalysis, directImageFollowupContent } = loadTypeScript('src/features/images/directImageAnalysis.ts');
const img = byte => new File([Uint8Array.of(byte, byte + 1, byte + 2)], `photo-${byte}.png`, { type: 'image/png' });
const dataUrl = byte => `data:image/png;base64,${Buffer.from([byte, byte + 1, byte + 2]).toString('base64')}`;

async function main() {
  const registry = new ConversationImageRegistry();
  const [first, second] = registry.addUploads('chat-a', [img(1), img(10)], 'upload-1');
  registry.addUploads('chat-b', [img(20)], 'upload-other');
  const context = { imageRegistry: registry, conversationId: 'chat-a', messageId: 'upload-1', directInputImageIds: [first, second] };

  // VLM already received a user attachment: neither a new image part nor
  // a duplicate analysis call is required.
  const previousReads = fileReads;
  const already = await prepareDirectImageAnalysis('tc1', 'What color is it?', second, context);
  assert.equal(JSON.parse(already.content).status, 'already_visible');
  assert.deepEqual(already.attachImageIds, undefined);
  assert.equal(fileReads, previousReads);
  assert.equal(await directImageFollowupContent('answer now', [already]), 'answer now');

  // Result from image generation is not in the chat's original prompt. Return
  // metadata and inject the full multimodal input only into the next request.
  const [generated, generated2] = await registry.addOutputs('chat-a', [dataUrl(30), dataUrl(35)], 'generated');
  const newOutput = await prepareDirectImageAnalysis('tc2', 'What changed?', generated2, context);
  assert.deepEqual(newOutput.attachImageIds, [generated2]);
  assert.ok(!newOutput.content.includes('base64,'), 'Tool result text must not contain image bytes');
  assert.equal(JSON.parse(newOutput.content).question, 'What changed?');
  const messages = await directImageFollowupContent('continue', [newOutput], id => registry.getAnalysisInput('chat-a', id));
  assert.equal(messages.length, 3);
  assert.equal(messages[0].type, 'text');
  assert.match(messages[1].text, new RegExp(generated2));
  assert.equal(messages[2].type, 'image_url');
  assert.equal(messages[2].image_url.url, dataUrl(35));
  assert.ok(!JSON.stringify(newOutput).includes('base64,'));

  const defaultId = registry.selectForAnalysis('chat-a', undefined, 'upload-1');
  assert.equal(defaultId, generated2, 'Latest output in tool round takes precedence over original upload');
  assert.equal(registry.selectForAnalysis('chat-a', first), first, 'Explicit ID must win');
  assert.throws(() => registry.selectForAnalysis('chat-b', generated2), /unavailable/);
  assert.throws(() => registry.selectForAnalysis('chat-a', 'img_gdeadbeefcafe_9876'), /unavailable/);
  assert.throws(() => registry.selectForAnalysis('chat-a', 30), /available image ID/);

  const single = await prepareDirectImageAnalysis('tc3', 'First source?', generated, context);
  let resolutions = [];
  const joined = await directImageFollowupContent('next', [newOutput, newOutput, single], async id => {
    resolutions.push(id);
    return registry.getAnalysisInput('chat-a', id);
  });
  assert.deepEqual(resolutions, [generated2, generated], 'Deduplicate and preserve tool-result order');
  assert.deepEqual(joined.filter(x => x.type === 'image_url').map(x => x.image_url.url), [dataUrl(35), dataUrl(30)]);
  assert.deepEqual(joined.filter(x => x.type === 'text').slice(1).map(x => x.text),
    [`Image 1 (${generated2}):`, `Image 2 (${generated}):`]);
  assert.equal(await directImageFollowupContent('plain', [{ ...newOutput, error: true }]), 'plain', 'Failed calls must not inject images');
  await assert.rejects(directImageFollowupContent('plain', [single]), /resolver is unavailable/);
  // Regression: seven freshly generated outputs, then seven analyze_image calls
  // in one tool round. No arbitrary four-image cutoff or silent truncation.
  const sevenImages = Array.from({ length: 7 }, (_, index) => dataUrl(50 + index));
  const sevenIds = await registry.addOutputs('chat-a', sevenImages, 'generated', [], 'seven comparison images');
  const sevenCalls = await Promise.all(sevenIds.map((id, index) =>
    prepareDirectImageAnalysis(`analysis-${index + 1}`, `Inspect image ${index + 1}`, id, context)));
  assert.ok(sevenCalls.every(call => call.attachImageIds?.length === 1));
  let sevenResolutions = [];
  const sevenParts = await directImageFollowupContent('Compare all seven test images', sevenCalls, async id => {
    sevenResolutions.push(id);
    return registry.getAnalysisInput('chat-a', id);
  });
  assert.equal(sevenParts.length, 15, 'Guidance plus seven ID labels and seven actual images');
  assert.match(sevenParts[0].text, /Compare all seven test images/);
  assert.deepEqual(sevenResolutions, sevenIds);
  for (let index = 0; index < 7; index++) {
    assert.deepEqual(sevenParts[1 + index * 2], { type: 'text', text: `Image ${index + 1} (${sevenIds[index]}):` });
    assert.deepEqual(sevenParts[2 + index * 2], {
      type: 'image_url', image_url: { url: sevenImages[index] },
    });
  }
  assert.ok(sevenCalls.every(call => !call.content.includes('base64,')), 'Tool results never contain image bytes');

  // Cancellation during a large round must prevent subsequent resolution work.
  const midBatchController = new AbortController();
  let started = 0;
  await assert.rejects(directImageFollowupContent('cancelled', sevenCalls, async () => {
    if (++started === 4) midBatchController.abort();
    return dataUrl(90);
  }, midBatchController.signal), /cancelled/);
  assert.equal(started, 4, 'Do not start a second batch after cancellation');
  const controller = new AbortController();
  controller.abort();
  await assert.rejects(prepareDirectImageAnalysis('tc4', 'Cancelled?', generated, context, controller.signal), /cancelled/);
  await assert.rejects(directImageFollowupContent('cancelled', [single], async () => dataUrl(1), controller.signal), /cancelled/);

  registry.clearConversation('chat-a');
  await assert.rejects(prepareDirectImageAnalysis('tc5', 'Gone?', generated, context), /unavailable/);

  // Execute the actual built-in MCP analyze_image case in isolation. The
  // Direct analysis must bypass resolveModel, ensureLoaded and chatCompletionOnce.
  const mcpSource = source('src/tools/mcpRuntime.ts');
  const start = mcpSource.indexOf("      case 'analyze_image': {");
  const end = mcpSource.indexOf('\n      }\n', start);
  assert.ok(start >= 0 && end > start);
  const wrapper = `async function executeCase(args, context, call, signal) {
    switch ('analyze_image') { ${mcpSource.slice(start, end + '\n      }'.length)} }
  }\nexports.executeCase = executeCase;`;
  const compiled = ts.transpileModule(wrapper, {
    fileName: 'mcp-analyze.ts',
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
    reportDiagnostics: true,
  });
  assert.equal((compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error).length, 0);
  const caseModule = { exports: {} };
  const invoked = { resolve: 0, ensure: 0, infer: 0 };
  let nextAnalysis = 'delegated image analysis';
  const api = {
    loadedModels: [],
    chatCompletionOnce: async (model, input) => {
      invoked.infer++;
      assert.ok(input[0].content[0].image_url.url.startsWith('data:image/'));
      assert.equal(input[0].content[1].text, 'What is wrong?');
      return nextAnalysis;
    },
  };
  const resolveModel = async (_capability, wanted) => {
    invoked.resolve++;
    return { name: wanted || 'FallbackVLM' };
  };
  const ensureLoaded = async () => { invoked.ensure++; };
  const stringArg = (args, key) => typeof args[key] === 'string' ? args[key].trim() : '';
  new Function('module', 'exports', 'api', 'resolveModel', 'ensureLoaded', 'stringArg', 'modelSupportsChatImageInput', 'prepareDirectImageAnalysis', 'latestImage', 'generated', compiled.outputText)(
    caseModule, caseModule.exports, api, resolveModel, ensureLoaded, stringArg, () => true, prepareDirectImageAnalysis, () => { throw new Error('unexpected legacy lookup'); }, [],
  );
  const invoke = caseModule.exports.executeCase;
  const modelRegistry = new ConversationImageRegistry();
  const [modelImage] = modelRegistry.addUploads('model-chat', [img(47)], 'model-message');
  const modelContext = { imageRegistry: modelRegistry, conversationId: 'model-chat', messageId: 'model-message', chatVisionModel: 'CurrentVLM' };
  const modelArgs = { image_id: modelImage, question: 'What is wrong?' };
  const skipped = await invoke(modelArgs, modelContext, { id: 'mcp1' });
  assert.deepEqual(skipped.attachImageIds, [modelImage]);
  assert.deepEqual(invoked, { resolve: 0, ensure: 0, infer: 0 }, 'A VLM must not call itself');

  const alreadyInput = await invoke(modelArgs, { ...modelContext, directInputImageIds: [modelImage] }, { id: 'mcp2' });
  assert.equal(JSON.parse(alreadyInput.content).status, 'already_visible');
  assert.equal(alreadyInput.attachImageIds, undefined);
  assert.deepEqual(invoked, { resolve: 0, ensure: 0, infer: 0 });

  const same = await invoke({ ...modelArgs, model: 'currentvlm' }, modelContext, { id: 'mcp3' });
  assert.deepEqual(same.attachImageIds, [modelImage]);
  assert.deepEqual(invoked, { resolve: 0, ensure: 0, infer: 0 });

  const delegated = await invoke(modelArgs, { ...modelContext, chatVisionModel: undefined }, { id: 'mcp4' });
  assert.equal(delegated.content, 'delegated image analysis');
  assert.equal(delegated.attachImageIds, undefined);
  assert.deepEqual(invoked, { resolve: 1, ensure: 1, infer: 1 }, 'Text planner delegates once');

  const separate = await invoke({ ...modelArgs, model: 'DifferentVLM' }, modelContext, { id: 'mcp5' });
  assert.equal(separate.content, 'delegated image analysis');
  assert.deepEqual(invoked, { resolve: 2, ensure: 2, infer: 2 }, 'Explicit different VLM delegates');

  nextAnalysis = '';
  await assert.rejects(invoke(modelArgs, { ...modelContext, chatVisionModel: undefined }, { id: 'mcp6' }), /no analysis/);

  // Integration contracts: only a proven VLM uses the fast path. Explicit
  // delegation to a different model and text-only chat keep the old path.
  const mcp = source('src/tools/mcpRuntime.ts');
  const omni = source('src/tools/omniTools.ts');
  const chat = source('src/components/ChatView.tsx');
  const streaming = source('src/hooks/useChatStreaming.ts');
  assert.match(chat, /getPrimaryChatComponent\(collectionInfo, knownModelInfos\)[\s\S]*?modelSupportsChatImageInput\(chatTargetInfo, chatTargetLoaded\)/);
  assert.match(chat, /directInputImageIds: requestImages\?\.length \? userMessage\.imageIds : \[\]/);
  assert.match(mcp, /context\.chatVisionModel && \(!explicitModel \|\| explicitModel\.toLowerCase\(\) === context\.chatVisionModel\.toLowerCase\(\)\)/);
  assert.match(mcp, /prepareDirectImageAnalysis\(call\.id, question, args\.image_id, context, signal\)[\s\S]*?const model = await resolveModel\('chat', explicitModel/);
  assert.match(omni, /context\.chatVisionModel\?\.toLowerCase\(\) === model\.toLowerCase\(\)/);
  assert.match(omni, /prepareDirectImageAnalysis\(call\.id, String\(args\.question \|\| 'Describe this image\.'\), args\.image_id, context, signal\)/);
  assert.match(streaming, /directImageFollowupContent\(\s*guidance, results, runtime\?\.resolveImageAttachment, controller\.signal/);
  assert.match(streaming, /\{ role: 'user' as const, content: followupContent \}/);
  assert.match(mcp, /resolveImageAttachment: active\.find\(runtime => runtime\.resolveImageAttachment\)\?\.resolveImageAttachment/);
  assert.doesNotMatch(source('src/features/images/directImageAnalysis.ts'), /chatCompletionOnce|localStorage|sessionStorage|indexedDB/i);
  console.log('Conversation direct image analysis: seven-image inspection, stable IDs, no-self-call, cancellation, isolation and integration PASS');
}
main().catch(err => { console.error(err); process.exitCode = 1; });
