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

const router = loadTypeScriptModule(routerTypesPath);
const managerSource = fs.readFileSync(managerPath, 'utf8');
const listSource = fs.readFileSync(listPath, 'utf8');
const editorSource = fs.readFileSync(editorPath, 'utf8');
const nodeEditorSource = fs.readFileSync(nodeEditorPath, 'utf8');
const capabilitySource = fs.readFileSync(capabilityPath, 'utf8');

const draft = {
  name: 'Fast or smart',
  candidates: ['user.fast', 'user.smart'],
  defaultModel: 'user.smart',
  classifiers: [{
    id: 'topic',
    type: 'semantic_similarity',
    model: 'user.embed',
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
assert.equal(parsed.classifiers[0].type, 'semantic_similarity');
assert.equal(parsed.rules[0].condition.operator, 'all');

assert.throws(() => router.parseRouterPayload({
  version: '1', model_name: 'user.bad', recipe: 'collection.router', components: ['a'],
  routing: { candidates: ['a'], default_model: 'a', router: { type: 'llm', model: 'a', prompt: 'route' } },
}), /not supported/i);

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

assert.match(listSource, /onOpenRouter/);
assert.match(listSource, /onOpenRouter && \([\s\S]*?icon="router"/);
assert.match(managerSource, /<RouterEditorPanel/);
assert.match(managerSource, /showRouterEditor \?/);
assert.match(managerSource, /await onRegister|handleRegisterRouter/);
assert.match(editorSource, /Save & register/);
assert.match(editorSource, /await onRegister\(nextRequest\)[\s\S]*upsertRouterRecord/, 'local persistence must happen only after server registration succeeds');
assert.match(editorSource, /NL Router and LLM classifiers remain hidden/);
assert.match(nodeEditorSource, /metadataComparator/);
assert.match(nodeEditorSource, /normalizeRouterNode/);
assert.match(nodeEditorSource, />AND<|>AND<\/button>/, 'a leaf must be wrappable into a compound rule');
assert.match(capabilitySource, /collection\.router[^\n]+return 'chat'/);
assert.doesNotMatch(editorSource, /routingMode|quick rules|advanced rules/i, 'one lossless rule model replaces destructive mode conversion');

console.log('GUI3 router editor contract checks passed.');
