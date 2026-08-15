/* The contract that makes the renderer backend-agnostic: a recipe lemond
 * publishes but no client table has ever heard of must still parse into a
 * complete descriptor. The catalog falls back; it never filters. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const webpack = require('webpack');

async function bundle() {
  const outputPath = fs.mkdtempSync(path.join(os.tmpdir(), 'lemonade-backend-catalog-'));
  const config = {
    mode: 'development',
    target: 'node',
    entry: path.resolve(__dirname, 'fixtures/backend-catalog-entry.ts'),
    output: { path: outputPath, filename: 'backendCatalog.cjs', library: { type: 'commonjs2' } },
    resolve: { extensions: ['.ts', '.tsx', '.js'] },
    module: { rules: [{ test: /\.tsx?$/, use: { loader: 'ts-loader', options: { transpileOnly: true, compilerOptions: { rootDir: '.' } } }, exclude: /node_modules/ }] },
    optimization: { minimize: false },
  };
  await new Promise((resolve, reject) => {
    webpack(config, (error, stats) => {
      if (error) return reject(error);
      if (stats?.hasErrors()) return reject(new Error(stats.toString({ all: false, errors: true })));
      resolve();
    });
  });
  return outputPath;
}

/* A payload shaped exactly like /v1/system-info, carrying two recipes the
 * renderer has never heard of alongside the real irregular cases. */
const PAYLOAD = {
  devices: { cpu: { name: 'Test CPU', available: true } },
  recipes: {
    'quantum-llm': {
      order: 3,
      display_name: 'Quantum LLM GPU',
      web_display_name: 'Quantum LLM',
      modality: 'Text generation',
      experimental: true,
      selectable_backend: true,
      uses_ctx_size: true,
      slot_policy: 'standard',
      default_backend: 'qpu',
      backends: { qpu: { state: 'installable', version: '', message: '', action: '' } },
      support: [{ backend: 'qpu', os: ['linux'], devices: [{ device: 'accelerator', families: [] }], device_summary: 'Quantum accelerators' }],
      options: [
        { name: 'quantum-llm_backend', cli_flag: '--quantum', default: '', type_name: 'BACKEND', help: 'Backend to use', group: 'Quantum Options' },
        { name: 'quantum-llm_args', cli_flag: '', default: '', type_name: 'ARGS', help: 'Extra args', group: 'Quantum Options' },
      ],
    },
    holo: {
      order: 4,
      display_name: 'Holo',
      modality: 'Holographic generation',
      backends: { cpu: { state: 'installed', version: '1.0', message: '', action: '', devices: ['cpu'] } },
      support: [],
      options: [],
    },
    'sd-cpp': {
      order: 1,
      display_name: 'StableDiffusion.cpp',
      web_display_name: 'stable-diffusion.cpp',
      modality: 'Image generation',
      selectable_backend: true,
      backends: { vulkan: { state: 'installed', version: 'b1', message: '', action: '' } },
      support: [{ backend: 'vulkan', os: ['linux'], devices: [{ device: 'amd_gpu', families: [] }, { device: 'nvidia_gpu', families: [] }], device_summary: '' }],
      options: [
        { name: 'sd-cpp_backend', cli_flag: '--sd-cpp', default: '', type_name: 'BACKEND', help: '', group: 'Stable Diffusion Options' },
        { name: 'sdcpp_args', cli_flag: '--sdcpp-args', default: '', type_name: 'ARGS', help: '', group: 'Stable Diffusion Options' },
      ],
    },
    moonshine: {
      order: 2,
      display_name: 'Moonshine',
      modality: 'Speech-to-text',
      selectable_backend: false,
      backends: { cpu: { state: 'installed', version: '0.2', message: '', action: '', devices: ['cpu'] } },
      support: [{ backend: 'cpu', os: ['linux'], devices: [{ device: 'cpu', families: [] }], device_summary: '' }],
      options: [{ name: 'moonshine_args', cli_flag: '--moonshine-args', default: '', type_name: 'ARGS', help: '', group: 'Moonshine Options' }],
    },
    llamacpp: {
      order: 5,
      display_name: 'Llama.cpp GPU',
      web_display_name: 'llama.cpp GPU',
      modality: 'Text generation',
      selectable_backend: true,
      uses_ctx_size: true,
      backends: { vulkan: { state: 'installed', version: 'b1', message: '', action: '', devices: ['amd_gpu'] } },
      support: [],
      options: [{ name: 'llamacpp_backend', cli_flag: '--llamacpp', default: '', type_name: 'BACKEND', help: '', group: 'Llama.cpp Backend Options' }],
    },
    kokoro: {
      order: 0,
      display_name: 'Kokoro',
      modality: 'Text-to-speech',
      backends: { cpu: { state: 'installed', version: '1', message: '', action: '', devices: ['cpu'] } },
      support: [],
      options: [],
    },
  },
};

