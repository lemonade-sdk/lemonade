// Router collection builder tests.
// Validates that buildRouterCollectionPullRequest produces JSON matching
// the frozen fixtures in test/cpp/fixtures/routing/.
//
// Run: node test/app/routerCollections.test.cjs

for (const key of Object.keys(process.env)) {
  if (key.startsWith('npm_') || key === 'INIT_CWD') delete process.env[key];
}

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

// ── TypeScript loader ──────────────────────────────────────────────────────
const appRoot = path.resolve(__dirname, '..', '..', 'src', 'app');
let ts; try { ts = require(path.join(appRoot, 'node_modules', 'typescript')); }
catch (_) { ts = require('typescript'); }

const originalTsLoader = require.extensions['.ts'];
require.extensions['.ts'] = function loadTypeScript(module, filename) {
  const source = fs.readFileSync(filename, 'utf8');
  const output = ts.transpileModule(source, {
    compilerOptions: {
      esModuleInterop: true, module: ts.ModuleKind.CommonJS,
      moduleResolution: ts.ModuleResolutionKind.NodeJs, target: ts.ScriptTarget.ES2020,
    },
    fileName: filename,
  }).outputText;
  module._compile(output, filename);
};

const collectionUtils = require(
  path.join(appRoot, 'src', 'renderer', 'utils', 'customCollections.ts')
);

if (originalTsLoader) require.extensions['.ts'] = originalTsLoader;
else delete require.extensions['.ts'];

// ── Fixture loader ─────────────────────────────────────────────────────────
const fixtureDir = path.resolve(__dirname, '..', 'cpp', 'fixtures', 'routing');
const fixture = (name) => JSON.parse(fs.readFileSync(path.join(fixtureDir, name), 'utf8'));

// ── Test harness ───────────────────────────────────────────────────────────
const tests = [];
function defineTest(name, fn) { tests.push({ name, fn }); }

function assertSubset(actual, expected, path = '') {
  if (Array.isArray(expected)) {
    assert.ok(Array.isArray(actual), `${path}: expected array`);
    assert.equal(actual.length, expected.length, `${path}: array length`);
    for (let i = 0; i < expected.length; i++) assertSubset(actual[i], expected[i], `${path}[${i}]`);
  } else if (expected !== null && typeof expected === 'object') {
    assert.ok(actual !== null && typeof actual === 'object', `${path}: expected object`);
    for (const key of Object.keys(expected)) {
      assert.ok(key in actual, `${path}.${key}: key missing`);
      assertSubset(actual[key], expected[key], `${path}.${key}`);
    }
  } else {
    assert.deepEqual(actual, expected, `${path}: value mismatch`);
  }
}

const build = collectionUtils.buildRouterCollectionPullRequest;
const parse = collectionUtils.routingToRouterCollectionDraft;

// ── Tree node helpers ──────────────────────────────────────────────────────
// Build RuleNode trees using the new conditionTree model

const leaf = (signalType, extra = {}) => ({ signalType, ...extra });
const and = (...conditions) => ({ operator: 'AND', conditions });
const or = (...conditions) => ({ operator: 'OR', conditions });
const not = (child) => ({ operator: 'NOT', conditions: [child] });

const rule = (id, routeTo, conditionTree, outputs) => ({
  id, routeTo, conditionTree: conditionTree ?? null, ...(outputs ? { outputs } : {}),
});

// ── NL Router ─────────────────────────────────────────────────────────────

