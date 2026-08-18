const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');

function loadTypeScriptModule(filename, dependencyOverrides = {}) {
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
  const localRequire = (request) => Object.prototype.hasOwnProperty.call(dependencyOverrides, request)
    ? dependencyOverrides[request]
    : require(request);
  Function('exports', 'require', 'module', '__filename', '__dirname', compiled.outputText)(
    module.exports,
    localRequire,
    module,
    filename,
    path.dirname(filename),
  );
  return module.exports;
}

function assertTypeScriptSyntax(filename) {
  const source = fs.readFileSync(filename, 'utf8');
  const compiled = ts.transpileModule(source, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS,
      esModuleInterop: true,
      jsx: ts.JsxEmit.ReactJSX,
    },
    fileName: filename,
    reportDiagnostics: true,
  });
  assert.equal(
    (compiled.diagnostics || []).length,
    0,
    (compiled.diagnostics || []).map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'),
  );
}

const routerTypesPath = path.join(root, 'src/features/router/routerTypes.ts');
const managerPath = path.join(root, 'src/components/ModelManager.tsx');
const listPath = path.join(root, 'src/components/ModelListPanel.tsx');
const editorPath = path.join(root, 'src/components/RouterEditorPanel.tsx');
const nodeEditorPath = path.join(root, 'src/components/RouterNodeEditor.tsx');
const graphPath = path.join(root, 'src/components/RouterRuleGraph.tsx');
const graphUtilsPath = path.join(root, 'src/features/router/routerGraph.ts');
const modelPickerPath = path.join(root, 'src/components/RouterModelPicker.tsx');
const workspacePanelsPath = path.join(root, 'src/components/WorkspacePanels.tsx');
const modalPath = path.join(root, 'src/components/inspect/Modal.tsx');
const stylesPath = path.join(root, 'src/styles/styles.css');
const connectionPath = path.join(root, 'src/features/router/routerConnections.ts');
const detailPath = path.join(root, 'src/components/ModelDetailPanel.tsx');
const apiPath = path.join(root, 'src/api.ts');

const router = loadTypeScriptModule(routerTypesPath);
const graphUtils = loadTypeScriptModule(graphUtilsPath, { './routerTypes': router });
const connections = loadTypeScriptModule(connectionPath);
const managerSource = fs.readFileSync(managerPath, 'utf8');
const listSource = fs.readFileSync(listPath, 'utf8');
const editorSource = fs.readFileSync(editorPath, 'utf8');
const nodeEditorSource = fs.readFileSync(nodeEditorPath, 'utf8');
const graphSource = fs.readFileSync(graphPath, 'utf8');
const modelPickerSource = fs.readFileSync(modelPickerPath, 'utf8');
const workspacePanelsSource = fs.readFileSync(workspacePanelsPath, 'utf8');
const modalSource = fs.readFileSync(modalPath, 'utf8');
const stylesSource = fs.readFileSync(stylesPath, 'utf8');
const detailSource = fs.readFileSync(detailPath, 'utf8');
const apiSource = fs.readFileSync(apiPath, 'utf8');

assertTypeScriptSyntax(editorPath);
assertTypeScriptSyntax(graphPath);
assertTypeScriptSyntax(modelPickerPath);
assertTypeScriptSyntax(workspacePanelsPath);
assertTypeScriptSyntax(nodeEditorPath);
assertTypeScriptSyntax(modalPath);
assertTypeScriptSyntax(managerPath);

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
            metadataComparator: 'any', metadataValues: 'quick\nroutine',
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
const duplicateConditionDraft = structuredClone(draft);
duplicateConditionDraft.rules[0].condition = {
  id: 'duplicate-gate',
  kind: 'group',
  operator: 'all',
  children: [
    { ...router.createRouterLeaf('has_tools'), booleanValue: true },
    { ...router.createRouterLeaf('has_tools'), booleanValue: false },
  ],
};
assert.match(
  router.validateRouterDraft(duplicateConditionDraft).join('\n'),
  /duplicate has_tools conditions are not allowed/i,
  'draft validation must reject duplicate direct conditions even when they arrive through import/load rather than graph interaction',
);
const distinctMetadataDraft = structuredClone(draft);
distinctMetadataDraft.rules[0].condition = {
  id: 'metadata-gate-validation',
  kind: 'group',
  operator: 'all',
  children: [
    { ...router.createRouterLeaf('metadata'), metadataKey: 'tier', metadataComparator: 'equals', metadataValues: 'pro' },
    { ...router.createRouterLeaf('metadata'), metadataKey: 'region', metadataComparator: 'equals', metadataValues: 'eu' },
  ],
};
assert.doesNotMatch(
  router.validateRouterDraft(distinctMetadataDraft).join('\n'),
  /duplicate metadata/i,
  'different metadata keys are distinct condition identities and must remain valid in one gate',
);
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

