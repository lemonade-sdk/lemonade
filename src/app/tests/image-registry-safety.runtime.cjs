const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = process.env.LEMONADE_TEST_ROOT || path.resolve(__dirname, '..');
const filename = path.join(root, 'src/features/images/imageRegistry.ts');
const compiled = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
  fileName: filename,
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  reportDiagnostics: true,
});
assert.equal((compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error).length, 0);
class ImageReader {
  readAsDataURL(blob) {
    blob.arrayBuffer().then(bytes => {
      this.result = `data:${blob.type};base64,${Buffer.from(bytes).toString('base64')}`;
      this.onload();
    }).catch(error => { this.error = error; this.onerror(); });
  }
}
const moduleStub = { exports: {} };
new Function('module', 'exports', 'fetch', 'FileReader', 'createImageBitmap', 'document', compiled.outputText)(
  moduleStub, moduleStub.exports, (...args) => global.fetch(...args), ImageReader, undefined, undefined,
);
const { ConversationImageRegistry } = moduleStub.exports;
const file = n => new Blob([Uint8Array.of(n, n + 1)], { type: 'image/png' });
const dataUrl = n => `data:image/png;base64,${Buffer.from([n, n + 1, n + 2]).toString('base64')}`;

async function main() {
  const registry = new ConversationImageRegistry();
  const [original] = registry.addUploads('branch', [file(1)], 'turn-a');
  const [outputA] = await registry.addOutputs('branch', [dataUrl(10)], 'generated', [], 'prompt A', undefined, 'turn-a');
  const [laterUpload] = registry.addUploads('branch', [file(2)], 'turn-b');
  const [outputB] = await registry.addOutputs('branch', [dataUrl(20)], 'edited', [outputA], 'prompt B', undefined, 'turn-b');
  registry.pruneForRestart('branch', ['turn-a', 'turn-b'], 'turn-b');
  assert.deepEqual(registry.resolve('branch', [original, outputA, laterUpload]).map(image => image.id),
    [original, outputA, laterUpload]);
  assert.throws(() => registry.resolve('branch', [outputB]), /unavailable/);
  assert.equal(registry.listForPlanner('branch', 'turn-b').includes(outputB), false);
  assert.deepEqual(registry.defaultsForMessage('branch', 'turn-b'), [laterUpload]);

  registry.pruneForRestart('branch', ['turn-a'], 'turn-a');
  assert.deepEqual(registry.resolve('branch', [original]).map(image => image.id), [original]);
  for (const removed of [outputA, laterUpload, outputB]) {
    assert.throws(() => registry.resolve('branch', [removed]), /unavailable/);
  }
  assert.equal(registry.selectForAnalysis('branch', undefined, 'turn-a'), original);

  const cap = 64 * 1024 * 1024;
  const nearLimit = new ConversationImageRegistry();
  nearLimit.addUploads('limit', [
    ...Array.from({ length: 7 }, () => ({ type: 'image/png', size: cap })),
    { type: 'image/png', size: cap - 2 },
  ], 'heavy');
  const beforeFailedOutput = nearLimit.defaultsForMessage('limit', 'other');
  const registration = await nearLimit.addOutputsBestEffort('limit', [dataUrl(90)], 'generated', [], 'test', undefined, 'heavy');
  assert.equal(registration.ids, undefined);
  assert.match(registration.warning, /memory limit/);
  assert.deepEqual(nearLimit.defaultsForMessage('limit', 'other'), beforeFailedOutput, 'Failed registration adds no partial image');

  const signalRegistry = new ConversationImageRegistry();
  const controller = new AbortController();
  const oldFetch = global.fetch;
  let seenSignal;
  let finishFetch;
  try {
    global.fetch = (_url, options) => {
      seenSignal = options.signal;
      return new Promise(resolve => { finishFetch = resolve; });
    };
    const pending = signalRegistry.addOutputs('abort', [dataUrl(5)], 'generated', [], 'test', controller.signal, 'turn');
    assert.equal(seenSignal, controller.signal, 'Fetch must receive the conversation cancellation signal');
    controller.abort();
    finishFetch(new Response(file(7)));
    await assert.rejects(pending, /cancelled/);
    assert.deepEqual(signalRegistry.defaultsForMessage('abort'), []);
  } finally {
    global.fetch = oldFetch;
  }

  const batch = new ConversationImageRegistry();
  const ids = await batch.addOutputs('seven', Array.from({ length: 7 }, (_, n) => dataUrl(30 + n)),
    'generated', [], 'seven', undefined, 'turn-7');
  assert.equal(ids.length, 7, 'No arbitrary four-image limit may be reintroduced');
  assert.deepEqual(batch.resolve('seven', ids).map(image => image.id), ids);

  // Exercise the browser-only resizing path as well as the no-DOM fallback.
  let bitmapClosed = false;
  let outputType = '';
  const canvas = {
    width: 0, height: 0,
    getContext: () => ({ drawImage() {} }),
    toBlob(callback, type) {
      outputType = type;
      callback(new Blob([Uint8Array.of(42)], { type }));
    },
  };
  const browserModule = { exports: {} };
  new Function('module', 'exports', 'fetch', 'FileReader', 'createImageBitmap', 'document', compiled.outputText)(
    browserModule, browserModule.exports, (...args) => global.fetch(...args), ImageReader,
    async () => ({ width: 2400, height: 1200, close() { bitmapClosed = true; } }),
    { createElement: () => canvas },
  );
  const browserRegistry = new browserModule.exports.ConversationImageRegistry();
  const originalPixels = file(64);
  const originalBytes = new Uint8Array(await originalPixels.arrayBuffer());
  const [browserId] = browserRegistry.addUploads('browser', [originalPixels], 'image-message');
  const resized = await browserRegistry.getAnalysisInput('browser', browserId);
  assert.equal(canvas.width, 1024);
  assert.equal(canvas.height, 512);
  assert.equal(outputType, 'image/png', 'Non-JPEG input must not be converted to JPEG');
  assert.ok(resized.startsWith('data:image/png;base64,'));
  assert.equal(bitmapClosed, true);
  assert.deepEqual(new Uint8Array(await browserRegistry.resolve('browser', [browserId])[0].original.arrayBuffer()),
    originalBytes, 'Analysis resizing must preserve the editing source');
  // Invalid MIME-looking bytes must not reach the standalone image editor.
  let closedDecodes = 0;
  const validationModule = { exports: {} };
  new Function('module', 'exports', 'fetch', 'FileReader', 'createImageBitmap', 'document', compiled.outputText)(
    validationModule, validationModule.exports, (...args) => global.fetch(...args), ImageReader,
    async blob => {
      if (blob.size !== 2) throw new Error('Not an image');
      return { width: 10, height: 10, close() { closedDecodes++; } };
    }, undefined,
  );
  const validating = new validationModule.exports.ConversationImageRegistry();
  await validating.validateUploadBytes([file(60)]);
  assert.equal(closedDecodes, 1, 'Decoded image resources must be released');
  await assert.rejects(validating.validateUploadBytes([
    new Blob([Uint8Array.of(1, 2, 3, 4, 5)], { type: 'image/png' }),
  ]), /could not be decoded/, 'A valid MIME header is not proof of image contents');

  // The browser-free or failed-decoder path must never turn a 64 MiB image
  // into an unbounded base64 request or retain it in the analysis cache.
  const fallback = new ConversationImageRegistry();
  const oversized = new Blob([new Uint8Array(9 * 1024 * 1024)], { type: 'image/png' });
  const [oversizedId] = fallback.addUploads('oversized', [oversized], 'turn');
  await assert.rejects(fallback.getAnalysisInput('oversized', oversizedId), /too large for analysis/);
  assert.equal(fallback.resolve('oversized', [oversizedId])[0].original, oversized,
    'An oversized analysis fallback must not affect the original editing source');
  assert.equal(fallback.resolve('oversized', [oversizedId])[0].analysisInput, undefined,
    'Failed conversions must not remain cached');

  const [accountedId] = fallback.addUploads('accounted', [file(81)], 'msg');
  const accountedState = fallback.conversations.get('accounted');
  await fallback.getAnalysisInput('accounted', accountedId);
  assert.ok(accountedState.analysisBytes > 0, 'Cache must count the derived data URL in the budget');
  fallback.pruneForRestart('accounted', []);
  assert.equal(accountedState.analysisBytes, 0, 'Branch pruning must release analysis cache accounting');
  const chat = fs.readFileSync(path.join(root, 'src/components/ChatView.tsx'), 'utf8');
  const effectStart = chat.indexOf('    const active = new Set([', chat.indexOf('const imagePreviewUrlsRef'));
  const effectEnd = chat.indexOf('\n  }, [conversations, pendingImages]);', effectStart);
  assert.ok(effectStart > 0 && effectEnd > effectStart);
  const releasePreviews = new Function('pendingImages', 'conversations', 'imagePreviewUrlsRef',
    'generatedMediaUrlsRef', 'URL', chat.slice(effectStart, effectEnd));
  const previewUrl = 'blob:edit-preview';
  const previews = { current: new Set([previewUrl]) };
  const tracked = { current: new Set([previewUrl]) };
  const revoked = [];
  const urlMock = { revokeObjectURL(url) { revoked.push(url); } };
  releasePreviews([{ preview: previewUrl }], [], previews, tracked, urlMock);
  releasePreviews([], [{ messages: [{ images: [previewUrl] }] }], previews, tracked, urlMock);
  assert.deepEqual(revoked, [], 'Unsent and sent images must remain viewable');
  releasePreviews([], [], previews, tracked, urlMock);
  assert.deepEqual(revoked, [previewUrl], 'Discarded previews must be revoked');
  assert.equal(tracked.current.has(previewUrl), false, 'Revoked previews must leave the global URL set');
  assert.match(chat, /hasImageTools && !toolsDisabledByBrowserContext/);
  console.log('Image registry safety: branch pruning, atomic limits, cancellation, multi-image refs, browser resizing, upload validation, bounded analysis cache and preview lifecycle PASS');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
