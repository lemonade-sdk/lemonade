/*
 * The GUI classifies a model by reading the deployment label the server already
 * stamped on it. These checks pin that lookup to two external sources of truth:
 * the label table in docs/api/openai.md, and the shipped model registry.
 */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const repoRoot = path.resolve(root, '../..');

function loadTypeScriptModule(filename) {
  const source = fs.readFileSync(filename, 'utf8');
  const compiled = ts.transpileModule(source, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS,
      esModuleInterop: true,
    },
    fileName: filename,
    reportDiagnostics: true,
  });
  const diagnostics = compiled.diagnostics || [];
  assert.equal(
    diagnostics.length,
    0,
    diagnostics.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'),
  );
  const module = { exports: {} };
  Function('exports', 'require', 'module', '__filename', '__dirname', compiled.outputText)(
    module.exports,
    require,
    module,
    filename,
    path.dirname(filename),
  );
  return module.exports;
}

const capabilitiesPath = path.join(root, 'src/modelCapabilities.ts');
const capabilitiesSource = fs.readFileSync(capabilitiesPath, 'utf8');
const {
  capabilityFromLoaded,
  capabilityFromModelInfo,
  deploymentKindFromLabels,
  identityFromModelInfo,
  modelStructure,
  capabilityLabel,
  identityFor,
  isComposerSelectableCapability,
} = loadTypeScriptModule(capabilitiesPath);

/* ── Every documented deployment label maps to a surface ───────────── */

const EXPECTED = {
  chat: 'chat',
  transcription: 'audio',
  embeddings: 'embedding',
  embedding: 'embedding',
  reranking: 'reranking',
  image: 'image',
  tts: 'tts',
  'audio-generation': 'audio-generation',
  classification: 'classification',
  classifier: 'classification',
  '3d': 'model3d',
};

for (const [label, kind] of Object.entries(EXPECTED)) {
  assert.equal(
    deploymentKindFromLabels([label]),
    kind,
    `the '${label}' deployment label must resolve to ${kind}`,
  );
}

/* ── Drift guard against docs/api/openai.md ────────────────────────── */

const apiDocs = fs.readFileSync(path.join(repoRoot, 'docs/api/openai.md'), 'utf8');
const deploymentSection = apiDocs.slice(
  apiDocs.indexOf('**Deployment labels**'),
  apiDocs.indexOf('**Input-modality labels**'),
);
assert.ok(deploymentSection.length > 0, 'the deployment label section must be findable in the API docs');

const documented = new Set();
for (const [, label] of deploymentSection.matchAll(/^\| `([a-z0-9-]+)` \|/gm)) documented.add(label);
// "Also accepted as `embedding`." / "Also accepted as `classifier`."
for (const [, alias] of deploymentSection.matchAll(/Also accepted as `([a-z0-9-]+)`/g)) documented.add(alias);

assert.equal(documented.size, 11, 'nine deployment labels plus the two documented aliases');

const literal = capabilitiesSource.slice(
  capabilitiesSource.indexOf('const DEPLOYMENT_LABEL_KIND'),
  capabilitiesSource.indexOf('/** ModelType strings the router reports'),
);
const implemented = new Set(
  [...literal.matchAll(/^\s{2}'?([a-z0-9-]+)'?:/gm)].map(match => match[1]),
);

assert.deepEqual(
  [...implemented].sort(),
  [...documented].sort(),
  'the GUI lookup must carry exactly the deployment labels documented in docs/api/openai.md',
);

/* ── chat outranks every other mode, as find_deployment_mode does ──── */

assert.equal(deploymentKindFromLabels(['chat', 'transcription']), 'chat');
assert.equal(deploymentKindFromLabels(['image', 'chat']), 'chat', 'order does not matter');
assert.equal(
  deploymentKindFromLabels(['CHAT', ' Vision ']),
  'chat',
  'labels are compared case- and whitespace-insensitively',
);

/* ── Characteristics and input modalities name no mode ─────────────── */

