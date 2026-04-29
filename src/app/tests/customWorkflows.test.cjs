for (const key of Object.keys(process.env)) {
  if (key.startsWith("npm_") || key === "INIT_CWD") {
    delete process.env[key];
  }
}

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const appRoot = path.resolve(__dirname, '..');
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

const workflowUtilsPath = path.join(appRoot, 'src', 'renderer', 'utils', 'customWorkflows.ts');
const workflowUtils = require(workflowUtilsPath);

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

defineTest('save, edit, delete, and export custom workflows', () => {
  const first = workflowUtils.saveCustomWorkflow({
    name: 'My Workflow',
    components: { llm: 'llm-a', image: 'image-a' },
  });

  assert.equal(first.id, 'workflow.my-workflow');
  assert.equal(first.name, 'My Workflow');
  assert.equal(first.components.llm, 'llm-a');
  assert.match(first.createdAt, /^\d{4}-\d{2}-\d{2}T/);

  const duplicate = workflowUtils.saveCustomWorkflow({
    name: 'My Workflow',
    components: { llm: 'llm-b' },
  });

  assert.equal(duplicate.id, 'workflow.my-workflow-2');

  const edited = workflowUtils.saveCustomWorkflow({
    id: first.id,
    name: 'Renamed Workflow',
    components: { llm: 'llm-a', speech: 'tts-a' },
  });

  assert.equal(edited.id, first.id);
  assert.equal(edited.createdAt, first.createdAt);
  assert.equal(edited.components.speech, 'tts-a');

  const exported = workflowUtils.buildCustomWorkflowsExportPayload();
  assert.equal(exported.version, 1);
  assert.equal(exported.workflows.length, 2);
  assert.deepEqual(exported.workflows.map((workflow) => workflow.id), [
    'workflow.my-workflow-2',
    'workflow.my-workflow',
  ]);

  workflowUtils.deleteCustomWorkflow(first.id);

  assert.deepEqual(workflowUtils.loadCustomWorkflows().map((workflow) => workflow.id), [
    'workflow.my-workflow-2',
  ]);
  assert.ok(dispatchedEvents.every((eventType) => eventType === 'customWorkflowsUpdated'));
});

defineTest('import accepts payloads, skips invalid entries, and avoids generated ID collisions', () => {
  workflowUtils.saveCustomWorkflow({
    name: 'Imported Workflow',
    components: { llm: 'existing-llm' },
  });

  const result = workflowUtils.importCustomWorkflows({
    version: 1,
    workflows: [
      { name: 'Imported Workflow', components: { llm: 'new-llm' } },
      { id: 'workflow.explicit', name: 'Explicit Workflow', components: { llm: 'explicit-llm' } },
      { name: 'Invalid Workflow', components: { image: 'missing-llm' } },
    ],
  });

  assert.equal(result.imported, 2);
  assert.equal(result.skipped, 1);

  const workflows = workflowUtils.loadCustomWorkflows();
  assert.deepEqual(workflows.map((workflow) => workflow.id), [
    'workflow.explicit',
    'workflow.imported-workflow',
    'workflow.imported-workflow-2',
  ]);

  const generatedImport = workflows.find((workflow) => workflow.id === 'workflow.imported-workflow-2');
  assert.equal(generatedImport.components.llm, 'new-llm');
});

defineTest('mergeCustomWorkflowsIntoModelsData creates synthetic collection models and hides stale workflows', () => {
  workflowUtils.saveCustomWorkflow({
    name: 'Usable Workflow',
    components: {
      llm: 'chat-llm',
      vision: 'vision-llm',
      image: 'image-model',
      speech: 'speech-model',
    },
  });
  workflowUtils.saveCustomWorkflow({
    name: 'Stale Workflow',
    components: { llm: 'chat-llm', image: 'missing-image-model' },
  });

  const merged = workflowUtils.mergeCustomWorkflowsIntoModelsData({
    'chat-llm': model(['tool-calling']),
    'vision-llm': model(['vision']),
    'image-model': model(['image']),
    'speech-model': model(['tts']),
  });

  const workflow = merged['workflow.usable-workflow'];
  assert.equal(workflow.recipe, 'collection');
  assert.equal(workflow.source, 'custom-workflow');
  assert.equal(workflow.workflow_name, 'Usable Workflow');
  assert.deepEqual(workflow.composite_models, ['chat-llm', 'vision-llm', 'image-model', 'speech-model']);
  assert.ok(workflow.labels.includes('workflow'));
  assert.ok(workflow.labels.includes('vision'));
  assert.ok(workflow.labels.includes('image'));
  assert.ok(workflow.labels.includes('speech'));
  assert.equal(merged['workflow.stale-workflow'], undefined);
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
    'workflow.fake': model(['workflow']),
  };

  assert.deepEqual(workflowUtils.getWorkflowRoleOptions(modelsData, 'llm').map((entry) => entry.id), [
    'plain-chat',
    'vision-chat',
  ]);
  assert.deepEqual(workflowUtils.getWorkflowRoleOptions(modelsData, 'image').map((entry) => entry.id), [
    'image-model',
  ]);
  assert.deepEqual(workflowUtils.getWorkflowRoleOptions(modelsData, 'edit').map((entry) => entry.id), [
    'edit-model',
  ]);
  assert.deepEqual(workflowUtils.getWorkflowRoleOptions(modelsData, 'transcription').map((entry) => entry.id), [
    'asr-model',
  ]);
  assert.deepEqual(workflowUtils.getWorkflowRoleOptions(modelsData, 'speech').map((entry) => entry.id), [
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
  console.log(`All custom workflow tests passed (${passed}/${tests.length}).`);
}

process.exit(process.exitCode || 0);
