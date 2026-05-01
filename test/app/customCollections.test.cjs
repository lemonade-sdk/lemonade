for (const key of Object.keys(process.env)) {
  if (key.startsWith('npm_') || key === 'INIT_CWD') {
    delete process.env[key];
  }
}

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const appRoot = path.resolve(__dirname, '..', '..', 'src', 'app');
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

class MemoryStorage {
  constructor() {
    this.values = new Map();
  }

  get length() {
    return this.values.size;
  }

  clear() {
    this.values.clear();
  }

  getItem(key) {
    return this.values.has(key) ? this.values.get(key) : null;
  }

  key(index) {
    return Array.from(this.values.keys())[index] ?? null;
  }

  removeItem(key) {
    this.values.delete(key);
  }

  setItem(key, value) {
    this.values.set(key, String(value));
  }
}

const storage = new MemoryStorage();
const dispatchedEvents = [];

global.CustomEvent = class CustomEvent {
  constructor(type) {
    this.type = type;
  }
};

global.window = {
  localStorage: storage,
  dispatchEvent(event) {
    dispatchedEvents.push(event.type);
    return true;
  },
};

function resetStorage() {
  storage.clear();
  dispatchedEvents.length = 0;
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

defineTest('save, edit, delete, and export custom collections', () => {
  const first = collectionUtils.saveCustomCollection({
    name: 'My Collection',
    components: { llm: 'llm-a', image: 'image-a' },
  });

  assert.equal(first.id, 'collection.my-collection');
  assert.equal(first.name, 'My Collection');
  assert.equal(first.components.llm, 'llm-a');
  assert.match(first.createdAt, /^\d{4}-\d{2}-\d{2}T/);

  const duplicate = collectionUtils.saveCustomCollection({
    name: 'My Collection',
    components: { llm: 'llm-b' },
  });

  assert.equal(duplicate.id, 'collection.my-collection-2');

  const edited = collectionUtils.saveCustomCollection({
    id: first.id,
    name: 'Renamed Collection',
    components: { llm: 'llm-a', speech: 'tts-a' },
  });

  assert.equal(edited.id, first.id);
  assert.equal(edited.createdAt, first.createdAt);
  assert.equal(edited.components.speech, 'tts-a');

  const exported = collectionUtils.buildCustomCollectionsExportPayload();
  assert.equal(exported.version, 1);
  assert.equal(exported.collections.length, 2);
  assert.deepEqual(exported.collections.map((collection) => collection.id), [
    'collection.my-collection-2',
    'collection.my-collection',
  ]);

  collectionUtils.deleteCustomCollection(first.id);

  assert.deepEqual(collectionUtils.loadCustomCollections().map((collection) => collection.id), [
    'collection.my-collection-2',
  ]);
  assert.ok(dispatchedEvents.every((eventType) => eventType === 'customCollectionsUpdated'));
});

defineTest('import accepts payloads, skips invalid entries, and avoids generated ID collisions', () => {
  collectionUtils.saveCustomCollection({
    name: 'Imported Collection',
    components: { llm: 'existing-llm' },
  });

  const result = collectionUtils.importCustomCollections({
    version: 1,
    collections: [
      { name: 'Imported Collection', components: { llm: 'new-llm' } },
      { id: 'collection.explicit', name: 'Explicit Collection', components: { llm: 'explicit-llm' } },
      { name: 'Invalid Collection', components: { image: 'missing-llm' } },
    ],
  });

  assert.equal(result.imported, 2);
  assert.equal(result.skipped, 1);

  const collections = collectionUtils.loadCustomCollections();
  assert.deepEqual(collections.map((collection) => collection.id), [
    'collection.explicit',
    'collection.imported-collection',
    'collection.imported-collection-2',
  ]);

  const generatedImport = collections.find((collection) => collection.id === 'collection.imported-collection-2');
  assert.equal(generatedImport.components.llm, 'new-llm');
});

defineTest('mergeCustomCollectionsIntoModelsData creates synthetic collection models and hides stale collections', () => {
  collectionUtils.saveCustomCollection({
    name: 'Usable Collection',
    components: {
      llm: 'chat-llm',
      vision: 'vision-llm',
      image: 'image-model',
      speech: 'speech-model',
    },
  });
  collectionUtils.saveCustomCollection({
    name: 'Stale Collection',
    components: { llm: 'chat-llm', image: 'missing-image-model' },
  });

  const merged = collectionUtils.mergeCustomCollectionsIntoModelsData({
    'chat-llm': model(['tool-calling']),
    'vision-llm': model(['vision']),
    'image-model': model(['image']),
    'speech-model': model(['tts']),
  });

  const collection = merged['collection.usable-collection'];
  assert.equal(collection.recipe, 'collection');
  assert.equal(collection.source, 'custom-collection');
  assert.equal(collection.collection_name, 'Usable Collection');
  assert.deepEqual(collection.composite_models, ['chat-llm', 'vision-llm', 'image-model', 'speech-model']);
  assert.ok(collection.labels.includes('collection'));
  assert.ok(collection.labels.includes('vision'));
  assert.ok(collection.labels.includes('image'));
  assert.ok(collection.labels.includes('speech'));
  assert.equal(merged['collection.stale-collection'], undefined);
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
    'collection-model': model([], true, 'collection'),
    'collection.fake': model(['collection']),
  };

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
  resetStorage();
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