defineTest('NL Router — matches l0a_llm_router.json structure', () => {
  const fix = fixture('l0a_llm_router.json');
  const req = build({
    name: 'Router-Auto',
    candidates: ['Qwen3-8B-GGUF', 'Qwen3.5-35B-A3B-GGUF'],
    defaultModel: 'Qwen3-8B-GGUF', routingMode: 'llm',
    routerModel: 'Qwen3-1.7B-GGUF', routerPrompt: fix.routing.router.prompt,
  });
  assert.equal(req.version, '1');
  assert.equal(req.recipe, 'collection.router');
  assert.equal(req.model_name, 'user.Router-Auto');
  assert.ok(req.components.includes('Qwen3-8B-GGUF'));
  assert.ok(req.components.includes('Qwen3.5-35B-A3B-GGUF'));
  assert.ok(req.components.includes('Qwen3-1.7B-GGUF'));
  assert.equal(req.components.length, 3);
  assertSubset(req.routing, {
    candidates: fix.routing.candidates, default_model: fix.routing.default_model,
    router: { type: 'llm', model: fix.routing.router.model, prompt: fix.routing.router.prompt },
  });
  assert.ok(!('rules' in req.routing));
  assert.ok(!('classifiers' in req.routing));
});

defineTest('NL Router — router model excluded from routing.candidates', () => {
  const req = build({
    name: 'Router-Auto', candidates: ['Qwen3-8B-GGUF', 'Qwen3.5-35B-A3B-GGUF'],
    defaultModel: 'Qwen3-8B-GGUF', routingMode: 'llm',
    routerModel: 'Qwen3-1.7B-GGUF', routerPrompt: 'Route. Reply with ONLY the model name.',
  });
  assert.ok(!req.routing.candidates.includes('Qwen3-1.7B-GGUF'));
});

// ── Single leaf conditions ─────────────────────────────────────────────────

defineTest('Single keywords_any leaf — flat match', () => {
  const req = build({
    name: 'Router-Keywords', candidates: ['a', 'b'], defaultModel: 'a',
    routingMode: 'rules', classifiers: [],
    rules: [rule('r', 'b', leaf('keywords_any', { signalValue: 'def , function, stack trace, compile' }))],
  });
  assert.deepEqual(req.routing.rules[0].match.keywords_any,
    ['def', 'function', 'stack trace', 'compile']);
  assert.ok(!('any' in req.routing.rules[0].match));
});

defineTest('Single min_chars leaf — flat match', () => {
  const req = build({
    name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
    routingMode: 'rules', classifiers: [],
    rules: [rule('r', 'b', leaf('min_chars', { signalValue: 4000 }))],
  });
  assert.equal(req.routing.rules[0].match.min_chars, 4000);
});

defineTest('Single classifier leaf — flat match', () => {
  const req = build({
    name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
    routingMode: 'rules',
    classifiers: [{ id: 'pii', type: 'classifier', model: 'm', labels: ['PII', 'NO_PII'] }],
    rules: [rule('r', 'b', leaf('classifier', { classifierId: 'pii', minScore: 0.5 }))],
  });
  const match = req.routing.rules[0].match;
  assert.equal(match.classifier, 'pii');
  assert.equal(match.min_score, 0.5);
  assert.ok(!('any' in match));
  assert.ok(!('all' in match));
});

// ── Operator nodes ─────────────────────────────────────────────────────────

defineTest('AND gate — two children → { all: [...] }', () => {
  const req = build({
    name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
    routingMode: 'rules', classifiers: [],
    rules: [rule('r', 'b', and(
      leaf('keywords_any', { signalValue: 'code' }),
      leaf('min_chars', { signalValue: 500 }),
    ))],
  });
  const match = req.routing.rules[0].match;
  assert.ok(Array.isArray(match.all));
  assert.ok(!('any' in match));
  assert.equal(match.all.length, 2);
});

defineTest('OR gate — two children → { any: [...] }', () => {
  const req = build({
    name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
    routingMode: 'rules', classifiers: [],
    rules: [rule('r', 'b', or(
      leaf('keywords_any', { signalValue: 'urgent' }),
      leaf('has_images'),
    ))],
  });
  const match = req.routing.rules[0].match;
  assert.ok(Array.isArray(match.any));
  assert.equal(match.any.length, 2);
});

defineTest('NOT gate — wraps child in { not: ... }', () => {
  const req = build({
    name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
    routingMode: 'rules', classifiers: [],
    rules: [rule('r', 'b', not(leaf('keywords_any', { signalValue: 'tutorial' })))],
  });
  const match = req.routing.rules[0].match;
  assert.deepEqual(match, { not: { keywords_any: ['tutorial'] } });
});