const commaListPayload = {
  version: '1', model_name: 'user.CommaLists', recipe: 'collection.router', components: ['model-a'],
  routing: { candidates: ['model-a'], default_model: 'model-a', rules: [{
    id: 'rule-1',
    match: { all: [
      { keywords_any: ['hello, world', 'plain'] },
      { metadata: { key: 'tags', any: ['red,blue', 'green'] } },
    ] },
    route_to: 'model-a',
  }] },
};
const commaListDraft = router.parseRouterPayload(commaListPayload);
const commaListBuilt = router.buildRouterPullRequest(commaListDraft);
assert.deepEqual(commaListBuilt.routing.rules[0].match.all[0].keywords_any, ['hello, world', 'plain'], 'comma-containing keywords must remain one value through GUI3');
assert.deepEqual(commaListBuilt.routing.rules[0].match.all[1].metadata.any, ['red,blue', 'green'], 'comma-containing metadata tokens must remain one value through GUI3');
const commaClassifierPayload = {
  version: '1', model_name: 'user.CommaClassifier', recipe: 'collection.router',
  components: ['model-a', 'llm-helper', 'embed-helper'],
  routing: { candidates: ['model-a'], default_model: 'model-a', classifiers: [
    { id: 'risk', type: 'llm', model: 'llm-helper', prompt: 'Classify.', labels: ['SAFE,LOCAL', 'RISKY'], default_label: 'SAFE,LOCAL', on_error: 'match_false' },
    { id: 'topic', type: 'semantic_similarity', model: 'embed-helper', reference_phrases: { greeting: ['hello, world', 'hi'] }, on_error: 'match_false' },
  ], rules: [
    { id: 'r1', match: { classifier: 'risk', label: 'SAFE,LOCAL' }, route_to: 'model-a' },
    { id: 'r2', match: { classifier: 'topic', label: 'greeting' }, route_to: 'model-a' },
  ] },
};
const commaClassifierBuilt = router.buildRouterPullRequest(router.parseRouterPayload(commaClassifierPayload));
assert.deepEqual(commaClassifierBuilt.routing.classifiers[0].labels, ['SAFE,LOCAL', 'RISKY'], 'comma-containing classifier labels must round-trip as single labels');
assert.deepEqual(commaClassifierBuilt.routing.classifiers[1].reference_phrases.greeting, ['hello, world', 'hi'], 'comma-containing semantic phrases must round-trip as single phrases');
assert.throws(() => router.parseRouterPayload({ ...commaListPayload, version: 1 }), /version 1 as a string/, "numeric schema versions must not be silently normalized to the server\'s string version");
assert.throws(() => router.parseRouterPayload({
  ...commaListPayload,
  routing: { ...commaListPayload.routing, rules: [{ ...commaListPayload.routing.rules[0], match: { keywords_any: [' padded '] } }] },
}), /leading or trailing whitespace/, 'import must reject list values GUI3 would normalize on save instead of silently changing semantics');

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

const graphRoot = router.createRouterLeaf('has_tools');
const graphWithSibling = graphUtils.appendRouterNodeAtPath(graphRoot, [], router.createRouterLeaf('has_images'));
assert.equal(graphWithSibling.kind, 'group', 'dropping a second leaf on a leaf must create a visual AND gate');
assert.equal(graphWithSibling.operator, 'all');
assert.equal(graphWithSibling.children.length, 2);
const graphNestedGate = graphUtils.createEmptyRouterGraphGroup('any');
const graphWithGate = graphUtils.appendRouterNodeAtPath(graphWithSibling, [], graphNestedGate);
assert.equal(graphWithGate.children.length, 3, 'dropping a gate on a root group must append it as a child');
const graphWithNestedLeaf = graphUtils.appendRouterNodeAtPath(graphWithGate, [2], router.createRouterLeaf('regex'));
assert.equal(graphUtils.routerNodeAtPath(graphWithNestedLeaf, [2, 0]).type, 'regex');
assert.throws(
  () => {
    const notGate = graphUtils.createEmptyRouterGraphGroup('not');
    const withChild = graphUtils.appendRouterNodeAtPath(notGate, [], router.createRouterLeaf('has_tools'));
    graphUtils.appendRouterNodeAtPath(withChild, [], router.createRouterLeaf('has_images'));
  },
  /NOT can contain only one condition/,
  'graph drag/drop must not create an invalid multi-child NOT gate',
);
assert.equal(graphUtils.isBlankRouterGraphNode(router.createRouterLeaf()), true, 'the seeded empty rule should render as an empty graph canvas');
assert.equal(graphUtils.removeRouterNodeAtPath(graphWithSibling, [1]).kind, 'leaf', 'removing a sibling should normalize a one-child gate');
const notWithChild = graphUtils.appendRouterNodeAtPath(graphUtils.createEmptyRouterGraphGroup('not'), [], router.createRouterLeaf('has_tools'));
const emptiedNot = graphUtils.removeRouterNodeAtPath(notWithChild, [0]);
assert.equal(emptiedNot.kind, 'group');
assert.equal(emptiedNot.operator, 'not');
assert.equal(emptiedNot.children.length, 0, 'removing a NOT child should leave a valid graph drop target instead of inserting a hidden placeholder condition');

