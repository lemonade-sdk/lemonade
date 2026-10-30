const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');
const React = require('react');
const { renderToStaticMarkup } = require('react-dom/server');

const filename = path.resolve(__dirname, '../src/components/MonitorGpuVitals.tsx');
const compiled = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
  compilerOptions: {
    target: ts.ScriptTarget.ES2020,
    module: ts.ModuleKind.CommonJS,
    jsx: ts.JsxEmit.ReactJSX,
    esModuleInterop: true,
  },
  fileName: filename,
  reportDiagnostics: true,
});
assert.deepEqual(compiled.diagnostics.filter(d => d.category === ts.DiagnosticCategory.Error), []);
const exported = {};
Function('exports', 'require', compiled.outputText)(exported, require);
const { MonitorGpuVitals } = exported;

const mkGpu = (index, fields = {}) => ({
  id: `nvidia:0000:0${index}:00.0`,
  name: `Test GPU ${index}`,
  vendor: 'nvidia',
  memory_type: 'dedicated',
  gpu_percent: 75,
  memory_used_gb: 8,
  memory_total_gb: 16,
  ...fields,
});
const gpus = [mkGpu(0), mkGpu(1, {
  name: 'AMD shared-memory GPU', memory_type: 'shared', gpu_percent: null,
  memory_used_gb: null, memory_total_gb: null,
})];
const render = (devices, selected = devices[0]?.id) => renderToStaticMarkup(
  React.createElement(MonitorGpuVitals, { gpus: devices, selectedId: selected, onSelect: () => {} }),
);
const html = render(gpus);
assert.equal((html.match(/class="dash2-gpus__device"/g) || []).length, 2);
assert.match(html, /aria-pressed="true"/);
assert.match(html, /aria-pressed="false"/);
assert.match(html, /8\.0\/16 GB/);
assert.match(html, /75%/);
assert.match(html, />MEM</);
assert.match(html, /GPU 1, AMD shared-memory GPU\. Utilization —\. MEM —/);
assert.doesNotMatch(html, /width:"-?\d+/);
assert.match(render(Array.from({ length: 5 }, (_, i) => mkGpu(i))), /\+1 GPUs/);
assert.equal((render(Array.from({ length: 5 }, (_, i) => mkGpu(i))).match(/class="dash2-gpus__device"/g) || []).length, 4);

const monitor = fs.readFileSync(path.resolve(__dirname, '../src/components/Monitor.tsx'), 'utf8');
assert.match(monitor, /const multiGpu = gpuDevices\.length >= 2/);
assert.match(monitor, /!multiGpu && hasGpu && <RingGauge label="GPU"/);
assert.match(monitor, /!multiGpu && !memoryTopology\.unified && hasGpu/);
assert.match(monitor, /dash2-card__h">\{t\('System Vitals'\)\}/);
assert.match(monitor, /MonitorGpuVitals gpus=\{gpuDevices\}/);
assert.match(monitor, /key: `gpu:\$\{activeGpu\.id\}`/);

const hook = fs.readFileSync(path.resolve(__dirname, '../src/hooks/useMonitorData.ts'), 'utf8');
assert.match(hook, /gpuById: aTarget\.gpuById/);
assert.match(hook, /Object\.entries\(h\.gpuById\)/);
const css = fs.readFileSync(path.resolve(__dirname, '../src/styles/styles.css'), 'utf8');
assert.match(css, /dash2-card--vitals-multi \{ container-type: inline-size; \}/);
assert.match(css, /@container \(min-width: 560px\)/);
assert.match(css, /@container \(min-width: 960px\)/);
assert.match(css, /@container \(max-width: 420px\)/);
console.log('Monitor multi-GPU regression checks passed.');