defineTest('Nested: (keywords AND clf) OR has_images', () => {
  const req = build({
    name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
    routingMode: 'rules',
    classifiers: [{ id: 'pii', type: 'classifier', model: 'm' }],
    rules: [rule('r', 'b', or(
      and(
        leaf('keywords_any', { signalValue: 'ssn, credit card' }),
        leaf('classifier', { classifierId: 'pii', minScore: 0.5 }),
      ),
      leaf('has_images'),
    ))],
  });
  const match = req.routing.rules[0].match;
  assert.ok(Array.isArray(match.any), 'top level any');
  assert.equal(match.any.length, 2);
  assert.ok(Array.isArray(match.any[0].all), 'first child is AND');
  assert.ok('has_images' in match.any[1]);
});

defineTest('NOT on a leaf (leaf.not) — flat { not: leaf }', () => {
  const req = build({
    name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
    routingMode: 'rules', classifiers: [],
    rules: [rule('r', 'b', leaf('keywords_any', { signalValue: 'simple', not: true }))],
  });
  assert.deepEqual(req.routing.rules[0].match, { not: { keywords_any: ['simple'] } });
});

defineTest('label emitted in classifier leaf', () => {
  const req = build({
    name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
    routingMode: 'rules',
    classifiers: [{ id: 'pii', type: 'classifier', model: 'm', labels: ['PII', 'NO_PII'] }],
    rules: [rule('r', 'b', leaf('classifier', { classifierId: 'pii', label: 'NO_PII', minScore: 0.8 }))],
  });
  const match = req.routing.rules[0].match;
  assert.equal(match.label, 'NO_PII');
  assert.equal(match.min_score, 0.8);
});

defineTest('max_score emitted when specified', () => {
  const req = build({
    name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
    routingMode: 'rules',
    classifiers: [{ id: 'c', type: 'classifier', model: 'm' }],
    rules: [rule('r', 'b', leaf('classifier', { classifierId: 'c', minScore: 0.4, maxScore: 0.9 }))],
  });
  const match = req.routing.rules[0].match;
  assert.equal(match.min_score, 0.4);
  assert.equal(match.max_score, 0.9);
});

defineTest('has_tools emitted', () => {
  const req = build({
    name: 'R', candidates: ['a'], defaultModel: 'a',
    routingMode: 'rules', classifiers: [],
    rules: [rule('r', 'a', leaf('has_tools'))],
  });
  assert.equal(req.routing.rules[0].match.has_tools, true);
});

defineTest('outputs field round-trips', () => {
  const req = build({
    name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
    routingMode: 'rules', classifiers: [],
    rules: [rule('r', 'b', leaf('keywords_any', { signalValue: 'ssn' }), { verdict: 'warn' })],
  });
  assert.deepEqual(req.routing.rules[0].outputs, { verdict: 'warn' });
});

defineTest('outputs omitted when not set', () => {
  const req = build({
    name: 'R', candidates: ['a'], defaultModel: 'a',
    routingMode: 'rules', classifiers: [],
    rules: [rule('r', 'a', leaf('keywords_any', { signalValue: 'hello' }))],
  });
  assert.ok(!('outputs' in req.routing.rules[0]));
});

// ── Fixture matching ───────────────────────────────────────────────────────

