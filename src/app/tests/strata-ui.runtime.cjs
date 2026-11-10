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

const specs = sampler.samplerFieldsForRecipe('strata');
assert.equal(specs.find(spec => spec.flag === '--mtp').backendDefault, '2');
const args = '--mtp 0 --no-warmup';
const split = sampler.splitSamplerArgs(specs, args);
assert.equal(split.rest, '');
assert.equal(split.fields['--mtp'], '0');
assert.equal(split.fields['--no-warmup'], 'true');
assert.deepEqual(sampler.buildArgsMap(sampler.parseCustomArgs(sampler.composeSamplerArgs(specs, split.fields, split.rest))),
  sampler.buildArgsMap(sampler.parseCustomArgs(args)));
const flashSpecs = sampler.samplerFieldsForRecipe('strata', 'repo:Qwen3.8-Flash-Next-Gyro-S-TQ1_0.gguf');
assert.equal(flashSpecs.find(spec => spec.flag === '--mtp').kind, 'text');
assert.equal(sampler.composeSamplerArgs(flashSpecs, { '--mtp': 'off', '--kv-resident': '0' }, ''),
  '--mtp off --kv-resident 0');
assert.ok(isStrataGguf('Qwen3.8-Flash-Next-Gyro-S-TQ1_0.gguf'));
assert.ok(!isStrataGguf('DeepSeek-V4.1-Flash-UD-Q8_K_XL.gguf'));

const file = 'Qwen3.6-35B-A3B-UD-Q4_K_XL.gguf';
const details = { variants: [{ name: 'UD-Q4_K_XL', primary_file: file }], mmproj_files: [] };
const shared = { name: 'Qwen-Strata', recipe: 'strata', checkpoint: `unsloth/model:${file}` };
const llama = { ...shared, name: 'Qwen-GGUF', recipe: 'llamacpp' };
assert.equal(findRegisteredRemoteModel([llama, shared], 'huggingface', 'unsloth/model', 'UD-Q4_K_XL', 'strata', details), shared);
assert.equal(findRegisteredRemoteModel([llama, shared], 'huggingface', 'unsloth/model', 'UD-Q4_K_XL', 'llamacpp', details), llama);
assert.equal(findRegisteredRemoteModel([llama], 'huggingface', 'unsloth/model', 'UD-Q4_K_XL', 'strata', details), undefined);
console.log('Strata UI configuration, shared checkpoints and engine controls passed.');