for (const labels of [[], ['reasoning'], ['tool-calling', 'coding'], ['vision'], ['chat-transcription'], ['hot']]) {
  assert.equal(
    deploymentKindFromLabels(labels),
    'unknown',
    `${JSON.stringify(labels)} names no deployment mode`,
  );
}

/* ── Collections deploy as chat; Omni/Router are structural ────────── */

const omniCollection = { id: 'user.suite', recipe: 'collection.omni', labels: ['chat'], components: ['a', 'b'] };
const routerCollection = { id: 'user.router', recipe: 'collection.router', labels: ['chat'] };

assert.equal(modelStructure('llamacpp'), 'single');
assert.equal(modelStructure('collection.omni'), 'omni');
assert.equal(modelStructure('collection'), 'omni');
assert.equal(modelStructure('collection.router'), 'router');

assert.equal(capabilityFromModelInfo(omniCollection), 'chat', 'a collection deploys as chat');
assert.equal(identityFromModelInfo(omniCollection), 'omni');
assert.equal(identityFromModelInfo(routerCollection), 'router');
assert.equal(capabilityLabel(identityFromModelInfo(omniCollection)), 'Omni');
// One identity rule, shared by rows, snapshots and the chat composer.
assert.equal(identityFor('chat', 'collection.omni'), 'omni');
assert.equal(identityFor('chat', 'collection.router'), 'router');
assert.equal(identityFor('image', 'sd-cpp'), 'image');
assert.equal(isComposerSelectableCapability('chat'), true);
assert.equal(isComposerSelectableCapability('embedding'), false);

/* ── Loaded models are classified by the router's own ModelType ────── */

const LOADED = {
  llm: 'chat',
  embedding: 'embedding',
  reranking: 'reranking',
  transcription: 'audio',
  image: 'image',
  tts: 'tts',
  'audio-generation': 'audio-generation',
  classification: 'classification',
  mesh: 'model3d',
};
for (const [type, kind] of Object.entries(LOADED)) {
  assert.equal(
    capabilityFromLoaded({ model_name: 'x', type }),
    kind,
    `a loaded model reporting type '${type}' is ${kind}`,
  );
}

// A collection is synthesized client-side when its components are all loaded:
// it reports type 'omni', carries no labels, and is known only by its recipe.
assert.equal(
  capabilityFromLoaded({ model_name: 'user.suite', type: 'omni', recipe: 'collection.omni' }),
  'chat',
  'a loaded collection stays selectable rather than falling through to unknown',
);
assert.equal(
  capabilityFromLoaded({ model_name: 'x', type: 'omni', recipe: 'llamacpp' }),
  'unknown',
  'a non-collection reporting an unknown ModelType is still unknown',
);

/* ── The shipped registry never produces Unknown ───────────────────── */

const registry = JSON.parse(
  fs.readFileSync(path.join(repoRoot, 'src/cpp/resources/server_models.json'), 'utf8'),
);
const entries = Object.entries(registry).filter(([, value]) => value && typeof value === 'object');
assert.ok(entries.length > 100, 'the model registry must be readable');

const unclassified = entries.filter(([, info]) => capabilityFromModelInfo(info) === 'unknown');
assert.deepEqual(
  unclassified.map(([name]) => name),
  [],
  'every model in the shipped registry must classify to a known surface',
);

const chatModels = entries.filter(([, info]) => capabilityFromModelInfo(info) === 'chat');
assert.ok(chatModels.length > 100, 'the registry chat models must be recognized as chat');

// The bug this refactor closes: a registry_source of huggingface/modelscope used
// to send installed models down the remote-search branch and out as Unknown.
assert.equal(
  capabilityFromModelInfo({ id: 'installed', recipe: 'llamacpp', registry_source: 'huggingface', labels: ['chat'] }),
  'chat',
  'an installed model carrying a registry_source stays classified',
);

// A remote search row genuinely has no deployment label, and Unknown is honest.
assert.equal(capabilityFromModelInfo({ id: 'some/repo', recipe: 'llamacpp', labels: [] }), 'unknown');

console.log(`Model kind classification checks passed (${entries.length} registry models, ${documented.size} labels).`);
