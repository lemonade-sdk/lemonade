for (const key of Object.keys(process.env)) {
  if (key.startsWith('npm_') || key === 'INIT_CWD') {
    delete process.env[key];
  }
}

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const appRoot = path.resolve(__dirname, '..', '..', 'src', 'app');
let ts;
try {
  ts = require(path.join(appRoot, 'node_modules', 'typescript'));
} catch (_) {
  ts = require('typescript');
}

const originalTsLoader = require.extensions['.ts'];

require.extensions['.ts'] = function loadTypeScript(module, filename) {
  const source = fs.readFileSync(filename, 'utf8');
  const output = ts.transpileModule(source, {
    compilerOptions: {
      esModuleInterop: true,
      module: ts.ModuleKind.CommonJS,
      moduleResolution: ts.ModuleResolutionKind.NodeJs,
      target: ts.ScriptTarget.ES2020,
    },
    fileName: filename,
  }).outputText;
  module._compile(output, filename);
};

const collectionUtilsPath = path.join(appRoot, 'src', 'renderer', 'utils', 'customCollections.ts');
const collectionUtils = require(collectionUtilsPath);

if (originalTsLoader) {
  require.extensions['.ts'] = originalTsLoader;
} else {
  delete require.extensions['.ts'];
}

function model(labels, downloaded = true, recipe = 'llamacpp', extra = {}) {
  return {
    checkpoint: `${labels.join('-') || 'chat'}.gguf`,
    recipe,
    suggested: true,
    labels,
    downloaded,
    max_prompt_length: 4096,
    ...extra,
  };
}

const tests = [];

function defineTest(name, fn) {
  tests.push({ name, fn });
}

defineTest('buildCustomCollectionPullRequest uses user namespace, collection.omni, and deduped role order', () => {
  const request = collectionUtils.buildCustomCollectionPullRequest({
    name: 'My Kit',
    components: {
      llm: 'planner',
      vision: 'planner',
      image: 'image-model',
      edit: 'image-model',
      transcription: 'asr-model',
      speech: 'tts-model',
    },
  });

  assert.equal(request.model_name, 'user.My-Kit');
  assert.equal(request.recipe, 'collection.omni');
  assert.deepEqual(request.components, ['planner', 'image-model', 'asr-model', 'tts-model']);
});

defineTest('modelEntryToCustomCollection reads server-side user collections and built-in templates', () => {
  const modelsData = {
    'planner': model(['tool-calling']),
    'image-model': model(['image']),
    'user.MyKit': model(['collection'], true, 'collection.omni', {
      components: ['planner', 'image-model'],
    }),
    'LMX-Omni-5.5B-Lite': model(['collection'], true, 'collection.omni', {
      components: ['planner', 'image-model'],
    }),
  };

  const custom = collectionUtils.modelEntryToCustomCollection('user.MyKit', modelsData['user.MyKit'], modelsData);
  assert.equal(custom.id, 'user.MyKit');
  assert.equal(custom.name, 'MyKit');
  assert.equal(custom.components.llm, 'planner');
  assert.equal(custom.components.image, 'image-model');

  const template = collectionUtils.modelEntryToCustomCollection('LMX-Omni-5.5B-Lite', modelsData['LMX-Omni-5.5B-Lite'], modelsData);
  assert.equal(template.id, 'LMX-Omni-5.5B-Lite');
  assert.equal(template.name, 'LMX-Omni-5.5B-Lite');
});