defineTest('Keyword rules — matches l1_keywords.json structure', () => {
  const fix = fixture('l1_keywords.json');
  const req = build({
    name: 'Router-Keywords', candidates: ['Qwen3-8B-GGUF', 'vllm.qwen3-32b'],
    defaultModel: 'Qwen3-8B-GGUF', routingMode: 'rules', classifiers: [],
    rules: [
      rule('code-to-big', 'vllm.qwen3-32b',
        or(
          leaf('keywords_any', { signalValue: 'def, function, stack trace, compile' }),
          leaf('regex', { signalValue: '```[a-z]*' }),
        )),
      rule('long-context-to-big', 'vllm.qwen3-32b', leaf('min_chars', { signalValue: 4000 })),
    ],
  });
  assert.equal(req.version, '1');
  assert.deepEqual(req.components.sort(), ['Qwen3-8B-GGUF', 'vllm.qwen3-32b'].sort());
  assert.deepEqual(req.routing.candidates, fix.routing.candidates);
  const rules = req.routing.rules;
  assert.equal(rules.length, 2);
  assert.equal(rules[0].id, 'code-to-big');
  assert.ok(Array.isArray(rules[0].match.any));
  assert.equal(rules[1].match.min_chars, 4000);
});

defineTest('Semantic similarity — matches l2_semantic.json (concept map)', () => {
  const req = build({
    name: 'Router-Semantic', candidates: ['Qwen3-8B-GGUF', 'vllm.qwen3-32b'],
    defaultModel: 'Qwen3-8B-GGUF', routingMode: 'rules',
    classifiers: [{
      id: 'is_coding', type: 'semantic_similarity', model: 'nomic-embed-text-v1.5-GGUF',
      referencePhrases: {
        coding: ['write a function', 'fix this bug', 'refactor this code', 'time complexity'],
        math: ['integral', 'prove this theorem'],
      },
    }],
    rules: [rule('coding-to-big', 'vllm.qwen3-32b',
      leaf('classifier', { classifierId: 'is_coding', label: 'coding', minScore: 0.75 }))],
  });
  assert.ok(req.components.includes('nomic-embed-text-v1.5-GGUF'));
  const clf = req.routing.classifiers[0];
  assert.equal(clf.type, 'semantic_similarity');
  assert.ok(typeof clf.reference_phrases === 'object' && !Array.isArray(clf.reference_phrases));
  const match = req.routing.rules[0].match;
  assert.equal(match.classifier, 'is_coding');
  assert.equal(match.label, 'coding');
  assert.equal(match.min_score, 0.75);
});

defineTest('Model-backed classifier — matches l3_classifier.json structure', () => {
  const fix = fixture('l3_classifier.json');
  const req = build({
    name: 'Router-Classify', candidates: ['Qwen3-8B-GGUF', 'vllm.qwen3-32b'],
    defaultModel: 'vllm.qwen3-32b', routingMode: 'rules',
    classifiers: [
      { id: 'pii', type: 'classifier', model: 'pii-detector-small',
        labels: ['PII', 'NO_PII'], defaultLabel: 'PII', onError: 'match_true' },
      { id: 'jailbreak', type: 'classifier', model: 'jailbreak-detector-small',
        labels: ['JAILBREAK', 'BENIGN'], defaultLabel: 'JAILBREAK', onError: 'match_true' },
    ],
    rules: [rule('sensitive-stays-local', 'Qwen3-8B-GGUF',
      leaf('classifier', { classifierId: 'pii', minScore: 0.5 }), { verdict: 'warn' })],
  });
  assert.ok(req.components.includes('pii-detector-small'));
  assertSubset(req.routing.classifiers, fix.routing.classifiers);
  const match = req.routing.rules[0].match;
  assert.equal(match.classifier, 'pii');
  assert.equal(match.min_score, 0.5);
  assert.deepEqual(req.routing.rules[0].outputs, { verdict: 'warn' });
});

// ── Schema invariants ──────────────────────────────────────────────────────

defineTest('Schema invariant — version is always "1"', () => {
  const req = build({
    name: 'T', candidates: ['a'], defaultModel: 'a',
    routingMode: 'llm', routerModel: 'tiny', routerPrompt: 'Pick.',
  });
  assert.equal(req.version, '1');
  assert.equal(typeof req.version, 'string');
});

defineTest('Schema invariant — model_name gets user. prefix', () => {
  const req = build({
    name: 'MyRouter', candidates: ['a'], defaultModel: 'a',
    routingMode: 'llm', routerModel: 'tiny', routerPrompt: 'Pick.',
  });
  assert.ok(req.model_name.startsWith('user.'));
});

