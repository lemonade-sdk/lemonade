const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require('typescript');

function compile(code) {
  return ts.transpileModule(code, {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  }).outputText;
}

// ── Device list helpers from ModelDetailPanel.tsx ───────────────────────────

const panelSource = fs.readFileSync(path.resolve(__dirname, '../src/components/ModelDetailPanel.tsx'), 'utf8')
  .replace(/\r\n/g, '\n');
const helperNames = ['activeRecipeForModel', 'optionalDisplayValue', 'systemRecipes', 'backendMapForRecipe',
  'recipeDefaultBackend', 'availableDeviceCounts', 'indexed', 'deviceBackendFor', 'runtimeDevicesFor',
  'hipIndexedAmdGpus', 'shortGpuId', 'gpuChoices', 'gpuChoiceFor', 'matchingDeviceOption',
  'deviceOptionsForKey', 'deviceOptionLabel'];
let helperCode = 'type SystemInfoLike = any; type RecipeOptions = any; type ModelInfo = any;\n';
for (const pattern of [/^const SINGLE_GPU_RECIPES.*$/m, /^interface RuntimeDevice.*$/m, /^const isGpuId = .*$/m]) {
  const match = panelSource.match(pattern);
  assert.ok(match, `${pattern} missing`);
  helperCode += `${match[0]}\n`;
}
const interfaceMatch = panelSource.match(/^interface GpuChoice \{[\s\S]*?\n\}\n/m);
assert.ok(interfaceMatch, 'GpuChoice missing');
helperCode += interfaceMatch[0];
for (const name of helperNames) {
  const match = panelSource.match(new RegExp(String.raw`^function ${name}\b[\s\S]*?\n}\n`, 'm'));
  assert.ok(match, `${name} missing`);
  helperCode += match[0];
}
helperCode += 'module.exports = { deviceOptionsForKey, deviceOptionLabel, matchingDeviceOption };';
const helperModule = { exports: {} };
vm.runInNewContext(compile(helperCode), { module: helperModule, exports: helperModule.exports });
const { deviceOptionsForKey, deviceOptionLabel, matchingDeviceOption } = helperModule.exports;
// Values built in the vm context have its prototypes; compare copies made here.
const plain = value => JSON.parse(JSON.stringify(value));

const p100 = 'Tesla P100-PCIE-16GB';
const info = {
  devices: {
    nvidia_gpu: [
      { available: true, index: 0, name: 'GeForce RTX 2060', uuid: 'GPU-aaaa0000-0000-0000-0000-000000000000' },
      { available: true, index: 1, name: p100, uuid: 'GPU-bbbb1111-0000-0000-0000-000000000000' },
      { available: true, index: 2, name: p100, uuid: 'GPU-cccc2222-0000-0000-0000-000000000000' },
    ],
    amd_gpu: [
      { available: true, integrated: true, name: 'Radeon 8060S', hip_index: 1, pci_bus_id: '0000:c3:00.0' },
      { available: true, integrated: false, name: 'RX 7900 XTX', hip_index: 0, pci_bus_id: '0000:03:00.0' },
    ],
  },
  recipes: { acestep: { default_backend: 'cuda', backends: { cuda: { state: 'installed' }, rocm: { state: 'installed' } } } },
};
const acestep = { recipe: 'acestep' };
const labels = (options, data = info) => options.map(option => deviceOptionLabel(option, data, null));

// A GPU is offered under its stable id, so a saved choice survives cards moving.
const cuda = deviceOptionsForKey('acestep_device', undefined, 'cuda', acestep, info);
assert.deepEqual(plain(cuda), ['auto', 'GPU-aaaa0000-0000-0000-0000-000000000000',
  'GPU-bbbb1111-0000-0000-0000-000000000000', 'GPU-cccc2222-0000-0000-0000-000000000000']);
// Cards of one model are told apart by a short id; a unique name needs none.
assert.deepEqual(plain(labels(cuda)), ['auto', 'CUDA0 (GeForce RTX 2060)',
  `CUDA1 (${p100} · GPU-bbbb1111)`, `CUDA2 (${p100} · GPU-cccc2222)`]);

// The /system-info payload already has the same names and UUIDs as /system-stats.
// Exercise the real two-NVIDIA-GPU shape without adding another API data source.
const twoNvidiaInfo = {
  ...info,
  devices: {
    ...info.devices,
    nvidia_gpu: [
      { index: 0, uuid: 'GPU-11111111-2222-3333-4444-555555555555', name: 'NVIDIA GeForce RTX 5090' },
      { index: 1, uuid: 'GPU-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee', name: 'NVIDIA GeForce RTX 4090' },
    ],
  },
};
const expectedCudaIds = ['auto', twoNvidiaInfo.devices.nvidia_gpu[0].uuid,
  twoNvidiaInfo.devices.nvidia_gpu[1].uuid];
const expectedCudaLabels = ['auto', 'CUDA0 (NVIDIA GeForce RTX 5090)',
  'CUDA1 (NVIDIA GeForce RTX 4090)'];