assert.throws(
  () => graphUtils.appendRouterNodeAtPath(graphWithSibling, [], router.createRouterLeaf('has_tools')),
  /already in this gate/,
  'a graph gate must reject a duplicate deterministic condition type',
);
const classifierGate = {
  id: 'classifier-gate', kind: 'group', operator: 'all', children: [
    { ...router.createRouterLeaf('classifier'), id: 'clf-a', classifierId: 'topic' },
    { ...router.createRouterLeaf('classifier'), id: 'clf-b', classifierId: 'risk' },
  ],
};
assert.equal(
  graphUtils.appendRouterNodeAtPath(classifierGate, [], { ...router.createRouterLeaf('classifier'), classifierId: 'third' }).children.length,
  3,
  'different classifier IDs remain independent conditions even though they share the classifier leaf type',
);
assert.equal(
  graphUtils.appendRouterNodeAtPath(classifierGate, [], { ...router.createRouterLeaf('classifier'), classifierId: 'topic', label: 'code', minScore: 0.9 }).children.length,
  3,
  'classifier leaves are parameterized score/label predicates and may repeat when their constraints differ',
);
const metadataGate = {
  id: 'metadata-gate', kind: 'group', operator: 'any', children: [
    { ...router.createRouterLeaf('metadata'), id: 'meta-a', metadataKey: 'tier', metadataComparator: 'equals', metadataValues: 'pro' },
    { ...router.createRouterLeaf('metadata'), id: 'meta-b', metadataKey: 'region', metadataComparator: 'equals', metadataValues: 'eu' },
  ],
};
assert.equal(
  graphUtils.routerReplacementConflictAtPath(metadataGate, [1], { ...router.createRouterLeaf('metadata'), metadataKey: 'tier', metadataComparator: 'exists' }),
  null,
  'metadata leaves are parameterized predicates and multiple constraints on the same key must remain expressible',
);
const regexGate = {
  id: 'regex-gate', kind: 'group', operator: 'all', children: [
    { ...router.createRouterLeaf('regex'), id: 'regex-a', textValue: '^foo' },
  ],
};
assert.equal(
  graphUtils.appendRouterNodeAtPath(regexGate, [], { ...router.createRouterLeaf('regex'), textValue: 'bar$' }).children.length,
  2,
  'multiple regex predicates are not mergeable and must remain legal in one gate',
);
const duplicateInspectorGroup = {
  id: 'inspector-group', kind: 'group', operator: 'all', children: [
    { ...router.createRouterLeaf('has_tools'), id: 'tools-a' },
    { ...router.createRouterLeaf('has_tools'), id: 'tools-b' },
  ],
};
assert.match(
  graphUtils.routerReplacementConflictAtPath(graphWithSibling, [], duplicateInspectorGroup) || '',
  /has tools condition is already in this gate/,
  'group edits from the inspector must not bypass duplicate-condition protection',
);
const collapseCanCreateDuplicate = {
  id: 'outer', kind: 'group', operator: 'all', children: [
    { ...router.createRouterLeaf('has_tools'), id: 'outer-tools' },
    {
      id: 'inner', kind: 'group', operator: 'all', children: [
        { ...router.createRouterLeaf('has_tools'), id: 'inner-tools' },
        { ...router.createRouterLeaf('has_images'), id: 'inner-images' },
      ],
    },
  ],
};
assert.equal(graphUtils.routerDuplicateConditionConflict(collapseCanCreateDuplicate), null);
const collapsedIntoDuplicate = graphUtils.removeRouterNodeAtPath(collapseCanCreateDuplicate, [1, 1]);
assert.match(
  graphUtils.routerDuplicateConditionConflict(collapsedIntoDuplicate) || '',
  /has tools condition is already in this gate/,
  'normalizing a nested one-child group can surface a duplicate into its parent gate',
);

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

const candidateTransitionDraft = structuredClone(draft);
candidateTransitionDraft.defaultModel = 'user.smart';
candidateTransitionDraft.rules[0].routeTo = 'user.smart';
const removedDefaultCandidate = router.toggleRouterDraftCandidate(candidateTransitionDraft, 'user.smart');
assert.deepEqual(removedDefaultCandidate.candidates, ['user.fast']);
assert.equal(removedDefaultCandidate.defaultModel, 'user.fast', 'removing the default candidate must promote a remaining valid candidate');
assert.equal(removedDefaultCandidate.rules[0].routeTo, 'user.fast', 'rules targeting a removed candidate must be repaired to the new default');
const removedLastCandidate = router.toggleRouterDraftCandidate(removedDefaultCandidate, 'user.fast');
assert.equal(removedLastCandidate.defaultModel, '');
assert.ok(removedLastCandidate.rules.every(rule => rule.routeTo === ''), 'removing the last candidate must not leave dangling rule destinations');


// Self-review: generated node IDs must not make an otherwise unchanged draft dirty.
const fingerprintA = structuredClone(draft);
const fingerprintB = structuredClone(draft);
fingerprintB.rules[0].condition.id = 'different-root-id';
fingerprintB.rules[0].condition.children[0].id = 'different-leaf-id';
assert.equal(router.routerDraftFingerprint(fingerprintA), router.routerDraftFingerprint(fingerprintB));
fingerprintB.rules[0].routeTo = 'user.smart';
assert.notEqual(router.routerDraftFingerprint(fingerprintA), router.routerDraftFingerprint(fingerprintB));

// The server restricts rule IDs but classifier IDs are arbitrary non-empty strings.
const serverCompatibleClassifierId = structuredClone(draft);
serverCompatibleClassifierId.classifiers[0].id = 'risk/topic';
serverCompatibleClassifierId.rules[1].condition.classifierId = 'risk/topic';
assert.deepEqual(router.validateRouterDraft(serverCompatibleClassifierId), []);
assert.equal(router.buildRouterPullRequest(serverCompatibleClassifierId).routing.classifiers[0].id, 'risk/topic');
const invalidRuleId = structuredClone(draft);
invalidRuleId.rules[0].id = 'bad/rule';
assert.match(router.validateRouterDraft(invalidRuleId).join('\n'), /letters, numbers, dot, underscore, and hyphen/);

// Whitespace normalization must keep default labels aligned with emitted labels/concepts.
const normalizedLabelsDraft = structuredClone(llmClassifierDraft);
const riskClassifier = normalizedLabelsDraft.classifiers.find(item => item.id === 'risk');
riskClassifier.labels = [' SAFE ', 'RISKY'];
riskClassifier.defaultLabel = ' SAFE ';
const normalizedLabelsPayload = router.buildRouterPullRequest(normalizedLabelsDraft);
const normalizedRisk = normalizedLabelsPayload.routing.classifiers.find(item => item.id === 'risk');
assert.deepEqual(normalizedRisk.labels, ['SAFE', 'RISKY']);
assert.equal(normalizedRisk.default_label, 'SAFE');

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

// A valid nested singleton gate must survive save/import. If serialization
// collapses the inner ALL, the two keywords_any leaves become direct siblings
// and the saved payload is rejected as a duplicate on its next import.
const singletonGateRoundTripDraft = structuredClone(draft);
singletonGateRoundTripDraft.rules = [{
  id: 'singleton-roundtrip',
  routeTo: 'user.fast',
  outputsText: '',
  condition: {
    id: 'outer-any',
    kind: 'group',
    operator: 'any',
    children: [
      { ...router.createRouterLeaf('keywords_any'), id: 'direct-keywords', textValue: 'foo' },
      {
        id: 'singleton-all',
        kind: 'group',
        operator: 'all',
        children: [
          { ...router.createRouterLeaf('keywords_any'), id: 'nested-keywords', textValue: 'bar' },
        ],
      },
    ],
  },
}];
assert.deepEqual(router.validateRouterDraft(singletonGateRoundTripDraft), []);
const singletonGatePayload = router.buildRouterPullRequest(singletonGateRoundTripDraft);
assert.deepEqual(singletonGatePayload.routing.rules[0].match, {
  any: [
    { keywords_any: ['foo'] },
    { all: [{ keywords_any: ['bar'] }] },
  ],
}, 'save must preserve singleton gates instead of promoting their child into a duplicate parent condition');
const singletonGateParsed = router.parseRouterPayload(singletonGatePayload);
assert.deepEqual(router.validateRouterDraft(singletonGateParsed), []);
assert.deepEqual(
  router.buildRouterPullRequest(singletonGateParsed).routing.rules[0].match,
  singletonGatePayload.routing.rules[0].match,
  'valid router drafts must remain stable across build -> parse -> build',
);
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

