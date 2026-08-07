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
const modelDataPath = path.join(appRoot, 'src', 'renderer', 'utils', 'modelData.ts');
const modelDataUtils = require(modelDataPath);

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

defineTest('collection export normalizes the /models/{id} object into the import-ready file shape', () => {
  // A live /models/{id} collection object: id-keyed, runtime fields present,
  // components as names, models[] embedding each component verbatim.
  const raw = {
    id: 'CreatorStudio',
    object: 'model',
    created: 1234567890,
    owned_by: 'lemonade',
    recipe: 'collection.omni',
    checkpoint: '',
    checkpoints: { main: '' },
    components: ['planner', 'multi-file'],
    labels: [],
    recipe_options: {},
    suggested: true,
    downloaded: true,
    models: [
      {
        id: 'planner', object: 'model', created: 1234567890, owned_by: 'lemonade',
        recipe: 'llamacpp', checkpoint: 'org/planner:Q4_K_M',
        checkpoints: { main: 'org/planner:Q4_K_M' },
        components: [], labels: ['tool-calling'], recipe_options: {},
        suggested: true, downloaded: true, size: 2.5,
      },
      {
        id: 'multi-file', object: 'model', created: 1234567890, owned_by: 'lemonade',
        recipe: 'sd-cpp', checkpoint: 'org/edit:model.safetensors',
        checkpoints: { main: 'org/edit:model.safetensors', vae: 'org/edit:vae.safetensors' },
        components: [], labels: ['edit'], recipe_options: {},
        suggested: false, downloaded: false, size: 6.9,
      },
    ],
  };

  const { filename, payload } = modelDataUtils.normalizeModelExportPayload(raw);

  // Filename has no user. prefix; the model_name inside is import-ready.
  assert.equal(filename, 'CreatorStudio.json');
  assert.equal(payload.model_name, 'user.CreatorStudio');
  assert.ok(!('id' in payload));

  // User-specific runtime fields and wire decorations never reach the file.
  for (const key of ['suggested', 'created', 'downloaded', 'object', 'owned_by']) {
    assert.ok(!(key in payload), `top-level ${key} should be stripped`);
    for (const entry of payload.models) {
      assert.ok(!(key in entry), `models[] ${key} should be stripped`);
    }
  }

  // Components stay as ordered names; models[] elements are normalized like
  // regular-model export files (model_name-keyed, checkpoints deduped).
  assert.deepEqual(payload.components, ['planner', 'multi-file']);
  assert.deepEqual(payload.models.map((entry) => entry.model_name), ['planner', 'multi-file']);
  assert.ok(!('checkpoint' in payload.models[0]), 'singular checkpoint deduped when checkpoints present');
  assert.deepEqual(payload.models[1].checkpoints, {
    main: 'org/edit:model.safetensors',
    vae: 'org/edit:vae.safetensors',
  });
});

defineTest('model export preserves remote registry provenance and drops local origins', () => {
  const remote = modelDataUtils.normalizeModelExportPayload({
    id: 'remote-model', recipe: 'llamacpp', checkpoint: 'org/repo:Q4_K_M',
    checkpoints: { main: 'org/repo:Q4_K_M' }, source: 'modelscope',
    registry_source: 'modelscope', components: [],
  }).payload;
  assert.equal(remote.source, 'modelscope');
  assert.equal(remote.registry_source, 'modelscope');

  const local = modelDataUtils.normalizeModelExportPayload({
    id: 'local-model', recipe: 'llamacpp', checkpoint: 'models--local/file.gguf',
    checkpoints: { main: 'models--local/file.gguf' }, source: 'local_upload',
    registry_source: 'huggingface', components: [],
  }).payload;
  assert.ok(!('source' in local));
  assert.ok(!('registry_source' in local));
});

defineTest('regular-model export carries no collection fields', () => {
  const raw = {
    id: 'planner', object: 'model', created: 1234567890, owned_by: 'lemonade',
    recipe: 'llamacpp', checkpoint: 'org/planner:Q4_K_M',
    checkpoints: { main: 'org/planner:Q4_K_M' },
    components: [], labels: ['tool-calling'], recipe_options: {},
    suggested: true, downloaded: true, size: 2.5,
  };

  const { filename, payload } = modelDataUtils.normalizeModelExportPayload(raw);
  assert.equal(filename, 'planner.json');
  assert.equal(payload.model_name, 'user.planner');
  assert.ok(!('components' in payload));
  assert.ok(!('models' in payload));
  assert.ok(!('downloaded' in payload));
  assert.ok(!('suggested' in payload));
  assert.ok(!('created' in payload));
});

