const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '../src');
const modules = new Map();
function load(relative) {
  const filename = path.resolve(root, relative);
  if (modules.has(filename)) return modules.get(filename).exports;
  const module = { exports: {} };
  modules.set(filename, module);
  const compiled = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
    fileName: filename,
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  });
  const localRequire = name => name.startsWith('.')
    ? load(path.relative(root, path.resolve(path.dirname(filename), `${name}.ts`)))
    : require(name);
  new Function('module', 'exports', 'require', compiled.outputText)(module, module.exports, localRequire);
  return module.exports;
}

const configuration = load('modelConfiguration.ts');
const metadata = load('features/backends/recipeMetadata.ts');
const sampler = load('samplerArgs.ts');
const { isStrataGguf } = load('features/models/huggingFaceSearch.ts');
const { findRegisteredRemoteModel } = load('features/models/registeredRemoteModel.ts');
const presentation = load('modelPresentation.ts');

const info = { recipes: { strata: {
  modality: 'Text generation',
  selectable_backend: true,
  uses_ctx_size: true,
  options: [
    { name: 'strata_backend', type_name: 'BACKEND' },
    { name: 'strata_parallel', type_name: 'SIZE', default: 1 },
    { name: 'strata_args', type_name: 'ARGS', default: '' },
  ],
} } };
assert.deepEqual(metadata.recipeConfigurableOptionNames(info, 'strata'),
  ['strata_backend', 'strata_parallel', 'strata_args']);
assert.equal(configuration.backendArgsFieldForRecipe('strata', info), 'strata_args');
assert.equal(presentation.backendLabel('strata'), 'Strata');

const options = { ctx_size: 8192, strata_backend: 'rocm', strata_parallel: 4, strata_args: '--mtp 0 --no-warmup' };
const model = { name: 'Qwen3.6', recipe: 'strata', labels: ['chat'], recipe_options: options };
assert.deepEqual(configuration.modelDefaultRecipeOptions(model), options);
assert.deepEqual(configuration.recipeOptionsForCapability(options, 'chat'), options);
assert.deepEqual(configuration.sanitizeRecipeOptions({ strata_parallel: '8', strata_args: '--mtp 0' }),
  { strata_parallel: 8, strata_args: '--mtp 0' });
assert.deepEqual(configuration.sanitizeRecipeOptions({ strata_args: '' }), {});
assert.deepEqual(configuration.recipeOptionsForCapability(options, 'image'), {});

const specs = sampler.samplerFieldsForRecipe('strata');
assert.equal(specs.find(spec => spec.flag === '--mtp').backendDefault, '2');
const args = '--mtp 0 --prefill auto --gpu 1 --expert-vram 8 --expert-slots 256 --cpu-threads 4 '
  + '--hybrid-rows 128 --draft-vocab 1000 --no-graph --no-warmup --batch-mtp';
const split = sampler.splitSamplerArgs(specs, args);
assert.equal(split.rest, '');
assert.equal(split.fields['--mtp'], '0');
assert.equal(split.fields['--no-warmup'], 'true');
assert.deepEqual(sampler.buildArgsMap(sampler.parseCustomArgs(sampler.composeSamplerArgs(specs, split.fields, split.rest))),
  sampler.buildArgsMap(sampler.parseCustomArgs(args)));
assert.equal(sampler.composeSamplerArgs(specs, { '--mtp': '0', '--no-graph': '' }, ''), '--mtp 0');
const malformed = sampler.splitSamplerArgs(specs, '--no-graph false --no-warmup --no-warmup');
assert.equal(malformed.rest, '--no-graph false --no-warmup --no-warmup');
assert.ok(malformed.claimedByRest.has('--no-graph'));

for (const quant of ['UD-Q4_K_XL', 'UD-Q6_K_XL', 'Q8_0']) {
  assert.ok(isStrataGguf(`Qwen3.6-35B-A3B-${quant}.gguf`));
}
for (const file of ['Qwen3.5-35B-A3B-Q8_0.gguf', 'Qwen3.6-35B-A3B-Q2_K.gguf',
  'qwen3.6-35b-a3b-q8_0.gguf',
  'NotQwen3.6-35B-A3B-Q8_0.gguf', 'Qwen3.6-35B-A3B-Q8_0-00001-of-00002.gguf']) {
  assert.ok(!isStrataGguf(file));
}
const file = 'Qwen3.6-35B-A3B-UD-Q4_K_XL.gguf';
const details = { variants: [{ name: 'UD-Q4_K_XL', primary_file: file }], mmproj_files: [] };
const manager = ts.createSourceFile('ModelManager.tsx',
  fs.readFileSync(path.join(root, 'components/ModelManager.tsx'), 'utf8'), ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
const functionNames = ['remoteVariantCheckpoint', 'remoteDefaultModelName'];
const helpers = manager.statements
  .filter(node => ts.isFunctionDeclaration(node) && functionNames.includes(node.name?.text))
  .map(node => node.getText(manager)).join('\n');
const remote = new Function(ts.transpileModule(helpers, {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
}).outputText + `\nreturn { ${functionNames.join(', ')} };`)();
assert.equal(remote.remoteVariantCheckpoint('unsloth/model', 'UD-Q4_K_XL', 'strata', details), `unsloth/model:${file}`);
assert.equal(remote.remoteVariantCheckpoint('unsloth/model', 'UD-Q4_K_XL', 'llamacpp', details), 'unsloth/model:UD-Q4_K_XL');
assert.equal(remote.remoteVariantCheckpoint('unsloth/model', 'variant', 'flm', details), 'unsloth/model');
assert.notEqual(remote.remoteDefaultModelName('huggingface', 'unsloth/model', details, 'UD-Q4_K_XL', 'strata'),
  remote.remoteDefaultModelName('huggingface', 'unsloth/model', details, 'UD-Q4_K_XL', 'llamacpp'));
const shared = { name: 'Qwen-Strata', recipe: 'strata', checkpoint: `unsloth/model:${file}` };
const llama = { ...shared, name: 'Qwen-GGUF', recipe: 'llamacpp' };
assert.equal(findRegisteredRemoteModel([llama, shared], 'huggingface', 'unsloth/model', 'UD-Q4_K_XL', 'strata', details), shared);
assert.equal(findRegisteredRemoteModel([llama, shared], 'huggingface', 'unsloth/model', 'UD-Q4_K_XL', 'llamacpp', details), llama);
assert.equal(findRegisteredRemoteModel([llama], 'huggingface', 'unsloth/model', 'UD-Q4_K_XL', 'strata', details), undefined);
console.log('Strata UI configuration, shared checkpoints and engine controls passed.');
