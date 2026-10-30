const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');
const React = require('react');
const { renderToStaticMarkup } = require('react-dom/server');

const src = name => path.resolve(__dirname, '../src', name);
function loadTsx(file) {
  const result = ts.transpileModule(fs.readFileSync(file, 'utf8'), {
    fileName: file,
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS,
      jsx: ts.JsxEmit.ReactJSX, esModuleInterop: true },
    reportDiagnostics: true,
  });
  assert.deepEqual((result.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error), []);
  const exported = {};
  Function('exports', 'require', result.outputText)(exported, require);
  return exported;
}
const { translate } = loadTsx(src('i18n.tsx'));
const { MonitorGpuVitals } = loadTsx(src('components/MonitorGpuVitals.tsx'));
const gpu = (index, fields = {}) => ({
  id: `nvidia:${index}`, vendor: 'nvidia', name: `Test GPU ${index}`,
  memory_type: 'dedicated', gpu_percent: 75, memory_used_gb: 8, memory_total_gb: 16,
  ...fields,
});
const render = (gpus, locale = 'en-US', selectedId = gpus[0]?.id) => renderToStaticMarkup(
  React.createElement(MonitorGpuVitals, {
    gpus, selectedId, onSelect: () => {}, t: (key, values) => translate(key, locale, values),
  }),
);
const devices = [gpu(0), gpu(1, { memory_type: 'shared', gpu_percent: null, memory_used_gb: null })];
const html = render(devices);
assert.equal((html.match(/class="dash2-gpus__device"/g) || []).length, 2);
assert.match(html, /aria-pressed="true"/);
assert.match(html, /aria-pressed="false"/);
assert.match(html, /8\.0\/16 GB/);
assert.match(html, />MEM</);
assert.match(html, /GPU 1, Test GPU 1\. Utilization —\. MEM —/);
assert.match(render([gpu(0, { memory_type: 'unknown' }), gpu(1)]), />MEM</);
assert.match(render([gpu(0, { gpu_percent: 150, memory_used_gb: 20 })]), /width:100%/);
const zh = render(devices, 'zh-CN', devices[1].id);
assert.match(zh, /选择 GPU 历史曲线/);
assert.match(zh, /利用率/);
assert.match(zh, /内存/);
const five = render(Array.from({ length: 5 }, (_, i) => gpu(i)));
assert.match(five, /\+1 GPUs/);
assert.equal((five.match(/class="dash2-gpus__device"/g) || []).length, 4);

const monitor = fs.readFileSync(src('components/Monitor.tsx'), 'utf8');
assert.match(monitor, /const multiGpu = gpuDevices\.length >= 2/);
assert.match(monitor, /!multiGpu && hasGpu && <RingGauge label="GPU"/);
assert.match(monitor, /!multiGpu && !memoryTopology\.unified && hasGpu/);
assert.match(monitor, /MonitorGpuVitals gpus=\{gpuDevices\}.* t=\{t\}/);
assert.match(monitor, /key: `gpu:\$\{activeGpu\.id\}`/);
assert.match(fs.readFileSync(src('components/MonitorView.tsx'), 'utf8'), /<Monitor key=\{api\.baseUrl\}/);
const css = fs.readFileSync(src('styles/styles.css'), 'utf8');
assert.match(css, /grid-template-columns: repeat\(auto-fit, minmax\(min\(480px, 100%\), 1fr\)\)/);
assert.match(css, /@container \(max-width: 540px\)/);
assert.match(css, /@container \(max-width: 420px\)/);
console.log('Monitor multi-GPU regression checks passed.');
