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
const sampler = load('samplerArgs.ts');
const { isStrataGguf } = load('features/models/huggingFaceSearch.ts');
const presentation = load('modelPresentation.ts');

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
const catalog = Object.entries(JSON.parse(fs.readFileSync(path.resolve(__dirname, '../../../src/cpp/resources/server_models.json'), 'utf8')))
  .filter(([, row]) => row.recipe === 'strata')
  .map(([id, row]) => ({ id, ...row, checkpoint: row.checkpoint || row.checkpoints.main, supported_backends: row.strata.backends }));
const qwen = 'Qwen3.6-35B-A3B-UD-Q4_K_XL.gguf';
const swift = 'Qwen3.8-Flash-Next-Gyro-S-TQ1_0.gguf';
for (const backend of ['cuda', 'rocm']) assert.ok(isStrataGguf(qwen, catalog, [backend]));
for (const backend of ['metal', 'sycl']) assert.ok(!isStrataGguf(qwen, catalog, [backend]));
assert.ok(isStrataGguf(swift, catalog, ['metal']));
assert.ok(isStrataGguf(`nested/${swift}`, catalog, ['rocm']));
assert.ok(!isStrataGguf(swift, catalog, []));
assert.ok(!isStrataGguf(swift, [], ['rocm']));
assert.ok(!isStrataGguf('DeepSeek-V4.1-Flash-UD-Q8_K_XL.gguf', catalog, ['rocm']));

console.log('Strata UI configuration and engine controls passed.');