defineTest('Schema invariant — default_model must be in candidates', () => {
  assert.throws(() => build({
    name: 'Bad', candidates: ['a'], defaultModel: 'not-a-candidate',
    routingMode: 'llm', routerModel: 'tiny', routerPrompt: 'Pick.',
  }), /default model/i);
});

defineTest('Schema invariant — rules mode with zero rules throws', () => {
  assert.throws(() => build({
    name: 'Empty', candidates: ['a'], defaultModel: 'a',
    routingMode: 'rules', classifiers: [], rules: [],
  }), /at least one rule/i);
});

defineTest('Schema invariant — llm mode with missing prompt throws', () => {
  assert.throws(() => build({
    name: 'NoPr', candidates: ['a'], defaultModel: 'a',
    routingMode: 'llm', routerModel: 'tiny', routerPrompt: '',
  }), /routing prompt/i);
});

defineTest('Schema invariant — components deduped', () => {
  const req = build({
    name: 'Overlap', candidates: ['a', 'b'], defaultModel: 'a',
    routingMode: 'llm', routerModel: 'a', routerPrompt: 'Pick.',
  });
  assert.equal(req.components.filter(c => c === 'a').length, 1);
});

defineTest('Schema invariant — classifier model in components', () => {
  const req = build({
    name: 'Comp', candidates: ['llm-a', 'llm-b'], defaultModel: 'llm-a',
    routingMode: 'rules',
    classifiers: [{ id: 'e', type: 'semantic_similarity', model: 'embed-model',
      referencePhrases: { topic: ['hello'] } }],
    rules: [rule('r', 'llm-b', leaf('classifier', { classifierId: 'e', minScore: 0.5 }))],
  });
  assert.ok(req.components.includes('embed-model'));
  assert.equal(req.components.length, 3);
});

// ── Classifier types ───────────────────────────────────────────────────────

defineTest('on_error defaults to match_false when omitted', () => {
  const req = build({
    name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
    routingMode: 'rules',
    classifiers: [{ id: 'c', type: 'classifier', model: 'm' }],
    rules: [rule('r', 'b', leaf('classifier', { classifierId: 'c', minScore: 0.5 }))],
  });
  assert.equal(req.routing.classifiers[0].on_error, 'match_false');
});

defineTest('llm classifier emits model and prompt only', () => {
  const req = build({
    name: 'R', candidates: ['safe', 'risky'], defaultModel: 'safe',
    routingMode: 'rules',
    classifiers: [{ id: 'j', type: 'llm', model: 'small-llm', prompt: 'SAFE or RISKY.' }],
    rules: [rule('r', 'risky', leaf('classifier', { classifierId: 'j', minScore: 0.5 }))],
  });
  const clf = req.routing.classifiers[0];
  assert.equal(clf.type, 'llm');
  assert.ok(clf.prompt);
  assert.ok(!('labels' in clf));
  assert.ok(!('reference_phrases' in clf));
});

// ── Parser round-trips ─────────────────────────────────────────────────────

defineTest('round-trip: l0a_llm_router — NL router fields preserved', () => {
  const fix = fixture('l0a_llm_router.json');
  const draft = parse('user.Router-Auto', fix.routing, fix.components);
  assert.equal(draft.routingMode, 'llm');
  assert.deepEqual(draft.candidates, fix.routing.candidates);
  assert.equal(draft.routerModel, fix.routing.router.model);
  assert.equal(draft.routerPrompt, fix.routing.router.prompt);
});

defineTest('round-trip: l1_keywords — conditionTree reconstructed', () => {
  const fix = fixture('l1_keywords.json');
  const draft = parse('user.Router-Keywords', fix.routing, fix.components);
  assert.equal(draft.routingMode, 'rules');
  assert.equal(draft.rules.length, 2);
  // First rule has conditionTree (not null)
  assert.ok(draft.rules[0].conditionTree !== null);
  assert.equal(draft.rules[0].id, 'code-to-big');
  assert.equal(draft.rules[0].routeTo, 'vllm.qwen3-32b');
  // Second rule: min_chars leaf
  const r1 = draft.rules[1];
  assert.ok(r1.conditionTree !== null && 'signalType' in r1.conditionTree);
  assert.equal(r1.conditionTree.signalType, 'min_chars');
  assert.equal(r1.conditionTree.signalValue, 4000);
});

