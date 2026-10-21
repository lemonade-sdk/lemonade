const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require('typescript');

const filename = path.resolve(__dirname, '../src/components/DownloadManager.tsx');
const source = fs.readFileSync(filename, 'utf8');
const parsed = ts.createSourceFile(filename, source, ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
const fn = parsed.statements.find(s => ts.isFunctionDeclaration(s) && s.name?.text === 'startDownloadAgain');
assert.ok(fn, 'the shared model/backend restart action must exist');
assert.match(source, /const handleResume = [\s\S]*?startDownloadAgain\(download, 'Resume'\)/);
assert.match(source, /const handleRetry = [\s\S]*?removeDownload\(download\);\s*startDownloadAgain\(download, 'Retry'\)/);
const compiled = ts.transpileModule(`${fn.getText(parsed)}\nmodule.exports = startDownloadAgain;`, {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  reportDiagnostics: true,
});
assert.deepEqual((compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error), []);
const calls = [];
const wakeCalls = [];
const callbacks = [];
const ctx = {
  module: { exports: {} },
  api: {
    pullModel: async (name, cb) => { calls.push(['model', name]); callbacks.push(cb); },
    installBackend: async (recipe, backend, cb) => { calls.push(['backend', recipe, backend]); callbacks.push(cb); },
  },
  downloadStore: { wake: (name, kind) => wakeCalls.push([name, kind]) },
  friendlyErrorMessage: err => err.message,
  console,
};
vm.runInNewContext(compiled.outputText, ctx);
const restart = ctx.module.exports;

restart({ modelName: 'llamacpp:rocm', downloadType: 'backend' }, 'Resume');
restart({ modelName: 'sdcpp:cuda', downloadType: 'backend' }, 'Retry');
restart({ modelName: 'Qwen3', downloadType: 'model' }, 'Retry');
assert.deepEqual(calls, [
  ['backend', 'llamacpp', 'rocm'],
  ['backend', 'sdcpp', 'cuda'],
  ['model', 'Qwen3'],
]);
assert.deepEqual(wakeCalls, [
  ['llamacpp:rocm', 'backend'], ['sdcpp:cuda', 'backend'], ['Qwen3', 'model'],
]);
callbacks[0].onProgress({ percent: 50 });
callbacks[0].onComplete();
assert.equal(wakeCalls.length, 5, 'backend progress and completion must refresh the server snapshot');
console.log('Backend download restart regression checks passed.');
