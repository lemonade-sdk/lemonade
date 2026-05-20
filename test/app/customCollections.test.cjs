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

function model(labels, downloaded = true, recipe = 'llamacpp') {
  return {
    checkpoint: `${labels.join('-') || 'chat'}.gguf`,
    recipe,
    suggested: true,
    labels,
    downloaded,
    max_prompt_length: 4096,
  };
}

const tests = [];

function defineTest(name, fn) {
  tests.push({ name, fn });
}

defineTest('custom collection pull payload uses PR 1842 server collection contract', () => {
  const payload = collectionUtils.buildCustomCollectionPullRequest({
    name: 'My Omni Collection',
    components: {
      llm: 'chat-llm',
      vision: 'chat-llm',
      image: 'image-model',
      edit: 'image-model',
      transcription: 'asr-model',
      speech: 'tts-model',
    },
  });

  assert.equal(payload.model_name, 'user.My-Omni-Collection');
  assert.equal(payload.recipe, 'collection.omni');
  assert.deepEqual(payload.components, ['chat-llm', 'image-model', 'asr-model', 'tts-model']);
});

defineTest('import accepts collection payloads and skips invalid entries', () => {
  const result = collectionUtils.importCustomCollections({
    version: 2,
    collections: [
      { name: 'Imported Collection', components: { llm: 'new-llm' } },
      { model_name: 'user.ExplicitCollection', components: ['explicit-llm', 'image-model'] },
      { name: 'Invalid Collection', components: { image: 'missing-llm' } },
    ],
  }, {
    'new-llm': model(['tool-calling']),
    'explicit-llm': model(['tool-calling']),
    'image-model': model(['image']),
  });

  assert.equal(result.imported, 2);
  assert.equal(result.skipped, 1);
  assert.deepEqual(result.collections.map((collection) => collection.id), [
    undefined,
    'user.ExplicitCollection',
  ]);
  assert.deepEqual(result.collections[1].components, {
    llm: 'explicit-llm',
    image: 'image-model',
  });
});

defineTest('model entries convert back to editable custom collection drafts', () => {
  const modelsData = {
    'user.CreatorStudio': {
      checkpoint: '',
      recipe: 'collection.omni',
      suggested: true,
      downloaded: true,
      components: ['chat-llm', 'vision-llm', 'image-model', 'asr-model', 'tts-model'],
      labels: ['custom'],
    },
    'chat-llm': model(['tool-calling']),
    'vision-llm': model(['vision']),
    'image-model': model(['image']),
    'asr-model': model(['transcription']),
    'tts-model': model(['tts']),
  };

  const collection = collectionUtils.modelEntryToCustomCollection('user.CreatorStudio', modelsData['user.CreatorStudio'], modelsData);
  assert.equal(collection.id, 'user.CreatorStudio');
  assert.equal(collection.name, 'CreatorStudio');
  assert.deepEqual(collection.components, {
    llm: 'chat-llm',
    vision: 'vision-llm',
    image: 'image-model',
    transcription: 'asr-model',
    speech: 'tts-model',
  });
});

defineTest('role options include only downloaded compatible concrete models', () => {
  const modelsData = {
    'plain-chat': model([]),
    'vision-chat': model(['vision']),
    'image-model': model(['image']),
    'edit-model': model(['edit']),
    'asr-model': model(['transcription']),
    'tts-model': model(['speech']),
    'not-downloaded-image': model(['image'], false),
    'user.Collection': model(['custom'], true, 'collection.omni'),
  };
  modelsData['user.Collection'].components = ['plain-chat', 'image-model'];

  assert.deepEqual(collectionUtils.getCollectionRoleOptions(modelsData, 'llm').map((entry) => entry.id), [
    'plain-chat',
    'vision-chat',
  ]);
  assert.deepEqual(collectionUtils.getCollectionRoleOptions(modelsData, 'image').map((entry) => entry.id), [
    'image-model',
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