// Self-review: imports must fail closed at every schema layer instead of silently
// dropping data that GUI3 cannot round-trip.
assert.throws(
  () => router.parseRouterPayload({ ...implicitAllPayload, future_root: true }),
  /Router JSON contains unsupported field/,
);
assert.throws(
  () => router.parseRouterPayload({ ...implicitAllPayload, models: [{ model_name: 'embedded' }] }),
  /Embedded models are not editable/,
);
assert.throws(
  () => router.routerDraftFromModelInfo({ ...implicitAllPayload, id: implicitAllPayload.model_name, models: [{ model_name: 'embedded' }] }),
  /Embedded models are not editable/,
  'server detail loading must not bypass the embedded-model round-trip guard',
);
assert.throws(
  () => router.parseRouterPayload({ ...implicitAllPayload, components: ['user.fast', 'unused.helper'] }),
  /components do not match editable routing dependencies/,
);
assert.throws(
  () => router.parseRouterPayload({
    ...implicitAllPayload,
    routing: { ...implicitAllPayload.routing, future_routing: true },
  }),
  /Routing block contains unsupported field/,
);
assert.throws(
  () => router.parseRouterPayload({
    ...implicitAllPayload,
    routing: { ...implicitAllPayload.routing, rules: [{ ...implicitAllPayload.routing.rules[0], future_rule: true }] },
  }),
  /Rule 1 contains unsupported field/,
);
const strictClassifierPayload = {
  version: '1', model_name: 'user.strict-classifier', recipe: 'collection.router',
  components: ['user.fast', 'user.classifier'],
  routing: {
    candidates: ['user.fast'], default_model: 'user.fast',
    classifiers: [{ id: 'risk/topic', type: 'classifier', model: 'user.classifier', labels: ['SAFE'], on_error: 'match_false' }],
    rules: [{ id: 'safe-rule', match: { classifier: 'risk/topic', label: 'SAFE' }, route_to: 'user.fast' }],
  },
};
assert.equal(router.parseRouterPayload(strictClassifierPayload).classifiers[0].id, 'risk/topic');
assert.throws(
  () => router.parseRouterPayload({
    ...strictClassifierPayload,
    routing: { ...strictClassifierPayload.routing, classifiers: [{ ...strictClassifierPayload.routing.classifiers[0], on_error: 'sometimes' }] },
  }),
  /on_error must be/,
);
assert.throws(
  () => router.parseRouterPayload({
    ...strictClassifierPayload,
    routing: { ...strictClassifierPayload.routing, classifiers: [{ ...strictClassifierPayload.routing.classifiers[0], future_classifier: true }] },
  }),
  /Classifier 1 contains unsupported field/,
);
assert.throws(
  () => router.parseRouterPayload({
    ...strictClassifierPayload,
    routing: { ...strictClassifierPayload.routing, classifiers: { nope: true } },
  }),
  /routing.classifiers must be an array/,
);
let deepMatch = { has_tools: true };
for (let i = 0; i < router.MAX_ROUTER_TREE_DEPTH + 2; i += 1) deepMatch = { not: deepMatch };
assert.throws(
  () => router.parseRouterPayload({
    ...implicitAllPayload,
    routing: { ...implicitAllPayload.routing, rules: [{ ...implicitAllPayload.routing.rules[0], match: deepMatch }] },
  }),
  /nesting exceeds/,
);