defineTest('export includes endpoint collection entries plus component checkpoint metadata', () => {
  const modelsData = {
    'planner': model(['tool-calling'], true, 'llamacpp', { checkpoint: 'org/planner:Q4_K_M' }),
    'image-model': model(['image'], true, 'sd-cpp', {
      checkpoint: 'org/image:image.gguf',
      image_defaults: { width: 512, height: 512 },
    }),
    'multi-file': model(['edit'], true, 'sd-cpp', {
      checkpoint: '',
      checkpoints: { main: 'org/edit:model.safetensors', vae: 'org/edit:vae.safetensors' },
    }),
  };

  const payload = collectionUtils.buildCustomCollectionsExportPayload([{
    id: 'user.CreatorStudio',
    name: 'CreatorStudio',
    components: { llm: 'planner', image: 'image-model', edit: 'multi-file' },
  }], modelsData);

  assert.equal(payload.version, 3);
  assert.deepEqual(payload.collections, [{
    model_name: 'user.CreatorStudio',
    recipe: 'collection.omni',
    components: ['planner', 'image-model', 'multi-file'],
  }]);
  assert.deepEqual(payload.models.map((entry) => entry.model_name), ['planner', 'image-model', 'multi-file']);
  assert.equal(payload.models.find((entry) => entry.model_name === 'planner').checkpoint, 'org/planner:Q4_K_M');
  assert.deepEqual(payload.models.find((entry) => entry.model_name === 'multi-file').checkpoints, {
    main: 'org/edit:model.safetensors',
    vae: 'org/edit:vae.safetensors',
  });
});

defineTest('import uses exported component model metadata to validate collections before registration', () => {
  const payload = {
    version: 3,
    collections: [{
      model_name: 'user.ImportedKit',
      recipe: 'collection.omni',
      components: ['planner', 'image-model'],
    }],
    models: [
      { model_name: 'planner', recipe: 'llamacpp', checkpoint: 'org/planner:Q4_K_M', labels: ['tool-calling'] },
      { model_name: 'image-model', recipe: 'sd-cpp', checkpoint: 'org/image:image.gguf', labels: ['image'] },
    ],
  };

  const result = collectionUtils.importCustomCollections(payload, {});
  assert.equal(result.imported, 1);
  assert.equal(result.skipped, 0);
  assert.equal(result.models.length, 2);
  assert.equal(result.collections[0].id, 'user.ImportedKit');
  assert.equal(result.collections[0].components.llm, 'planner');
  assert.equal(result.collections[0].components.image, 'image-model');
});

defineTest('role options include registered concrete compatible models', () => {
  const modelsData = {
    'plain-chat': model(['tool-calling']),
    'vision-chat': model(['vision', 'tool-calling']),
    'image-model': model(['image']),
    'edit-model': model(['edit']),
    'asr-model': model(['transcription']),
    'tts-model': model(['speech']),
    'registered-image': model(['image'], false),
    'user.Collection': model(['collection'], true, 'collection.omni', { components: ['plain-chat'] }),
  };

  assert.deepEqual(collectionUtils.getCollectionRoleOptions(modelsData, 'llm').map((entry) => entry.id), [
    'plain-chat',
    'vision-chat',
  ]);
  assert.deepEqual(collectionUtils.getCollectionRoleOptions(modelsData, 'image').map((entry) => entry.id), [
    'image-model',
    'registered-image',
  ]);
  assert.deepEqual(collectionUtils.getCollectionRoleOptions(modelsData, 'edit').map((entry) => entry.id), [
    'edit-model',
  ]);
  assert.deepEqual(collectionUtils.getCollectionRoleOptions(modelsData, 'transcription').map((entry) => entry.id), [
    'asr-model',
  ]);
  assert.deepEqual(collectionUtils.getCollectionRoleOptions(modelsData, 'speech').map((entry) => entry.id), [
    'tts-model',
  ]);
});

let passed = 0;

for (const { name, fn } of tests) {
  try {
    fn();
    passed += 1;
    console.log(`PASS ${name}`);
  } catch (error) {
    console.error(`FAIL ${name}`);
    console.error(error && error.stack ? error.stack : error);
    process.exitCode = 1;
    break;
  }
}

if (process.exitCode !== 1) {
  console.log(`All custom collection tests passed (${passed}/${tests.length}).`);
}

process.exit(process.exitCode || 0);
