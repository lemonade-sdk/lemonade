const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const filename = path.resolve(__dirname, '../src/components/ModelDetailPanel.tsx');
const source = ts.createSourceFile(filename, fs.readFileSync(filename, 'utf8'),
  ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
let initializer;
function find(node) {
  if (ts.isVariableDeclaration(node) && node.name.getText(source) === 'loadWithShownConfiguration') {
    initializer = node.initializer;
  }
  ts.forEachChild(node, find);
}
find(source);
assert.ok(initializer);
const code = ts.transpileModule(`return ${initializer.getText(source)};`, {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
}).outputText;
const ref = { current: null };
const load = Function('useCallback', 'configLoadOptionsRef', code)(callback => callback, ref);
const model = { id: 'user.Test' };
const requests = [];
const start = (...args) => requests.push(args);
load(start, model);
assert.deepEqual(requests.at(-1), [model, {}],
  'Load before the options fetch completes must still select the panel policy instead of local overrides');
ref.current = () => ({ ctx_size: 8192 });
load(start, model);
assert.deepEqual(requests.at(-1), [model, { ctx_size: 8192 }],
  'Load after options arrive must use the current configuration');
ref.current = () => ({ llamacpp_args: null, merge_args: false });
load(start, model);
assert.deepEqual(requests.at(-1), [model, { llamacpp_args: null, merge_args: false }],
  'an intentional clear after options arrive must retain its protected fields');
ref.current = null;
load(start, model);
assert.deepEqual(requests.at(-1), [model, {}],
  'switching models must not replay stale options while its configuration is loading');
console.log('Panel load readiness behavior checks passed.');