assert.match(listSource, /onOpenRouter/);
assert.match(listSource, /onOpenRouter && \([\s\S]*?icon="router"/);
assert.match(managerSource, /<RouterEditorPanel/);
assert.match(managerSource, /showRouterEditor \?/);
assert.match(managerSource, /await onRegister|handleRegisterRouter/);
assert.match(editorSource, /Save & register/);
assert.match(editorSource, /await onRegister\(nextRequest,\s*submittedDraft\.name\.trim\(\)\)/, 'router save must await server registration with display metadata');
assert.doesNotMatch(editorSource, /upsertRouterRecord|loadRouterRecords|routerRecordToDraft/, 'router editor must not persist definitions in browser storage');
assert.match(editorSource, /Natural-Language Router|Natural-language router/);
assert.match(editorSource, /addClassifier\('llm'\)/);
assert.doesNotMatch(editorSource, /issue #2405|remain hidden/i);
assert.match(nodeEditorSource, /metadataComparator/);
assert.match(nodeEditorSource, /normalizeRouterNode/);
assert.match(nodeEditorSource, />AND<|>AND<\/button>/, 'a leaf must be wrappable into a compound rule');
assert.match(nodeEditorSource, /unusedGroupLeafType/, 'group inspector add/change helpers must seed a condition type that is not already in the gate');
assert.match(editorSource, /Switching modes clears incompatible configuration after confirmation/);
assert.doesNotMatch(editorSource, /window\.confirm/, 'router editor confirmations must use GUI3-native dialogs rather than browser confirm');
assert.match(editorSource, /routerDraftHasRulesProgress\(draft\)[\s\S]*kind: 'switch-llm'/, 'rules-to-LLM mode changes must open a branded warning before clearing meaningful work');
assert.match(editorSource, /routerDraftHasLlmProgress\(draft\)[\s\S]*kind: 'switch-rules'/, 'LLM-to-rules mode changes must open a branded warning before clearing meaningful work');
assert.match(editorSource, /if \(isDirty\)[\s\S]*kind: 'reset'/, 'New must warn whenever it would discard changes relative to the loaded/saved baseline');
assert.match(editorSource, /switchRouterDraftMode\(current, mode\)/, 'confirmed mode changes must use the tested destructive transition helper');
assert.match(editorSource, /<Modal[\s\S]*router-confirm-dialog-title/, 'destructive router actions must render through the GUI3 modal surface');
assert.match(editorSource, /router-editor__toast[\s\S]*role="status"/, 'successful registration feedback must remain visible in a bottom-right status toast');
assert.match(editorSource, /api\.modelDetail\(modelNameValue\)/, 'editing a server router must fetch its full model detail before parsing the policy');
const detailCallIndex = editorSource.indexOf('api.modelDetail(modelNameValue)');
const localFallbackIndex = editorSource.indexOf('if ((initialModel as any).routing)', detailCallIndex);
assert.ok(detailCallIndex >= 0 && localFallbackIndex > detailCallIndex, 'cached local routing may only be used after the authoritative detail request fails');
assert.match(editorSource, /const embeddingModels = useMemo[\s\S]*normalized === 'embedding' \|\| normalized === 'embeddings'/, 'semantic similarity picker must use explicitly labelled embedding models');
assert.match(editorSource, /const classifierModels = useMemo[\s\S]*classification/, 'text classifier picker must only expose classification models');
assert.doesNotMatch(editorSource, /explicit\.length \? explicit : models/, 'embedding picker must not fall back to every model');
assert.match(editorSource, /models=\{classifier\.type === 'semantic_similarity' \? embeddingModels : classifier\.type === 'llm' \? candidateModels : classifierModels\}/, 'classifier pickers must use type-specific model lists');
assert.ok((editorSource.match(/<RouterModelPicker/g) || []).length >= 2, 'NL router and classifier model fields must use searchable model pickers');
assert.match(modelPickerSource, /router-model-picker__search/, 'model picker must expose a search field inside the dropdown');
assert.match(modelPickerSource, /role="combobox"/, 'model picker search must expose combobox semantics');
assert.match(modelPickerSource, /model\.labels/, 'model picker search should match model labels as well as names');
assert.match(modelPickerSource, /createPortal[\s\S]*document\.body/, 'searchable model menus must escape capped list overflow via a portal');
assert.match(editorSource, /<RouterRuleGraph/, 'ordered rules must use the visual GUI3 graph builder');
assert.match(editorSource, /router-editor__rules-workspace[\s\S]*router-editor__rule-list[\s\S]*router-editor__rule-builder/, 'rules must use the GUI2-style compact-list plus selected-canvas pipeline instead of embedding a canvas in every rule');
assert.match(editorSource, /selectedRuleIndex/, 'the visual pipeline must keep one selected rule bound to the graph canvas');
assert.match(graphSource, /application\/x-lemonade-router-node/, 'graph builder must use an isolated drag payload type');
assert.match(graphSource, /draggable[\s\S]*Logic Gates|Logic Gates[\s\S]*draggable/, 'graph toolbox must provide draggable logic gates');
assert.match(graphSource, /onDrop=\{event =>/, 'graph canvas must accept drag/drop input');
assert.match(graphSource, /setPan[\s\S]*setZoom|setZoom[\s\S]*setPan/, 'graph canvas must retain GUI2-style pan and zoom controls');
assert.match(graphSource, /RouterNodeEditor/, 'selected graph nodes must remain fully editable through the existing validated node editor');
assert.match(graphSource, /function exactSummary[\s\S]*JSON\.stringify\(raw\)/, 'graph summaries must expose leading/trailing whitespace when it is part of regex or metadata semantics');
assert.match(graphSource, /className=\{`router-graph__gate[\s\S]*aria-pressed=\{selected\}/, 'graph gates must be keyboard-focusable controls so nested toolbox insertion is a real non-drag alternative');
assert.match(graphSource, /routerReplacementConflictAtPath/, 'node edits must enforce the same duplicate-condition rule as graph drops');
assert.match(graphSource, /routerDuplicateConditionConflict\(next\)/, 'node removal must reject normalization that would create a duplicate condition in the parent gate');
assert.match(editorSource, /router-editor__drag-handle[\s\S]*draggable/, 'ordered rules must be draggable for reordering while keeping keyboard move controls');
assert.match(stylesSource, /router-editor__classifier-list \{ max-height:[\s\S]*router-editor__rule-list \{ max-height:/, 'classifier and rule lists must be height-capped with internal scrolling');
assert.match(stylesSource, /router-editor__classifier-list,[\s\S]*router-editor__rule-list[\s\S]*overflow-y: auto/, 'router lists must scroll internally rather than expanding without bound');
assert.match(stylesSource, /router-editor__rules-workspace[\s\S]*grid-template-columns/, 'rule layout must preserve the compact-list plus graph-canvas split on desktop');

// Review round 2: keep the router surface compact without regressing data safety.
assert.match(workspacePanelsSource, /descriptionPlacement\?: 'body' \| 'identity'/, 'detail panels need an opt-in description placement next to identity metadata');
assert.match(editorSource, /descriptionPlacement="identity"/, 'router description must sit beside the collection.router metadata in the header');
assert.match(workspacePanelsSource, /titleExtras\?: React\.ReactNode/, 'detail panels need a title-row slot for compact editor actions');
assert.match(editorSource, /titleExtras=\{\([\s\S]*router-editor__toolbar/, 'router New/import/export controls must occupy the title-row space freed by the removed generic close button');
assert.doesNotMatch(editorSource, /onClose=\{onClose\}/, 'router detail must not render the redundant generic close icon when a Close action already exists');
assert.doesNotMatch(editorSource, /router-editor__backend-note/, 'redundant router backend explainer must stay removed');
assert.match(editorSource, />JSON Preview</, 'router view labels should use title case');
assert.match(editorSource, /<h3>Candidate Models<\/h3>/);
assert.match(editorSource, /<span>Default Model /);
assert.match(stylesSource, /router-editor__connection-list[\s\S]*max-height:[\s\S]*overflow-y: auto/, 'connected models must show a compact 3-4 row viewport with internal scroll');
assert.match(editorSource, /title="Remove candidate"[\s\S]*toggleCandidate\(connection\.modelName\)|toggleCandidate\(connection\.modelName\)[\s\S]*title="Remove candidate"/, 'candidate connections need a direct remove affordance using the safe candidate transition');
assert.match(editorSource, /Removed \${name}; \${updates\.join\('; '\)}/, 'candidate removal must surface automatic default/rule-target repairs instead of changing routing behavior silently');
assert.match(editorSource, /toggleRouterDraftCandidate\(current, name\)/, 'all candidate removal entry points must share the tested repair transition');
assert.doesNotMatch(editorSource, /External endpoints are provider-wide and affect every model from that provider/, 'provider-wide helper copy was explicitly removed by review');
assert.doesNotMatch(editorSource, /Managed by Lemonade|Local registered model|Routing target/, 'connection rows must not repeat internal/target metadata');
assert.match(stylesSource, /router-editor__connection-trailing[\s\S]*justify-content: flex-end/, 'source badge and backend/provider details must align at the right edge');
assert.match(editorSource, /router-editor__concept-list/, 'semantic concepts need their own capped scrolling viewport');
assert.match(stylesSource, /router-editor__concept-list[\s\S]*max-height:[\s\S]*overflow-y: auto/, 'semantic concepts must not grow the classifier card without bound');
assert.match(editorSource, /jsonCopied \? 'check' : 'copy'/, 'JSON copy control must replace the icon with an in-place success state');
assert.match(editorSource, /jsonCopied \? 'Copied' : 'Copy'/, 'JSON copy control must expose the temporary Copied label');
assert.doesNotMatch(editorSource, /JSON copied\./, 'copy feedback must not use the distant global toast');
assert.match(stylesSource, /router-graph__tool--any \{ color: var\(--success\)/, 'OR gates must be green');
assert.match(stylesSource, /router-graph__tool--not \{ color: var\(--danger\)/, 'NOT gates must be red');
assert.match(stylesSource, /router-graph__tool--all \{ color: var\(--accent-fg\)/, 'AND gates must use the blue/accent color');
assert.match(editorSource, /onExpand=\{\(\) => setExpandedRuleIndex\(selectedRuleIndex\)\}/, 'graph builder needs an explicit expand action');
assert.match(editorSource, /router-graph-expanded-title[\s\S]*router-editor__graph-modal[\s\S]*<RouterRuleGraph/, 'expanded graph editing must render a real editable graph in the GUI3 modal surface');
assert.doesNotMatch(editorSource, /<h3>Natural-language router<\/h3>|<h3>Natural-Language Router<\/h3>/, 'NL mode must not repeat its strategy label as a section heading');
assert.doesNotMatch(editorSource, /The selected model must return exactly one candidate\. Invalid output falls back to the default model\./, 'misleading NL-router helper copy must stay removed');


// Maintainer self-review contracts beyond the explicit review list.
assert.match(graphSource, /lastEmittedNodeRef[\s\S]*historyRef\.current = \[\][\s\S]*futureRef\.current = \[\]/, 'external graph changes must invalidate stale per-instance undo/redo history');
assert.match(graphSource, /wrapRouterNode\(current, 'not'\)/, 'changing a multi-child gate to NOT must wrap the full subtree rather than truncate children');
assert.match(nodeEditorSource, /Negate the complete existing expression[\s\S]*children: \[inner\]/, 'inspector AND\/OR to NOT conversion must preserve the complete existing subtree');
assert.match(graphSource, /toolboxTargetPath = selectedNode\?\.kind === 'group'/, 'keyboard\/touch toolbox clicks must target a selected nested gate rather than only the root');
assert.match(nodeEditorSource, /label: undefined/, 'choosing a classifier should continue following default_label instead of freezing the current default into the rule');
assert.match(nodeEditorSource, /labels\.length > 0 \? 'Select label' : 'Use classifier output'/, 'classifier label picker must not claim a default exists when the classifier has none');
assert.match(graphSource, /labels\.length > 0 \? 'Select label' : 'classifier output'/, 'graph summary must surface a missing classifier label instead of pretending the first label is the default');

const labelLessClassifierDraft = router.createEmptyRouterDraft();
labelLessClassifierDraft.name = 'LabelGuard';
labelLessClassifierDraft.candidates = ['model-a'];
labelLessClassifierDraft.defaultModel = 'model-a';
labelLessClassifierDraft.classifiers = [{
  ...router.createRouterClassifier(0, 'classifier'),
  id: 'dynamic', model: 'classifier-model', labels: [], defaultLabel: undefined,
}];
labelLessClassifierDraft.rules = [{
  ...router.createRouterRule(0, 'model-a'),
  condition: { ...router.createRouterLeaf('classifier'), classifierId: 'dynamic', label: 'UNDECLARED' },
}];
assert.match(router.validateRouterDraft(labelLessClassifierDraft).join(' '), /label .* is not declared by classifier/i, 'explicit classifier labels must be rejected when the classifier declares no labels, matching the server parser');
const regexSafetyDraft = router.createEmptyRouterDraft();
regexSafetyDraft.name = 'RegexSafety';
regexSafetyDraft.candidates = ['model-a'];
regexSafetyDraft.defaultModel = 'model-a';
regexSafetyDraft.rules = [{ ...router.createRouterRule(0, 'model-a'), condition: { ...router.createRouterLeaf('regex'), textValue: '((a+))+' } }];
assert.match(router.validateRouterDraft(regexSafetyDraft).join(' '), /nested unbounded quantifier/, 'GUI3 must catch wrapper-group nested quantifiers rejected by the C++ server');
regexSafetyDraft.rules[0].condition = { ...router.createRouterLeaf('regex'), textValue: '(a+){2,}' };
assert.match(router.validateRouterDraft(regexSafetyDraft).join(' '), /nested unbounded quantifier/, 'GUI3 must catch unbounded brace quantifiers rejected by the C++ server');
regexSafetyDraft.rules[0].condition = { ...router.createRouterLeaf('regex'), textValue: '(a+){2,4}' };
assert.doesNotMatch(router.validateRouterDraft(regexSafetyDraft).join(' '), /nested unbounded quantifier/, 'bounded outer quantifiers should remain allowed, matching the server safeguard');
regexSafetyDraft.rules[0].condition = { ...router.createRouterLeaf('regex'), textValue: ' ^hello $ ' };
assert.equal(router.buildRouterPullRequest(regexSafetyDraft).routing.rules[0].match.regex, ' ^hello $ ', 'regex whitespace is authored semantics and must round-trip exactly');

const exactPromptDraft = router.createEmptyRouterDraft();
exactPromptDraft.name = 'ExactPrompt';
exactPromptDraft.candidates = ['model-a'];
exactPromptDraft.defaultModel = 'model-a';
exactPromptDraft.mode = 'llm';
exactPromptDraft.llmRouter = { model: 'router-model', prompt: '  route deliberately  ' };
assert.equal(router.buildRouterPullRequest(exactPromptDraft).routing.router.prompt, '  route deliberately  ', 'NL router prompt whitespace must not be silently normalized on save');

const exactIdentityPayload = {
  version: '1', model_name: 'user.ExactIdentity', recipe: 'collection.router',
  components: ['model-a', 'classifier-model'],
  routing: {
    candidates: ['model-a'], default_model: 'model-a',
    classifiers: [{ id: ' risk/topic ', type: 'classifier', model: 'classifier-model', labels: ['OK'], default_label: 'OK' }],
    rules: [{ id: 'rule-1', match: { all: [
      { classifier: ' risk/topic ', label: 'OK' },
      { metadata: { key: ' task_class ', equals: 'coding' } },
    ] }, route_to: 'model-a' }],
  },
};
const exactIdentityDraft = router.parseRouterPayload(exactIdentityPayload);
const exactIdentityBuilt = router.buildRouterPullRequest(exactIdentityDraft);
assert.equal(exactIdentityBuilt.routing.classifiers[0].id, ' risk/topic ', 'imported classifier IDs must retain exact server-side identity');
assert.equal(exactIdentityBuilt.routing.rules[0].match.all[0].classifier, ' risk/topic ', 'classifier references must not be trimmed independently from classifier IDs');
assert.equal(exactIdentityBuilt.routing.rules[0].match.all[1].metadata.key, ' task_class ', 'metadata keys are exact strings and must survive import/save without trimming');
assert.match(nodeEditorSource, /Minimum UTF-8 bytes|Maximum UTF-8 bytes/, 'min_chars\/max_chars UI must describe the server\'s UTF-8-byte semantics accurately');
assert.match(nodeEditorSource, /One keyword per line/, 'keyword conditions must use a delimiter that preserves commas inside server-valid keywords');
assert.match(nodeEditorSource, /Values <small>one per line<\/small>/, 'metadata any values must preserve commas by using one value per line');
assert.match(editorSource, /One reference phrase per line/, 'semantic reference phrases must preserve commas instead of using comma-separated authoring');
assert.match(editorSource, /Output Labels <small>one per line<\/small>/, 'classifier labels must preserve commas by using one label per line');
assert.match(editorSource, /CommittedTextInput[\s\S]*commitClassifierId/, 'classifier IDs must commit atomically rather than rewriting references on every keystroke');
assert.match(editorSource, /Classifier \${index \+ 1} ID`\} normalize=\{input => input\}/, 'classifier identity fields must preserve exact whitespace instead of normalizing merely on blur');
assert.match(editorSource, /cancelCommitRef[\s\S]*event\.key === 'Escape'[\s\S]*cancelCommitRef\.current = true/, 'Escape in commit-on-blur identity fields must cancel the pending blur commit rather than save discarded text');
assert.match(editorSource, /const copied = document\.execCommand\('copy'\)[\s\S]*if \(!copied\) throw/, 'clipboard fallback must only report success when the browser accepts the copy operation');
assert.match(editorSource, /Classifier ID .*already in use/, 'classifier rename commits must reject collisions before rewriting rule references');
assert.match(editorSource, /commitSemanticConceptName/, 'semantic concept renames must commit atomically to avoid key-remount and reference corruption');
assert.match(editorSource, /defaultLabel: classifier\.defaultLabel === concept \? undefined : classifier\.defaultLabel/, 'removing an unrelated semantic concept must not clear a still-valid default label');
assert.match(editorSource, /normalized\.includes\(classifier\.defaultLabel\)[\s\S]*classifier\.defaultLabel[\s\S]*undefined/, 'editing output labels must preserve a default label while it remains valid');
assert.doesNotMatch(editorSource, /key=\{`\$\{classifier\.id\}-\$\{index\}`\}/, 'editable classifier IDs must not be used as React keys');
assert.doesNotMatch(editorSource, /key=\{`\$\{concept\}-\$\{conceptIndex\}`\}/, 'editable concept names must not be used as React keys');
assert.match(editorSource, /routerDraftFingerprint\(draft\)[\s\S]*const isDirty/, 'editor must track a semantic dirty baseline that ignores generated graph node IDs');
assert.match(editorSource, /requestClose[\s\S]*Close router editor\?/, 'closing with unsaved changes must require an explicit discard confirmation');
assert.match(editorSource, /Import router JSON\?[\s\S]*discard the unsaved changes/, 'import must not silently replace an edited draft');
assert.match(editorSource, /Load another router\?[\s\S]*discard the unsaved changes/, 'saved-router switching must not silently replace an edited draft');
assert.match(editorSource, /submittedFingerprint[\s\S]*routerDraftFingerprint\(current\) === submittedFingerprint/, 'save completion must preserve edits made while registration is in flight');
assert.match(editorSource, /server registration already succeeded|server registration already succeeded/i, 'local cache failures after a successful server registration must not be reported as registration failures');
assert.match(editorSource, /const \[deleting, setDeleting\][\s\S]*pending\.kind === 'delete'[\s\S]*Keep the modal\/focus trap mounted/, 'router deletion must keep its confirmation modal mounted while the server mutation is in flight');
assert.match(editorSource, /disabled=\{deleting\}[\s\S]*Deleting…/, 'delete confirmation actions must be disabled while deletion is in flight to prevent duplicate mutations');
assert.match(editorSource, /initialModelKeyRef[\s\S]*draftFingerprintRef\.current === baselineAtDetailStart/, 'late model-detail responses must not overwrite edits made while loading');
assert.match(editorSource, /detailParseError[\s\S]*initialModelKeyRef\.current = null|initialModelKeyRef\.current = null[\s\S]*detailParseError/, 'failed detail parsing must clear the load key so a later model refresh can retry');
assert.match(modelPickerSource, /closeAndRestoreFocus[\s\S]*triggerRef\.current\?\.focus\(\)/, 'portaled model picker must restore keyboard focus to its trigger after Escape or selection');
assert.match(modalSource, /const titleId = ariaLabelledBy \|\|/, 'modals must always have an accessible programmatic title even when callers omit an id');
assert.match(modalSource, /previousFocusRef[\s\S]*previous\?\.isConnected[\s\S]*previous\.focus\(\)/, 'modal close must restore focus to the invoking control when possible');
assert.match(modalSource, /document\.activeElement === titleRef\.current[\s\S]*last\.focus\(\)/, 'Shift+Tab from the initially focused modal title must remain trapped inside the dialog');
assert.match(editorSource, /const resetDraft = \(\) => \{[\s\S]*Keep the last processed initial-model key/, 'New must preserve the processed opener key so same-router refreshes cannot silently rehydrate it');
assert.match(editorSource, /baselineFingerprintRef\.current = '__imported-router__';[\s\S]*Preserve the processed initial-model key/, 'Import must preserve the processed opener key so background refreshes cannot replace imported work');
assert.match(managerSource, /Router local cache cleanup failed[\s\S]*setRouterModels\(current => current\.filter/, 'server-side router deletion must remain truthful even if local cache cleanup fails');
assert.match(managerSource, /Lemonade is disconnected so the server definition was not deleted/, 'offline router deletion must not pretend the persistent server definition was removed');
assert.match(editorSource, /deletionWarning[\s\S]*setSavedRecords\(current => current\.filter/, 'a successful server delete with cache-cleanup failure must not immediately resurrect the stale router in the editor dropdown');
assert.match(managerSource, /routerDeleteCandidate[\s\S]*<Modal[\s\S]*Delete router definition\?/, 'router deletion from the model list must use the same branded modal surface instead of native confirm');
assert.doesNotMatch(managerSource, /if \(!confirm\(`Delete router definition/, 'router-specific model-list delete must not regress to a browser confirm dialog');


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

assert.match(editorSource, /Connected Model Topology/, 'router editor must expose connected model sources');
assert.match(editorSource, /Edit Endpoint/, 'external provider endpoints must be editable from the router editor');
assert.match(editorSource, /api\.installCloudProvider/, 'endpoint edits must use the provider registry API');
assert.match(apiSource, /allow_insecure_http: allowInsecureHttp/, 'provider endpoint edits must preserve explicit insecure HTTP policy');
assert.match(detailSource, /Router settings/, 'saved routers need a router-specific settings summary');
assert.match(detailSource, /Connected models/, 'saved routers must show connected model topology');
assert.match(detailSource, /isRouterCollection[\s\S]*collection\.router/, 'router collections must use the persistent settings tab');
assert.match(managerSource, /openRouterEditor\(model\)/, 'editing a saved router must reopen the router editor');



// Self-review: LLM-classifier prompts and connected component identities are
// authored/server-exact strings and must not be trimmed during save/display.
{
  const draft = router.createEmptyRouterDraft();
  draft.name = 'ExactClassifierPrompt';
  draft.candidates = [' candidate/exact '];
  draft.defaultModel = ' candidate/exact ';
  draft.classifiers = [{
    id: 'risk',
    type: 'llm',
    model: ' helper/exact ',
    prompt: '  classify deliberately  ',
    labels: ['SAFE'],
    defaultLabel: 'SAFE',
    referencePhrases: {},
    onError: 'match_false',
  }];
  draft.rules = [{
    id: 'rule-1',
    routeTo: ' candidate/exact ',
    condition: { ...router.createRouterLeaf('classifier'), classifierId: 'risk', label: 'SAFE' },
    outputsText: '',
  }];
  const payload = router.buildRouterPullRequest(draft);
  assert.equal(payload.routing.classifiers[0].prompt, '  classify deliberately  ');
  assert.deepEqual(payload.routing.candidates, [' candidate/exact ']);
  assert(payload.components.includes(' candidate/exact '));
  assert(payload.components.includes(' helper/exact '));
  const rebuilt = router.buildRouterPullRequest(router.parseRouterPayload(payload));
  assert.equal(rebuilt.routing.classifiers[0].prompt, '  classify deliberately  ', 'LLM-classifier prompt whitespace must survive parse/build round-trip');
}
assert.match(editorSource, /Model\/component identifiers are exact server identities/);
assert.match(editorSource, /exact component identity[\s\S]{0,180}modelName: candidate/);
assert.doesNotMatch(editorSource, /const normalized = name\.trim\(\);[\s\S]{0,120}names\.add\(normalized\)/);

console.log('GUI3 router editor contract checks passed.');