for (const recipe of ['thenoise', 'sd-cpp', 'acestep']) {
  const key = `${recipe}_device`;
  const options = deviceOptionsForKey(key, undefined, 'cuda', { recipe }, twoNvidiaInfo);
  assert.deepEqual(plain(options), expectedCudaIds, `${recipe}: stable GPU IDs`);
  assert.deepEqual(plain(labels(options, twoNvidiaInfo)), expectedCudaLabels,
    `${recipe}: names should come directly from system-info`);
  assert.equal(matchingDeviceOption('cuda1', options, twoNvidiaInfo), expectedCudaIds[2]);
}
assert.equal(deviceOptionLabel('cuda0', twoNvidiaInfo), expectedCudaLabels[1]);
assert.equal(deviceOptionLabel('CUDA1', twoNvidiaInfo), expectedCudaLabels[2]);
// Also check that the rendered options use the label helper, not the raw value.
assert.match(panelSource,
  /options\.map\(opt => <option key=\{opt\} value=\{opt\}>\{deviceOptionLabel\(opt, info, runtimeDevices,/);

// ROCm is numbered by HIP, not by the iGPU-first amd_gpu order, and saved by PCI address.
const rocm = deviceOptionsForKey('acestep_device', undefined, 'rocm', acestep, info);
assert.deepEqual(plain(rocm), ['auto', '0000:03:00.0', '0000:c3:00.0']);
assert.deepEqual(plain(labels(rocm)), ['auto', 'ROCm0 (RX 7900 XTX)', 'ROCm1 (Radeon 8060S)']);

// A value saved by the CLI (CUDA2) or an older app (cuda2) is the same option, not a second one.
for (const saved of ['CUDA2', 'cuda2', 'gpu-CCCC2222-0000-0000-0000-000000000000']) {
  const options = deviceOptionsForKey('acestep_device', saved, 'cuda', acestep, info);
  assert.equal(options.length, cuda.length, `${saved} must not add an entry`);
  assert.equal(matchingDeviceOption(saved, options, info), 'GPU-cccc2222-0000-0000-0000-000000000000');
}

// A GPU saved against another server, or a card that is gone, stays visible and is marked.
for (const saved of ['GPU-ffff9999-0000-0000-0000-000000000000', 'cuda7']) {
  const options = deviceOptionsForKey('acestep_device', saved, 'cuda', acestep, info);
  assert.equal(options[0], saved);
  assert.equal(deviceOptionLabel(saved, info, null, 'not on this server'), `${saved} (not on this server)`);
}

// Without HIP data, several AMD GPUs are not guessed at.
const noHip = JSON.parse(JSON.stringify(info));
for (const gpu of noHip.devices.amd_gpu) delete gpu.hip_index;
assert.deepEqual(plain(deviceOptionsForKey('acestep_device', undefined, 'rocm', acestep, noHip)), ['auto']);

// ── Per-server system-info cache from api.ts ────────────────────────────────

const apiFile = path.resolve(__dirname, '../src/api.ts');
const apiSource = fs.readFileSync(apiFile, 'utf8');
const parsed = ts.createSourceFile(apiFile, apiSource, ts.ScriptTarget.Latest, true);
const apiClass = parsed.statements.find(s => ts.isClassDeclaration(s) && s.name?.text === 'LemonadeAPI');
assert.ok(apiClass);
const member = (name, kind) => {
  const found = apiClass.members.find(m => m.name?.getText(parsed) === name && (!kind || m.kind === kind));
  assert.ok(found, `${name} missing`);
  return found.getText(parsed);
};
const apiProgram = `class TestAPI {
  _hostBaseUrl = 'http://a:13305';
  _systemInfoData = null;
  _systemInfoFetchedAt = 0;
  _systemInfoListeners = new Set();
  _systemInfoInFlight = null;
  _systemInfoGeneration = 0;
  _systemInfoServer = null;
  _serverChangeListeners = new Set();
  get baseUrl() { return this._hostBaseUrl; }
  ${member('baseUrl', ts.SyntaxKind.SetAccessor)}
  ${member('systemInfoData', ts.SyntaxKind.GetAccessor)}
  ${member('onServerChange')}
  ${member('_noteServerChange')}
  ${member('systemInfo')}
}
module.exports = TestAPI;`;
const pending = [];
const apiContext = {
  module: { exports: {} },
  normalizeBaseUrl: url => url,
  safeSetLocalStorage: () => true,
  LS_BASE_URL: 'base_url',
};
vm.runInNewContext(compile(apiProgram), apiContext);
const TestAPI = apiContext.module.exports;

(async () => {
  const api = new TestAPI();
  api._json = () => new Promise(resolve => pending.push(resolve));
  let changes = 0;
  api.onServerChange(() => { changes += 1; });

  const first = api.systemInfo();
  pending.shift()({ machine: 'A' });
  await first;
  assert.deepEqual(plain(api.systemInfoData), { machine: 'A' });

  // Same server: no change is announced and the data stays.
  api.baseUrl = 'http://a:13305';
  assert.equal(changes, 0);
  assert.deepEqual(plain(api.systemInfoData), { machine: 'A' });

  // A request still in flight for A must not be stored once the app talks to B.
  const late = api.systemInfo();
  api.baseUrl = 'http://b:13305';
  assert.equal(changes, 1);
  assert.equal(api.systemInfoData, null, 'machine A data must not be served for machine B');
  pending.shift()({ machine: 'A' });
  await late;
  assert.equal(api.systemInfoData, null);

  const fresh = api.systemInfo();
  pending.shift()({ machine: 'B' });
  await fresh;
  assert.deepEqual(plain(api.systemInfoData), { machine: 'B' });

  console.log('GPU device selection checks passed.');
})().catch(error => {
  console.error(error);
  process.exit(1);
});
