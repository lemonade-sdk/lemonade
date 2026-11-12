const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..', 'src');
function load(relative, deps = {}) {
  const file = path.join(root, relative);
  const compiled = ts.transpileModule(fs.readFileSync(file, 'utf8'), {
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
    fileName: file, reportDiagnostics: true,
  });
  assert.equal(compiled.diagnostics.length, 0);
  const module = { exports: {} };
  Function('exports', 'require', 'module', compiled.outputText)(module.exports, name => {
    assert.ok(Object.hasOwn(deps, name), `unexpected dependency: ${name}`);
    return deps[name];
  }, module);
  return module.exports;
}
const capabilities = load('modelCapabilities.ts');
const {
  engineVariantKey, engineVariantGroups, collapseEngineVariantRows,
  engineVariantRowSelected, canAddEngineVariant, variantNameTaken,
} = load('features/models/engineVariants.ts', { '../../modelCapabilities': capabilities });

const a = { id: 'Qwen-Q4', recipe: 'llamacpp', checkpoint: 'org/Qwen-GGUF:Q4_K_M.gguf',
  source: 'huggingface', registry_source: 'huggingface', labels: ['chat'], downloaded: true };
const b = { ...a, id: 'user.Qwen-Q4-vllm', recipe: 'vllm', labels: ['chat', 'custom'] };
const sameEngine = { ...a, id: 'user.tuned' };
const q8 = { ...b, id: 'Qwen-Q8', checkpoint: 'org/Qwen-GGUF:Q8_0.gguf' };
const otherRegistry = { ...b, id: 'MS', source: 'modelscope', registry_source: 'modelscope' };
const otherMode = { ...b, id: 'embedding', labels: ['embeddings'] };
const onDemand = { ...b, id: 'ondemand', labels: ['chat', 'on-demand'] };
const collection = { ...b, id: 'router', recipe: 'collection.router' };
const unknownSource = { ...b, id: 'cloud', source: 'cloud' };
const extra = { ...b, id: 'extra', source: 'extra_models_dir' };
const upload = { ...b, id: 'upload', source: 'local_upload' };
const differentCompanion = { ...b, id: 'projector', checkpoints: { main: a.checkpoint, mmproj: 'repo:proj.gguf' } };
const unfamiliarRole = { ...b, checkpoints: { main: a.checkpoint, adapter: 'repo:adapter.gguf' } };
const all = [a, b, sameEngine, q8, otherRegistry, otherMode, onDemand, collection, unknownSource, extra, upload, differentCompanion];
const groups = engineVariantGroups(all);
assert.deepEqual(groups.get(a.id), [a, b, sameEngine]);
assert.equal(groups.get(b.id), groups.get(a.id));
for (const model of all.slice(3)) assert.equal(groups.has(model.id), false, model.id);
assert.equal(engineVariantGroups([a, sameEngine]).size, 0);
assert.equal(engineVariantKey(a), engineVariantKey({ ...b, checkpoints: { main: a.checkpoint } }));
assert.equal(engineVariantKey({ ...a, mmproj: 'repo:proj.gguf' }), engineVariantKey(differentCompanion));
assert.notEqual(engineVariantKey(a), engineVariantKey(differentCompanion));
assert.equal(engineVariantKey({ ...a, checkpoints: { main: 'different' } }), null);
assert.equal(engineVariantKey({ ...a, source: 'modelscope' }), null);
assert.equal(engineVariantKey({ ...a, source: '', registry_source: '' }), null);
assert.equal(canAddEngineVariant(a), true);
assert.equal(canAddEngineVariant(unfamiliarRole), false);
assert.equal(canAddEngineVariant(onDemand), false);
assert.equal(variantNameTaken('user.QWEN-Q4', all), true);
assert.equal(variantNameTaken('user.Qwen-new', all), false);

const row = (model, status = 'downloaded', pinned = false) => ({ model, status, pinned });
const ids = rows => rows.map(r => r.model.id);
assert.deepEqual(ids(collapseEngineVariantRows([row(a), row(b), row(q8)], groups, null)), [a.id, q8.id]);
assert.deepEqual(ids(collapseEngineVariantRows([row(a), row(b), row(q8)], groups, b.id)), [b.id, q8.id]);
assert.deepEqual(ids(collapseEngineVariantRows([row(a), row(b, 'running')], groups, a.id)), [b.id]);
assert.deepEqual(ids(collapseEngineVariantRows([row(a, 'running', true), row(b, 'running')], groups, b.id)), [a.id]);
assert.deepEqual(ids(collapseEngineVariantRows([row(a), row(b, 'downloading')], groups, a.id)), [b.id]);
// Filter has removed a: never resurrect a nonmatching representative.
assert.deepEqual(ids(collapseEngineVariantRows([row(b)], groups, a.id)), [b.id]);
assert.equal(engineVariantRowSelected(a, b.id, groups), true);
assert.equal(engineVariantRowSelected(a, q8.id, groups), false);
assert.equal(engineVariantRowSelected(a, null, groups), false);
console.log('Engine variant identity, grouping, filters, activity and collision tests passed.');
