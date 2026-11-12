const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const overrideSource = path.join(root, 'src/features/models/engineOverrides.ts');
const originals = require.extensions['.ts'];
require.extensions['.ts'] = function loadTs(module, filename) {
  const code = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS, esModuleInterop: true },
    fileName: filename,
  }).outputText;
  module._compile(code, filename);
};

try {
  const { collapseIdenticalBuiltinOverrides, inferenceEnginesForModel } = require(overrideSource);
  const builtin = {
    id: 'builtin.Coder', canonical_model_name: 'builtin.Coder',
    recipe: 'llamacpp', checkpoint: 'org/coder:Q4_K_M',
    checkpoints: { main: 'org/coder:Q4_K_M', mmproj: 'org/projector:fp16' },
    registry_source: 'huggingface', labels: ['chat'],
  };
  const user = {
    ...builtin, id: 'Coder', canonical_model_name: 'user.Coder',
    recipe: 'gufo', custom: true,
  };
  const equal = collapseIdenticalBuiltinOverrides([builtin, user]);
  assert.deepEqual(equal.map(model => model.canonical_model_name), ['user.Coder']);
  assert.equal(collapseIdenticalBuiltinOverrides([builtin, { ...user, checkpoint: 'org/coder:Q8_0', checkpoints: { ...user.checkpoints, main: 'org/coder:Q8_0' } }]).length, 2);
  assert.equal(collapseIdenticalBuiltinOverrides([builtin, { ...user, checkpoints: { main: 'org/coder:Q4_K_M' } }]).length, 2);
  assert.equal(collapseIdenticalBuiltinOverrides([builtin, { ...user, registry_source: 'modelscope' }]).length, 2);

  const systemInfo = { recipes: {
    llamacpp: { display_name: 'llama.cpp', supported_modes: ['chat', 'embeddings'], backends: { cuda: { state: 'installed' } } },
    gufo: { display_name: 'Gufo', modality: 'Text generation', backends: { rocm: { state: 'installable' } } },
    ds4: { display_name: 'DS4', supported_modes: ['chat'], backends: { rocm: { state: 'unsupported' } } },
    thenoise: { display_name: 'TheNoise', supported_modes: ['image'], backends: { cuda: { state: 'installed' } } },
    sdcpp: { display_name: 'SD.cpp', modality: 'Image generation', backends: { cuda: { state: 'installed' } } },
  } };
  assert.deepEqual(inferenceEnginesForModel(user, systemInfo).map(x => x.recipe), ['gufo', 'llamacpp']);
  const image = { ...user, labels: ['image'], recipe: 'thenoise' };
  assert.deepEqual(inferenceEnginesForModel(image, systemInfo).map(x => x.recipe), ['sdcpp', 'thenoise']);

  const css = fs.readFileSync(path.join(root, 'src/styles/styles.css'), 'utf8');
  const panel = fs.readFileSync(path.join(root, 'src/components/ModelDetailPanel.tsx'), 'utf8');
  const manager = fs.readFileSync(path.join(root, 'src/components/ModelManager.tsx'), 'utf8');
  const route = fs.readFileSync(path.join(root, '../cpp/server/routes/lemonade/models/unregister.cpp'), 'utf8');
  assert.match(css, /\.detail-configuration__engine-row\s*\{[^}]*display:\s*grid;[^}]*grid-template-columns:\s*minmax\(0, 1fr\) max-content;[^}]*align-items:\s*end;/s);
  assert.match(css, /\.detail-configuration__engine-actions\s*\{[^}]*align-items:\s*center;[^}]*min-height:\s*36px;/s);
  assert.match(css, /@container model-configuration \(max-width: 520px\)\s*\{\s*\.detail-configuration__engine-row\s*\{\s*grid-template-columns:\s*minmax\(0, 1fr\)/s);
  assert.match(panel, /onChangeInferenceEngine\?: \(model: ModelInfo, recipe: string\)/);
  assert.match(manager, /collapseIdenticalBuiltinOverrides\(visibleServerModels\)/);
  // Delete is not Restore Built-in. The ordinary Delete path is the same
  // for built-in models, converted built-ins, and standalone user models.
  // The original Omni/Router collection flows remain separate.
  const deleteBlock = manager.split('const handleDelete = async (model: ModelInfo) => {')[1]
    ?.split('const handlePull = async (model: ModelInfo) => {')[0];
  assert.ok(deleteBlock, 'model delete handler must exist');
  assert.match(deleteBlock, /if \(modelIsCustom\(model\) && isCollectionModel\(model\)\)/);
  assert.match(deleteBlock, /Delete Omni collection/);
  assert.doesNotMatch(deleteBlock, /Delete custom override|Delete custom model/);
  assert.match(deleteBlock, /const standaloneCustom = modelIsCustom\(model\)/);
  assert.match(deleteBlock, /custom registration and managed model files/);
  assert.match(deleteBlock, /if \(!confirm\(confirmation\)\) return;/);
  assert.equal((deleteBlock.match(/await api\.deleteModel\(name\);/g) || []).length, 2,
    'the collection path and standard model path must both delete through the API');
  assert.match(route, /unregister_user_model\(id\)/);
  assert.doesNotMatch(route, /delete_model\(/);
  console.log('Model engine override checks passed');
} finally {
  if (originals) require.extensions['.ts'] = originals;
  else delete require.extensions['.ts'];
}
