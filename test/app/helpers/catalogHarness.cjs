const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const app = path.resolve(__dirname, '../../../src/app');
const renderer = path.join(app, 'src/renderer');
const ts = require(path.join(app, 'node_modules/typescript'));
for (const extension of ['.ts', '.tsx']) require.extensions[extension] = (module, filename) => {
  module._compile(ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
    fileName: filename,
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020, jsx: ts.JsxEmit.ReactJSX, esModuleInterop: true },
  }).outputText, filename);
};
const load = name => require(path.join(renderer, name));
const stub = (name, exports) => { require.cache[path.join(renderer, name)] = { exports }; };
const fixture = name => JSON.parse(fs.readFileSync(path.join(__dirname, '../fixtures/backend-catalog', name + '.json'), 'utf8'));
const cases = [];
const test = (name, run) => cases.push({ name, run });
const tick = () => new Promise(resolve => setImmediate(resolve));
async function run() {
  let failed = 0;
  for (const item of cases) {
    try { await item.run(); console.log('PASS ' + item.name); }
    catch (error) { failed++; console.error('FAIL ' + item.name, error); }
  }
  console.log(`${cases.length - failed}/${cases.length} tests passed; ${failed} failed`);
  if (failed) process.exitCode = 1;
}
function bridge() {
  const callbacks = {};
  const window = new EventTarget();
  window.location = { origin: 'http://browser.test' };
  window.api = {
    getServerBaseUrl: async () => 'http://one.test/proxy/',
    getServerAPIKey: async () => 'private-initial-key',
    onConnectionSettingsUpdated: fn => { callbacks.connection = fn; },
  };
  global.window = window;
  global.CustomEvent = class extends Event { constructor(name, options) { super(name); this.detail = options?.detail; } };
  stub('tauriShim.ts', { tauriReady: Promise.resolve() });
  return callbacks;
}
module.exports = { assert, fs, path, app, renderer, load, stub, fixture, test, run, bridge, tick };