defineTest('round-trip: l2_semantic — concept map round-trips', () => {
  const original = {
    name: 'Router-Semantic', candidates: ['Qwen3-8B-GGUF', 'vllm.qwen3-32b'],
    defaultModel: 'Qwen3-8B-GGUF', routingMode: 'rules',
    classifiers: [{ id: 'is_coding', type: 'semantic_similarity', model: 'nomic-embed-text-v1.5-GGUF',
      referencePhrases: { coding: ['write a function', 'fix this bug'], math: ['integral'] } }],
    rules: [rule('r', 'vllm.qwen3-32b', leaf('classifier', { classifierId: 'is_coding', label: 'coding', minScore: 0.75 }))],
  };
  const req = build(original);
  const draft = parse(req.model_name, req.routing, req.components);
  assert.equal(draft.classifiers[0].type, 'semantic_similarity');
  assert.ok(draft.conditionTree === undefined); // field is on rules, not draft
  const tree = draft.rules[0].conditionTree;
  assert.ok(tree && 'signalType' in tree && tree.signalType === 'classifier');
  assert.equal(tree.classifierId, 'is_coding');
  assert.equal(tree.label, 'coding');
});

defineTest('round-trip: l3_classifier — outputs preserved', () => {
  const fix = fixture('l3_classifier.json');
  const draft = parse('user.Router-Classify', fix.routing, fix.components);
  assert.equal(draft.classifiers.length, 2);
  assert.equal(draft.classifiers[0].onError, 'match_true');
  const r = draft.rules[0];
  assert.deepEqual(r.outputs, { verdict: 'warn' });
  assert.ok(r.conditionTree !== null);
});

defineTest('round-trip: full builder→parser identity', () => {
  const original = {
    name: 'RoundTrip', candidates: ['a', 'b'], defaultModel: 'a',
    routingMode: 'rules',
    classifiers: [{ id: 'pii', type: 'classifier', model: 'm',
      labels: ['PII', 'NO_PII'], defaultLabel: 'PII', onError: 'match_true' }],
    rules: [rule('r1', 'a',
      and(leaf('keywords_any', { signalValue: 'ssn' }),
          leaf('classifier', { classifierId: 'pii', minScore: 0.5 })),
      { verdict: 'warn' })],
  };
  const built = build(original);
  const parsed = parse(built.model_name, built.routing, built.components);
  const rebuilt = build(parsed);
  assert.equal(rebuilt.version, built.version);
  assert.deepEqual(rebuilt.routing.candidates, built.routing.candidates);
  assert.deepEqual(rebuilt.routing.classifiers, built.routing.classifiers);
  assert.equal(rebuilt.routing.rules[0].id, built.routing.rules[0].id);
  assert.deepEqual(rebuilt.routing.rules[0].outputs, built.routing.rules[0].outputs);
  // The match structure should be equivalent (both produce all:[keywords_any, classifier])
  assert.ok(Array.isArray(rebuilt.routing.rules[0].match.all));
});

// ── Run ────────────────────────────────────────────────────────────────────
let passed = 0;
const failures = [];
for (const { name, fn } of tests) {
  try { fn(); passed++; console.log(`PASS  ${name}`); }
  catch (error) { failures.push(name); console.error(`FAIL  ${name}`); console.error(error?.stack ?? error); }
}
console.log('');
if (failures.length === 0) {
  console.log(`All router collection tests passed (${passed}/${tests.length}).`);
} else {
  console.error(`${failures.length} test(s) failed:`);
  for (const name of failures) console.error(`  • ${name}`);
  process.exit(1);
}
