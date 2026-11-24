const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const panelPath = path.resolve(__dirname, '../src/components/ModelDetailPanel.tsx');
const source = ts.createSourceFile(panelPath, fs.readFileSync(panelPath, 'utf8'),
  ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
let initializer;
function findBuilder(node) {
  if (ts.isVariableDeclaration(node) && node.name.getText(source) === 'buildLoadOptions') {
    initializer = node.initializer;
  }
  ts.forEachChild(node, findBuilder);
}
findBuilder(source);
assert.ok(initializer, 'the model panel must expose its current load payload builder');
const compiled = ts.transpileModule(`const buildLoadOptions = ${initializer.getText(source)};
return buildLoadOptions();`, {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
}).outputText;
const execute = Function('buildConfigOptions', 'recipeKeys', 'argsKeys', 'argsKeyNames',
  'recipeDraft', 'savedLoadSettings', 'supportsContextSize', 'sendsArgs', compiled);

function payload({ baseline = '', draft = baseline, configured = {},
  keys = ['llamacpp_backend', 'llamacpp_args'], args = ['llamacpp_args'],
  supportsContextSize = true } = {}) {
  const values = typeof draft === 'string' ? { llamacpp_args: draft } : draft;
  const initial = typeof baseline === 'string' ? { llamacpp_args: baseline } : baseline;
  return execute(() => configured, keys, args, new Set(args), values,
    { recipe: initial }, supportsContextSize, args.length > 0);
}

assert.deepEqual(payload({ configured: { llamacpp_backend: 'metal' } }),
  { llamacpp_backend: 'metal', ctx_size: -1 },
  'an unchanged empty argument field must not become an admin-only override');
for (const baseline of ['--threads 2', '--threads 2 --temp 0.5']) {
  assert.deepEqual(payload({ baseline, configured: { llamacpp_backend: 'metal', llamacpp_args: baseline } }),
    { llamacpp_backend: 'metal', ctx_size: -1 },
    'unchanged server-effective arguments must stay on the server, whether saved or defaulted');
}
assert.deepEqual(payload({ baseline: '--threads 2', configured: { ctx_size: 2048, llamacpp_args: '--threads 2' } }),
  { llamacpp_backend: null, ctx_size: 2048 },
  'changing context size must not replay unchanged backend arguments');
assert.deepEqual(payload({ baseline: '--threads 2', draft: '--threads 4', configured: { llamacpp_args: '--threads 4' } }),
  { llamacpp_backend: null, llamacpp_args: '--threads 4', ctx_size: -1, merge_args: false },
  'changed arguments must remain explicit so the server can require admin authorization');
assert.deepEqual(payload({ baseline: '--threads 2', draft: '' }),
  { llamacpp_backend: null, llamacpp_args: null, ctx_size: -1, merge_args: false },
  'clearing arguments must remain an explicit protected tombstone');
assert.deepEqual(payload({
  keys: ['llamacpp_args', 'whispercpp_args'], args: ['llamacpp_args', 'whispercpp_args'],
  baseline: { llamacpp_args: '--threads 2', whispercpp_args: '--threads 1' },
  draft: { llamacpp_args: '--threads 2', whispercpp_args: '--threads 3' },
  configured: { llamacpp_args: '--threads 2', whispercpp_args: '--threads 3' },
  supportsContextSize: false,
}), { whispercpp_args: '--threads 3', merge_args: false },
'only changed argument fields belong in a multi-argument recipe payload');
assert.deepEqual(payload({ keys: ['steps'], args: [], configured: { steps: 5 }, supportsContextSize: false }),
  { steps: 5 }, 'recipes without arguments must not acquire merge_args');
console.log('Model load payload behavior checks passed.');