defineTest('router collection export preserves routing and components', () => {
  const routing = {
    candidates: ['local', 'remote'],
    default_model: 'local',
    rules: [
      { id: 'code', match: { keywords_any: ['def '] }, route_to: 'remote' },
    ],
  };
  const raw = {
    id: 'RouterKit', object: 'model', created: 1234567890, owned_by: 'lemonade',
    recipe: 'collection.router',
    checkpoint: '',
    checkpoints: { main: '' },
    components: ['local', 'remote'],
    labels: [],
    recipe_options: {},
    routing,
    suggested: true,
    downloaded: true,
  };

  const { filename, payload } = modelDataUtils.normalizeModelExportPayload(raw);
  assert.equal(filename, 'RouterKit.json');
  assert.equal(payload.model_name, 'user.RouterKit');
  assert.deepEqual(payload.components, ['local', 'remote']);
  assert.deepEqual(payload.routing, routing);
  assert.ok(!('downloaded' in payload));
  assert.ok(!('suggested' in payload));
});

defineTest('DEFAULT_OMNI_SYSTEM_PROMPT is the canonical default from toolDefinitions.json', () => {
  const toolDefinitionsPath = path.join(appRoot, 'src', 'renderer', 'utils', 'toolDefinitions.json');
  const toolDefs = JSON.parse(fs.readFileSync(toolDefinitionsPath, 'utf8'));
  assert.equal(
    typeof collectionUtils.DEFAULT_OMNI_SYSTEM_PROMPT,
    'string',
    'The default prompt constant should be exported as a string.',
  );
  assert.equal(
    collectionUtils.DEFAULT_OMNI_SYSTEM_PROMPT,
    toolDefs.system_prompt,
    'The editor default must match toolDefinitions.json verbatim so diff-on-save is exact.',
  );
});

defineTest('buildCustomCollectionPullRequest attaches an optional system_prompt and omits it when blank', () => {
  const baseDraft = {
    name: 'WithPrompt',
    components: { llm: 'planner' },
  };

  const withoutPrompt = collectionUtils.buildCustomCollectionPullRequest(baseDraft);
  assert.equal(
    'system_prompt' in withoutPrompt,
    false,
    'A draft without systemPrompt should not include the wire field.',
  );

  const blankPrompt = collectionUtils.buildCustomCollectionPullRequest({ ...baseDraft, systemPrompt: '   \n   ' });
  assert.equal(
    'system_prompt' in blankPrompt,
    false,
    'A whitespace-only systemPrompt should be treated as absent.',
  );

  const customPrompt = '   You are a helpful tester. Tools: {tool_list}{tool_guidance}\n   ';
  const withPrompt = collectionUtils.buildCustomCollectionPullRequest({ ...baseDraft, systemPrompt: customPrompt });
  assert.equal(withPrompt.system_prompt, customPrompt.trim());
});

defineTest('modelEntryToCustomCollection surfaces system_prompt back into the editable draft', () => {
  const modelsData = {
    'planner': model(['tool-calling']),
    'user.MyKit': model(['collection'], true, 'collection.omni', {
      components: ['planner'],
      system_prompt: 'Greetings. {tool_list}{tool_guidance}',
    }),
  };

  const custom = collectionUtils.modelEntryToCustomCollection('user.MyKit', modelsData['user.MyKit'], modelsData);
  assert.equal(custom.systemPrompt, 'Greetings. {tool_list}{tool_guidance}');
});

defineTest('collection export round-trip preserves system_prompt on the import-ready payload', () => {
  const raw = {
    id: 'CreatorStudio',
    object: 'model',
    created: 1234567890,
    owned_by: 'lemonade',
    recipe: 'collection.omni',
    checkpoint: '',
    checkpoints: { main: '' },
    components: ['planner'],
    labels: [],
    recipe_options: {},
    suggested: true,
    downloaded: true,
    system_prompt: 'Custom override. {tool_list}{tool_guidance}',
    models: [
      {
        id: 'planner', object: 'model', created: 1234567890, owned_by: 'lemonade',
        recipe: 'llamacpp', checkpoint: 'org/planner:Q4_K_M',
        checkpoints: { main: 'org/planner:Q4_K_M' },
        components: [], labels: ['tool-calling'], recipe_options: {},
        suggested: true, downloaded: true, size: 2.5,
      },
    ],
  };

  const { payload } = modelDataUtils.normalizeModelExportPayload(raw);
  assert.equal(
    payload.system_prompt,
    'Custom override. {tool_list}{tool_guidance}',
    'system_prompt should survive the export normalization step.',
  );
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
