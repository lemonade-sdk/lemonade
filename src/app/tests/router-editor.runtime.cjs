const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');

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
  assert.equal(
    (compiled.diagnostics || []).length,
    0,
    (compiled.diagnostics || []).map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'),
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

const routerTypesPath = path.join(root, 'src/features/router/routerTypes.ts');
const managerPath = path.join(root, 'src/components/ModelManager.tsx');
const listPath = path.join(root, 'src/components/ModelListPanel.tsx');
const editorPath = path.join(root, 'src/components/RouterEditorPanel.tsx');
const nodeEditorPath = path.join(root, 'src/components/RouterNodeEditor.tsx');
const capabilityPath = path.join(root, 'src/modelCapabilities.ts');
const connectionPath = path.join(root, 'src/features/router/routerConnections.ts');
const detailPath = path.join(root, 'src/components/ModelDetailPanel.tsx');
const apiPath = path.join(root, 'src/api.ts');

const router = loadTypeScriptModule(routerTypesPath);
const connections = loadTypeScriptModule(connectionPath);
const managerSource = fs.readFileSync(managerPath, 'utf8');
const listSource = fs.readFileSync(listPath, 'utf8');
const editorSource = fs.readFileSync(editorPath, 'utf8');
const nodeEditorSource = fs.readFileSync(nodeEditorPath, 'utf8');
const capabilitySource = fs.readFileSync(capabilityPath, 'utf8');
const detailSource = fs.readFileSync(detailPath, 'utf8');
const apiSource = fs.readFileSync(apiPath, 'utf8');

const draft = {
  name: 'Fast or smart',
  candidates: ['user.fast', 'user.smart'],
  defaultModel: 'user.smart',
  mode: 'rules',
  llmRouter: { model: '', prompt: '' },
  classifiers: [{
    id: 'topic',
    type: 'semantic_similarity',
    model: 'user.embed',
    prompt: '',
    labels: [],
    referencePhrases: {
      code: ['write code', 'debug this'],
      chat: ['talk to me'],
    },
    defaultLabel: 'chat',
    onError: 'match_false',
  }],
  rules: [
    {
      id: 'fast-tools',
      routeTo: 'user.fast',
      condition: {
        id: 'root',
        kind: 'group',
        operator: 'all',
        children: [
          { id: 'tools', kind: 'leaf', type: 'has_tools', booleanValue: true },
          { id: 'size', kind: 'leaf', type: 'max_chars', numberValue: 1000 },
          {
            id: 'meta', kind: 'leaf', type: 'metadata', metadataKey: 'task_class',
            metadataComparator: 'any', metadataValues: 'quick, routine',
          },
        ],
      },
      outputsText: '{"tier":"fast"}',
    },
    {
      id: 'coding',
      routeTo: 'user.smart',
      condition: {
        id: 'classifier', kind: 'leaf', type: 'classifier', classifierId: 'topic',
        label: 'code', minScore: 0.65, maxScore: 1,
      },
    },
  ],
};

assert.deepEqual(router.validateRouterDraft(draft), []);
const payload = router.buildRouterPullRequest(draft);
assert.equal(payload.version, '1');
assert.equal(payload.recipe, 'collection.router');
assert.equal(payload.model_name, 'user.Fast-or-smart');
assert.deepEqual(payload.components.sort(), ['user.embed', 'user.fast', 'user.smart']);
assert.deepEqual(payload.routing.rules[0].match, {
  all: [
    { has_tools: true },
    { max_chars: 1000 },
    { metadata: { key: 'task_class', any: ['quick', 'routine'] } },
  ],
});
assert.deepEqual(payload.routing.classifiers[0].reference_phrases, draft.classifiers[0].referencePhrases);

const parsed = router.parseRouterPayload(payload);
assert.equal(parsed.rules.length, 2);
assert.equal(parsed.mode, 'rules');
assert.equal(parsed.classifiers[0].type, 'semantic_similarity');
assert.equal(parsed.rules[0].condition.operator, 'all');

const nlPayload = {
  version: '1', model_name: 'user.nl', recipe: 'collection.router', components: ['a', 'router-model'],
  routing: {
    candidates: ['a'],
    default_model: 'a',
    router: { type: 'llm', model: 'router-model', prompt: 'Use a for routine requests.' },
  },
};
const nlDraft = router.parseRouterPayload(nlPayload);
assert.equal(nlDraft.mode, 'llm');
assert.equal(nlDraft.llmRouter.model, 'router-model');
assert.equal(nlDraft.llmRouter.prompt, 'Use a for routine requests.');
assert.deepEqual(router.validateRouterDraft(nlDraft), []);
const rebuiltNl = router.buildRouterPullRequest(nlDraft);
assert.deepEqual(rebuiltNl.routing.router, nlPayload.routing.router);
assert.equal('rules' in rebuiltNl.routing, false);
assert.equal('classifiers' in rebuiltNl.routing, false);
assert.deepEqual(rebuiltNl.components.sort(), ['a', 'router-model']);
assert.throws(
  () => router.parseRouterPayload({ ...nlPayload, routing: { ...nlPayload.routing, router: 'llm' } }),
  /must be an object/,
);
assert.throws(
  () => router.parseRouterPayload({ ...nlPayload, routing: { ...nlPayload.routing, rules: [] } }),
  /cannot be combined/,
);

const llmClassifierDraft = structuredClone(draft);
llmClassifierDraft.classifiers.push({
  id: 'risk', type: 'llm', model: 'user.fast', prompt: 'Choose SAFE or RISKY.',
  labels: ['SAFE', 'RISKY'], defaultLabel: 'SAFE', referencePhrases: {}, onError: 'match_false',
});
llmClassifierDraft.rules.push({
  id: 'risky', routeTo: 'user.smart', outputsText: '',
  condition: { id: 'risk-condition', kind: 'leaf', type: 'classifier', classifierId: 'risk', label: 'RISKY', minScore: 0.5 },
});
assert.deepEqual(router.validateRouterDraft(llmClassifierDraft), []);
const llmClassifierPayload = router.buildRouterPullRequest(llmClassifierDraft);
assert.equal(llmClassifierPayload.routing.classifiers.find(item => item.id === 'risk').prompt, 'Choose SAFE or RISKY.');

const staleLabel = structuredClone(draft);
staleLabel.rules[1].condition.label = 'removed';
assert.ok(router.validateRouterDraft(staleLabel).some(message => message.includes('not declared')));

const badScore = structuredClone(draft);
badScore.rules[1].condition.minScore = 1.2;
assert.ok(router.validateRouterDraft(badScore).some(message => message.includes('[0, 1]')));

const unary = {
  id: 'group', kind: 'group', operator: 'all',
  children: [{ id: 'leaf', kind: 'leaf', type: 'has_tools', booleanValue: true }],
};
assert.equal(router.normalizeRouterNode(unary).kind, 'leaf', 'one-child groups must collapse instead of becoming invalid');

const renamedLabelTree = router.renameClassifierLabelReference(
  draft.rules[1].condition,
  'topic',
  'code',
  'coding',
);
assert.equal(renamedLabelTree.label, 'coding', 'semantic concept renames must update rule references');
assert.equal(router.routerNodeReferencesClassifier(renamedLabelTree, 'topic'), true);

const untouchedEmptyDraft = router.createEmptyRouterDraft();
untouchedEmptyDraft.name = 'Empty';
untouchedEmptyDraft.candidates = ['user.fast'];
untouchedEmptyDraft.defaultModel = 'user.fast';
untouchedEmptyDraft.rules[0].routeTo = 'user.fast';
assert.equal(router.routerDraftHasRulesProgress(untouchedEmptyDraft), false, 'the seeded empty rule is not meaningful progress');
untouchedEmptyDraft.rules[0].condition.textValue = 'code';
assert.equal(router.routerDraftHasRulesProgress(untouchedEmptyDraft), true, 'edited rule content must trigger a switch warning');
const classifierOnlyProgress = router.createEmptyRouterDraft();
classifierOnlyProgress.classifiers.push(router.createRouterClassifier(0, 'classifier'));
assert.equal(router.routerDraftHasRulesProgress(classifierOnlyProgress), true, 'classifier-only work must trigger a switch warning');
assert.equal(router.routerDraftHasLlmProgress({ ...untouchedEmptyDraft, llmRouter: { model: '', prompt: '' } }), false);
assert.equal(router.routerDraftHasLlmProgress({ ...untouchedEmptyDraft, llmRouter: { model: 'user.fast', prompt: '' } }), true);
assert.equal(router.routerDraftHasLlmProgress({ ...untouchedEmptyDraft, llmRouter: { model: '', prompt: 'route carefully' } }), true);

const rulesWithProgress = {
  ...untouchedEmptyDraft,
  rules: [{ ...untouchedEmptyDraft.rules[0], condition: { ...untouchedEmptyDraft.rules[0].condition, textValue: 'code' } }],
  classifiers: [router.createRouterClassifier(0, 'classifier')],
};
const switchedToLlm = router.switchRouterDraftMode(rulesWithProgress, 'llm');
assert.equal(switchedToLlm.mode, 'llm');
assert.deepEqual(switchedToLlm.rules, []);
assert.deepEqual(switchedToLlm.classifiers, []);
const switchedBackToRules = router.switchRouterDraftMode({
  ...switchedToLlm,
  llmRouter: { model: 'user.router', prompt: 'pick a model' },
}, 'rules');
assert.equal(switchedBackToRules.mode, 'rules');
assert.deepEqual(switchedBackToRules.llmRouter, { model: '', prompt: '' });
assert.equal(switchedBackToRules.rules.length, 1);
assert.equal(switchedBackToRules.rules[0].routeTo, switchedBackToRules.defaultModel);

const implicitAllPayload = {
  version: '1',
  model_name: 'user.implicit-all',
  recipe: 'collection.router',
  components: ['user.fast'],
  routing: {
    candidates: ['user.fast'],
    default_model: 'user.fast',
    rules: [{
      id: 'compound-leaf',
      match: { keywords_any: ['code'], has_tools: true },
      route_to: 'user.fast',
    }],
  },
};
const implicitAllDraft = router.parseRouterPayload(implicitAllPayload);
assert.equal(implicitAllDraft.rules[0].condition.kind, 'group');
assert.equal(implicitAllDraft.rules[0].condition.operator, 'all');
assert.equal(implicitAllDraft.rules[0].condition.children.length, 2, 'implicit-all parsing must preserve every leaf condition');
assert.deepEqual(router.buildRouterPullRequest(implicitAllDraft).routing.rules[0].match, {
  all: [{ keywords_any: ['code'] }, { has_tools: true }],
});
assert.throws(
  () => router.parseRouterPayload({
    ...implicitAllPayload,
    routing: { ...implicitAllPayload.routing, rules: [{ ...implicitAllPayload.routing.rules[0], match: { keywords_any: ['code'], future_condition: true } }] },
  }),
  /Unsupported rule condition field/,
  'unknown conditions must fail closed instead of being silently dropped',
);
assert.throws(
  () => router.parseRouterPayload({
    ...implicitAllPayload,
    routing: { ...implicitAllPayload.routing, rules: [{ ...implicitAllPayload.routing.rules[0], match: { all: [{ has_tools: true }], has_images: true } }] },
  }),
  /cannot mix logical operators with leaf conditions/,
  'logical operators must not be normalized together with leaf conditions because the server rejects that shape',
);
assert.throws(
  () => router.parseRouterPayload({
    ...implicitAllPayload,
    routing: { ...implicitAllPayload.routing, rules: [{ ...implicitAllPayload.routing.rules[0], match: { metadata: { key: 'tier', equals: 'pro', exists: true } } }] },
  }),
  /requires exactly one comparator/,
  'metadata must preserve the server requirement of exactly one comparator',
);
assert.throws(
  () => router.parseRouterPayload({
    ...implicitAllPayload,
    routing: { ...implicitAllPayload.routing, rules: [{ ...implicitAllPayload.routing.rules[0], match: { metadata: { key: 'tier', equals: 'pro', future_operator: true } } }] },
  }),
  /Unsupported metadata field/,
  'unknown metadata operators must fail closed instead of being silently dropped',
);

assert.match(listSource, /onOpenRouter/);
assert.match(listSource, /onOpenRouter && \([\s\S]*?icon="router"/);
assert.match(managerSource, /<RouterEditorPanel/);
assert.match(managerSource, /showRouterEditor \?/);
assert.match(managerSource, /await onRegister|handleRegisterRouter/);
assert.match(editorSource, /Save & register/);
assert.match(editorSource, /await onRegister\(nextRequest\)[\s\S]*upsertRouterRecord/, 'local persistence must happen only after server registration succeeds');
assert.match(editorSource, /Natural-language router/);
assert.match(editorSource, /addClassifier\('llm'\)/);
assert.doesNotMatch(editorSource, /issue #2405|remain hidden/i);
assert.match(nodeEditorSource, /metadataComparator/);
assert.match(nodeEditorSource, /normalizeRouterNode/);
assert.match(nodeEditorSource, />AND<|>AND<\/button>/, 'a leaf must be wrappable into a compound rule');
assert.match(capabilitySource, /collection\.router[^\n]+return 'chat'/);
assert.match(editorSource, /Switching modes clears incompatible configuration after confirmation/);
assert.match(editorSource, /routerDraftHasRulesProgress\(draft\)[\s\S]*window\.confirm/, 'rules-to-LLM mode changes must warn before clearing meaningful work');
assert.match(editorSource, /routerDraftHasLlmProgress\(draft\)[\s\S]*window\.confirm/, 'LLM-to-rules mode changes must warn before clearing meaningful work');
assert.match(editorSource, /switchRouterDraftMode\(current, 'llm'\)/, 'switching to LLM must use the tested destructive transition helper');
assert.match(editorSource, /switchRouterDraftMode\(current, 'rules'\)/, 'switching to rules must use the tested destructive transition helper');
assert.match(editorSource, /api\.modelDetail\(modelNameValue\)/, 'editing a server router must fetch its full model detail before parsing the policy');
const detailCallIndex = editorSource.indexOf('api.modelDetail(modelNameValue)');
const localFallbackIndex = editorSource.indexOf('if ((initialModel as any).routing)', detailCallIndex);
assert.ok(detailCallIndex >= 0 && localFallbackIndex > detailCallIndex, 'cached local routing may only be used after the authoritative detail request fails');
assert.match(editorSource, /const embeddingModels = useMemo[\s\S]*normalized === 'embedding' \|\| normalized === 'embeddings'/, 'semantic similarity picker must use explicitly labelled embedding models');
assert.match(editorSource, /const classifierModels = useMemo[\s\S]*classification/, 'text classifier picker must only expose classification models');
assert.doesNotMatch(editorSource, /explicit\.length \? explicit : models/, 'embedding picker must not fall back to every model');
assert.match(editorSource, /classifier\.type === 'llm' \? candidateModels : classifierModels/, 'classifier pickers must use type-specific model lists');


const providerRows = [{
  name: 'fireworks',
  base_url: 'https://api.fireworks.ai/inference/v1',
  env_var: 'LEMONADE_FIREWORKS_API_KEY',
  env_var_set: false,
  runtime_key_set: true,
  models_discovered: 3,
  allow_insecure_http: false,
}];
const connectionModels = [
  { id: 'Qwen3-8B-GGUF', model_name: 'Qwen3-8B-GGUF', recipe: 'llamacpp', display_name: 'Qwen 3 8B' },
  { id: 'fireworks.kimi-k2p5', model_name: 'fireworks.kimi-k2p5', recipe: 'cloud', display_name: 'Kimi K2.5' },
];
const localConnection = connections.describeRouterModelConnection('Qwen3-8B-GGUF', connectionModels, providerRows);
assert.equal(localConnection.kind, 'internal');
assert.equal(localConnection.backend, 'llamacpp');
const cloudConnection = connections.describeRouterModelConnection('fireworks.kimi-k2p5', connectionModels, providerRows);
assert.equal(cloudConnection.kind, 'external');
assert.equal(cloudConnection.provider, 'fireworks');
assert.equal(cloudConnection.endpoint, providerRows[0].base_url);
assert.equal(cloudConnection.authConfigured, true);
assert.equal(connections.validateProviderEndpoint('https://server.example/v1'), null);
assert.equal(connections.validateProviderEndpoint('http://127.0.0.1:8000/v1'), null);
assert.match(connections.validateProviderEndpoint('http://server.example/v1') || '', /explicit insecure HTTP opt-in/);
assert.equal(connections.validateProviderEndpoint('http://server.example/v1', true), null);
assert.match(connections.validateProviderEndpoint('file:///tmp/model') || '', /http/);
assert.match(connections.validateProviderEndpoint('https://user:secret@example.com/v1') || '', /credentials/);

assert.match(editorSource, /Connected model topology/, 'router editor must expose connected model sources');
assert.match(editorSource, /Edit endpoint/, 'external provider endpoints must be editable from the router editor');
assert.match(editorSource, /api\.installCloudProvider/, 'endpoint edits must use the provider registry API');
assert.match(apiSource, /allow_insecure_http: allowInsecureHttp/, 'provider endpoint edits must preserve explicit insecure HTTP policy');
assert.match(detailSource, /Router settings/, 'saved routers need a router-specific settings summary');
assert.match(detailSource, /Connected models/, 'saved routers must show connected model topology');
assert.match(detailSource, /isRouterCollection[\s\S]*collection\.router/, 'router collections must use the persistent settings tab');
assert.match(managerSource, /openRouterEditor\(model\)/, 'editing a saved router must reopen the router editor');

console.log('GUI3 router editor contract checks passed.');