function makeApi() {
  const listeners = new Set();
  return {
    systemInfoData: null,
    systemInfo: null,
    onSystemInfoChange(fn) { listeners.add(fn); return () => listeners.delete(fn); },
    deliver(data) { this.systemInfoData = data; listeners.forEach(fn => fn()); },
  };
}

async function main() {
  const outputPath = await bundle();
  const store = require(path.join(outputPath, 'backendCatalog.cjs'));
  const { parseBackendCatalog, backendLabel, backendCompactLabel } = store;
  const { orderSections, sectionForModality, sectionMeta, OTHER_RUNTIMES } = store;

  /* ── An unknown recipe parses completely ──────────────────────── */

  const catalog = parseBackendCatalog(PAYLOAD);
  const quantum = catalog.byRecipe.get('quantum-llm');

  assert.ok(quantum, 'an unrecognized recipe must not be dropped');
  assert.equal(quantum.webDisplayName, 'Quantum LLM');
  assert.equal(quantum.modality, 'Text generation');
  assert.equal(quantum.experimental, true);
  assert.equal(quantum.usesCtxSize, true);
  assert.equal(quantum.backendOptionName, 'quantum-llm_backend',
    'the backend option is read from options[], never concatenated from the recipe name');
  assert.equal(quantum.argsOptionName, 'quantum-llm_args');
  assert.deepEqual(quantum.devices, ['accelerator'],
    'a backend with no detected devices falls back to its declared support row');

  const holo = catalog.byRecipe.get('holo');
  assert.ok(holo, 'a recipe with an unknown modality must not be dropped');
  assert.equal(holo.modality, 'Holographic generation');
  assert.equal(holo.webDisplayName, 'Holo', 'web_display_name falls back to display_name');

  /* ── The real irregular cases ─────────────────────────────────── */

  const sdcpp = catalog.byRecipe.get('sd-cpp');
  assert.equal(sdcpp.backendOptionName, 'sd-cpp_backend');
  assert.equal(sdcpp.argsOptionName, 'sdcpp_args',
    'sd-cpp pairs an irregular backend option with a differently spelled args option');
  assert.deepEqual(sdcpp.devices, ['amd_gpu', 'nvidia_gpu'],
    'one backend variant may occupy more than one device row');

  assert.equal(catalog.byRecipe.get('moonshine').backendOptionName, null,
    'moonshine declares no BACKEND option and must not get a backend selector');
  assert.equal(catalog.byRecipe.get('moonshine').argsOptionName, 'moonshine_args');

  const kokoro = catalog.byRecipe.get('kokoro');
  assert.equal(kokoro.backendOptionName, null);
  assert.equal(kokoro.argsOptionName, null);
  assert.deepEqual(kokoro.options, [], 'a recipe with no options still parses');

  /* ── Server order, not client order ───────────────────────────── */

  assert.deepEqual(
    catalog.recipes.map(entry => entry.recipe),
    ['kokoro', 'sd-cpp', 'moonshine', 'quantum-llm', 'holo', 'llamacpp'],
    'recipes render in the descriptor registry order the server publishes',
  );

  for (const descriptor of catalog.recipes) {
    assert.ok(descriptor.recipe, 'every descriptor keeps its recipe id');
    assert.ok(Array.isArray(descriptor.options), 'every descriptor exposes an options array');
  }

  assert.equal(catalog.raw, PAYLOAD, 'the untouched payload stays reachable');

  /* ── Sections key on modality, a closed set, not on recipe names ─ */

  assert.equal(sectionForModality('Text generation'), 'Text generation');
  assert.equal(sectionForModality(''), OTHER_RUNTIMES, 'a recipe with no modality still gets a section');
  assert.equal(sectionMeta('Text generation').title, 'Language models');
  assert.equal(sectionMeta('Text classification').title, 'Classification',
    'onnxruntime gets its own section instead of being filed under language models');
  assert.equal(sectionMeta('Holographic generation').title, 'Holographic generation',
    'an unrecognized modality is titled from its own name, never dropped');

  assert.deepEqual(
    orderSections(['3D generation', 'Holographic generation', 'Text generation', OTHER_RUNTIMES, 'Speech-to-text']),
    ['Text generation', 'Speech-to-text', '3D generation', 'Holographic generation', OTHER_RUNTIMES],
    'documented modalities lead, newcomers follow, the catch-all is last',
  );

  /* ── Labels: the server names it, we override only where wrong ── */

  store.resetBackendCatalogForTests();
  const labelApi = makeApi();
  store.attachBackendCatalog(labelApi);
  labelApi.deliver(PAYLOAD);

  assert.equal(backendLabel('quantum-llm'), 'Quantum LLM',
    'a recipe no client table mentions is named by the server');
  assert.equal(backendCompactLabel('quantum-llm'), 'Quantum LLM');
  assert.equal(backendLabel('moonshine'), 'Moonshine',
    'no override needed where the published name is already right');
  assert.equal(backendLabel('llamacpp'), 'llama.cpp',
    'the override wins over the docs-facing name that bakes in the device');
  assert.equal(backendLabel('sd-cpp'), 'stable-diffusion.cpp');
  assert.equal(backendCompactLabel('sd-cpp'), 'SD.cpp',
    'the chip shortens where the published name is too long');
  assert.equal(backendLabel('never-heard-of-it'), 'never-heard-of-it',
    'an unknown recipe falls back to its id, never to empty or undefined');

  /* ── A stale failure never replaces a fresher catalog ──────────── */

  store.resetBackendCatalogForTests();
  const api = makeApi();
  let failSlowRefresh;
  api.systemInfo = () => new Promise((_, reject) => { failSlowRefresh = reject; });
  store.attachBackendCatalog(api);

  const slowRefresh = store.refreshBackendCatalog();
  assert.equal(store.getBackendCatalogSnapshot().status, 'loading');

  api.deliver(PAYLOAD);
  assert.equal(store.getBackendCatalogSnapshot().status, 'ready');

  failSlowRefresh(new Error('network went away'));
  await slowRefresh;

  const settled = store.getBackendCatalogSnapshot();
  assert.equal(settled.status, 'ready', 'a superseded failure must not clobber a newer payload');
  assert.equal(settled.error, null);
  assert.ok(settled.catalog.byRecipe.has('quantum-llm'));

  /* ── A failure with nothing newer does surface ─────────────────── */

  store.resetBackendCatalogForTests();
  const failing = makeApi();
  failing.systemInfo = () => Promise.reject(new Error('server unreachable'));
  store.attachBackendCatalog(failing);
  await store.refreshBackendCatalog();

  const errored = store.getBackendCatalogSnapshot();
  assert.equal(errored.status, 'error');
  assert.equal(errored.error, 'server unreachable');

  fs.rmSync(outputPath, { recursive: true, force: true });
  console.log('Backend catalog contract checks passed.');
}

main().catch(error => {
  console.error(error);
  process.exit(1);
});
