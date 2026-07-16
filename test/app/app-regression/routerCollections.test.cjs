// Router collection builder — app regression tests.
//
// These tests verify that buildRouterCollectionPullRequest produces JSON
// accepted by the CURRENT C++ parser (M9 / routing_policy_parser.cpp).
//
// NL Router (routing.router) and llm classifiers are intentionally skipped:
// the M9 parser explicitly rejects those structures pending #2405.

for (const key of Object.keys(process.env)) {
  if (key.startsWith('npm_') || key === 'INIT_CWD') delete process.env[key];
}

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const repoRoot = path.resolve(__dirname, '..', '..', '..');
const appRoot = path.join(repoRoot, 'src', 'app');
const fixtureDir = path.join(repoRoot, 'test', 'cpp', 'fixtures', 'routing');

// ── TypeScript loader ──────────────────────────────────────────────────────

let ts;
try { ts = require(path.join(appRoot, 'node_modules', 'typescript')); }
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
  path.join(appRoot, 'src', 'renderer', 'utils', 'customCollections.ts'),
);

if (originalTsLoader) require.extensions['.ts'] = originalTsLoader;
else delete require.extensions['.ts'];

// ── Helpers ────────────────────────────────────────────────────────────────

const build = collectionUtils.buildRouterCollectionPullRequest;
const parse = collectionUtils.routingToRouterCollectionDraft;

const fixture = (name) => JSON.parse(fs.readFileSync(path.join(fixtureDir, name), 'utf8'));

const leaf = (signalType, extra = {}) => ({ signalType, ...extra });
const and = (...conditions) => ({ operator: 'AND', conditions });
const or = (...conditions) => ({ operator: 'OR', conditions });
const not = (child) => ({ operator: 'NOT', conditions: [child] });
const rule = (id, routeTo, conditionTree, outputs) => ({
  id, routeTo, conditionTree: conditionTree ?? null, ...(outputs ? { outputs } : {}),
});

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

// ── Tests (exported in the format expected by run-app-regression-tests.cjs) ─

const SKIP_NL_ROUTER = {
  skip: true,
  reason: 'routing.router desugaring is reserved for #2405 and rejected by the M9 parser',
};

const tests = [

  // ── Schema invariants (parser-independent) ─────────────────────────────

  {
    name: 'schema — version field is always the string "1"',
    run() {
      const req = build({
        name: 'T', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules', classifiers: [],
        rules: [rule('r', 'b', leaf('keywords_any', { signalValue: 'hi' }))],
      });
      assert.equal(req.version, '1');
      assert.equal(typeof req.version, 'string');
    },
  },

  {
    name: 'schema — model_name receives user. prefix',
    run() {
      const req = build({
        name: 'MyRouter', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules', classifiers: [],
        rules: [rule('r', 'b', leaf('keywords_any', { signalValue: 'hi' }))],
      });
      assert.ok(req.model_name.startsWith('user.'));
    },
  },

  {
    name: 'schema — recipe is collection.router',
    run() {
      const req = build({
        name: 'T', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules', classifiers: [],
        rules: [rule('r', 'b', leaf('keywords_any', { signalValue: 'hi' }))],
      });
      assert.equal(req.recipe, 'collection.router');
    },
  },

  {
    name: 'schema — default_model must be in candidates (throws)',
    run() {
      assert.throws(() => build({
        name: 'Bad', candidates: ['a'], defaultModel: 'not-a-candidate',
        routingMode: 'rules', classifiers: [],
        rules: [rule('r', 'a', leaf('keywords_any', { signalValue: 'hi' }))],
      }), /default model/i);
    },
  },

  {
    name: 'schema — rules mode with zero rules throws',
    run() {
      assert.throws(() => build({
        name: 'Empty', candidates: ['a'], defaultModel: 'a',
        routingMode: 'rules', classifiers: [], rules: [],
      }), /at least one rule/i);
    },
  },

  {
    name: 'schema — components deduplicated when candidate is also a classifier model',
    run() {
      const req = build({
        name: 'Comp', candidates: ['llm-a', 'llm-b'], defaultModel: 'llm-a',
        routingMode: 'rules',
        classifiers: [{ id: 'e', type: 'semantic_similarity', model: 'embed-model',
          referencePhrases: { topic: ['hello'] } }],
        rules: [rule('r', 'llm-b', leaf('classifier', { classifierId: 'e', minScore: 0.5 }))],
      });
      assert.ok(req.components.includes('embed-model'));
      assert.equal(req.components.filter(c => c === 'embed-model').length, 1);
    },
  },

  // ── NL Router — SKIPPED (M9 parser rejects routing.router) ────────────

  {
    name: 'NL Router — structure matches l0a_llm_router.json [SKIPPED: #2405 not implemented]',
    run() { return SKIP_NL_ROUTER; },
  },

  // ── Single leaf conditions ─────────────────────────────────────────────

  {
    name: 'keywords_any — flat match, no all/any wrapper',
    run() {
      const req = build({
        name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules', classifiers: [],
        rules: [rule('r', 'b', leaf('keywords_any', { signalValue: 'def , function, stack trace' }))],
      });
      assert.deepEqual(req.routing.rules[0].match.keywords_any,
        ['def', 'function', 'stack trace']);
      assert.ok(!('any' in req.routing.rules[0].match));
    },
  },

  {
    name: 'keywords_all — flat match',
    run() {
      const req = build({
        name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules', classifiers: [],
        rules: [rule('r', 'b', leaf('keywords_all', { signalValue: 'foo, bar' }))],
      });
      assert.deepEqual(req.routing.rules[0].match.keywords_all, ['foo', 'bar']);
    },
  },

  {
    name: 'min_chars — emitted as integer',
    run() {
      const req = build({
        name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules', classifiers: [],
        rules: [rule('r', 'b', leaf('min_chars', { signalValue: 4000 }))],
      });
      assert.equal(req.routing.rules[0].match.min_chars, 4000);
      assert.ok(Number.isInteger(req.routing.rules[0].match.min_chars));
    },
  },

  {
    name: 'max_chars — emitted as integer',
    run() {
      const req = build({
        name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules', classifiers: [],
        rules: [rule('r', 'b', leaf('max_chars', { signalValue: 200 }))],
      });
      assert.equal(req.routing.rules[0].match.max_chars, 200);
    },
  },

  {
    name: 'has_tools — emitted as boolean true',
    run() {
      const req = build({
        name: 'R', candidates: ['a'], defaultModel: 'a',
        routingMode: 'rules', classifiers: [],
        rules: [rule('r', 'a', leaf('has_tools'))],
      });
      assert.strictEqual(req.routing.rules[0].match.has_tools, true);
    },
  },

  {
    name: 'has_images — emitted as boolean true',
    run() {
      const req = build({
        name: 'R', candidates: ['a'], defaultModel: 'a',
        routingMode: 'rules', classifiers: [],
        rules: [rule('r', 'a', leaf('has_images'))],
      });
      assert.strictEqual(req.routing.rules[0].match.has_images, true);
    },
  },

  {
    name: 'regex — emitted as string',
    run() {
      const req = build({
        name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules', classifiers: [],
        rules: [rule('r', 'b', leaf('regex', { signalValue: '```[a-z]*' }))],
      });
      assert.equal(req.routing.rules[0].match.regex, '```[a-z]*');
    },
  },

  // ── NOT negation ───────────────────────────────────────────────────────

  {
    name: 'NOT operator — wraps child in { not: ... }',
    run() {
      const req = build({
        name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules', classifiers: [],
        rules: [rule('r', 'b', not(leaf('keywords_any', { signalValue: 'tutorial' })))],
      });
      assert.deepEqual(req.routing.rules[0].match,
        { not: { keywords_any: ['tutorial'] } });
    },
  },

  {
    name: 'leaf.not — flat { not: leaf } via leaf flag',
    run() {
      const req = build({
        name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules', classifiers: [],
        rules: [rule('r', 'b', leaf('keywords_any', { signalValue: 'simple', not: true }))],
      });
      assert.deepEqual(req.routing.rules[0].match,
        { not: { keywords_any: ['simple'] } });
    },
  },

  // ── AND / OR gates ─────────────────────────────────────────────────────

  {
    name: 'AND gate — two children emits { all: [...] }',
    run() {
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
    },
  },

  {
    name: 'OR gate — two children emits { any: [...] }',
    run() {
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
    },
  },

  {
    name: 'nested: (keywords AND min_chars) OR has_images',
    run() {
      const req = build({
        name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules', classifiers: [],
        rules: [rule('r', 'b', or(
          and(
            leaf('keywords_any', { signalValue: 'code' }),
            leaf('min_chars', { signalValue: 500 }),
          ),
          leaf('has_images'),
        ))],
      });
      const match = req.routing.rules[0].match;
      assert.ok(Array.isArray(match.any), 'top level any');
      assert.ok(Array.isArray(match.any[0].all), 'first child is AND');
      assert.ok('has_images' in match.any[1]);
    },
  },

  // ── Classifier — type: "classifier" ───────────────────────────────────

  {
    name: 'classifier — flat match, min_score emitted',
    run() {
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
    },
  },

  {
    name: 'classifier — label and max_score emitted when set',
    run() {
      const req = build({
        name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules',
        classifiers: [{ id: 'c', type: 'classifier', model: 'm', labels: ['A', 'B'] }],
        rules: [rule('r', 'b', leaf('classifier', { classifierId: 'c', label: 'A', minScore: 0.4, maxScore: 0.9 }))],
      });
      const match = req.routing.rules[0].match;
      assert.equal(match.label, 'A');
      assert.equal(match.min_score, 0.4);
      assert.equal(match.max_score, 0.9);
    },
  },

  {
    name: 'classifier — on_error defaults to match_false',
    run() {
      const req = build({
        name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules',
        classifiers: [{ id: 'c', type: 'classifier', model: 'm' }],
        rules: [rule('r', 'b', leaf('classifier', { classifierId: 'c', minScore: 0.5 }))],
      });
      assert.equal(req.routing.classifiers[0].on_error, 'match_false');
    },
  },

  {
    name: 'classifier — model in components list',
    run() {
      const req = build({
        name: 'R', candidates: ['llm-a', 'llm-b'], defaultModel: 'llm-a',
        routingMode: 'rules',
        classifiers: [{ id: 'c', type: 'classifier', model: 'clf-model', labels: ['X'] }],
        rules: [rule('r', 'llm-b', leaf('classifier', { classifierId: 'c', minScore: 0.5 }))],
      });
      assert.ok(req.components.includes('clf-model'));
    },
  },

  {
    name: 'classifier — llm type is SKIPPED (M9 parser rejects)',
    run() {
      return { skip: true, reason: 'llm classifier type reserved for #2405, rejected by M9 parser' };
    },
  },

  // ── Semantic similarity ─────────────────────────────────────────────────

  {
    name: 'semantic_similarity — reference_phrases emitted, model in components',
    run() {
      const req = build({
        name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules',
        classifiers: [{
          id: 'is_coding', type: 'semantic_similarity', model: 'nomic-embed',
          referencePhrases: {
            coding: ['write a function', 'fix this bug'],
            math: ['integral', 'theorem'],
          },
        }],
        rules: [rule('r', 'b', leaf('classifier', { classifierId: 'is_coding', label: 'coding', minScore: 0.75 }))],
      });
      assert.ok(req.components.includes('nomic-embed'));
      const clf = req.routing.classifiers[0];
      assert.equal(clf.type, 'semantic_similarity');
      assert.ok(typeof clf.reference_phrases === 'object' && !Array.isArray(clf.reference_phrases));
      assert.deepEqual(clf.reference_phrases.coding, ['write a function', 'fix this bug']);
      const match = req.routing.rules[0].match;
      assert.equal(match.classifier, 'is_coding');
      assert.equal(match.label, 'coding');
      assert.equal(match.min_score, 0.75);
    },
  },

  {
    name: 'semantic_similarity — default_label and on_error preserved on round-trip',
    run() {
      const req = build({
        name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules',
        classifiers: [{
          id: 's', type: 'semantic_similarity', model: 'embed',
          referencePhrases: { topic: ['hello', 'hi'] },
          defaultLabel: 'topic',
          onError: 'match_true',
        }],
        rules: [rule('r', 'b', leaf('classifier', { classifierId: 's', minScore: 0.6 }))],
      });
      const clf = req.routing.classifiers[0];
      assert.equal(clf.default_label, 'topic');
      assert.equal(clf.on_error, 'match_true');
    },
  },

  {
    name: 'semantic_similarity — labels field must NOT be emitted (server rejects it)',
    run() {
      const req = build({
        name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules',
        classifiers: [{
          id: 's', type: 'semantic_similarity', model: 'embed',
          referencePhrases: { topic: ['hello'] },
        }],
        rules: [rule('r', 'b', leaf('classifier', { classifierId: 's', minScore: 0.5 }))],
      });
      assert.ok(!('labels' in req.routing.classifiers[0]),
        'semantic_similarity classifier must not emit labels key');
    },
  },

  // ── Fixture matching ────────────────────────────────────────────────────

  {
    name: 'fixture: l1_keywords.json — keyword + regex rules match server fixture',
    run() {
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
          rule('long-context-to-big', 'vllm.qwen3-32b',
            leaf('min_chars', { signalValue: 4000 })),
        ],
      });
      assert.equal(req.version, '1');
      assert.deepEqual(req.components.sort(), ['Qwen3-8B-GGUF', 'vllm.qwen3-32b'].sort());
      assertSubset(req.routing, {
        candidates: fix.routing.candidates,
        default_model: fix.routing.default_model,
      });
      const rules = req.routing.rules;
      assert.equal(rules.length, 2);
      assert.equal(rules[0].id, 'code-to-big');
      assert.ok(Array.isArray(rules[0].match.any));
      assert.equal(rules[1].match.min_chars, 4000);
    },
  },

  {
    name: 'fixture: l2_semantic.json — semantic similarity matches server fixture',
    run() {
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
      assert.ok(typeof clf.reference_phrases === 'object');
      const match = req.routing.rules[0].match;
      assert.equal(match.classifier, 'is_coding');
      assert.equal(match.label, 'coding');
      assert.equal(match.min_score, 0.75);
    },
  },

  {
    name: 'fixture: l3_classifier.json — model classifier matches server fixture',
    run() {
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
    },
  },

  // ── Parser round-trips ─────────────────────────────────────────────────

  {
    name: 'round-trip: l1_keywords — conditionTree reconstructed from fixture',
    run() {
      const fix = fixture('l1_keywords.json');
      const draft = parse('user.Router-Keywords', fix.routing, fix.components);
      assert.equal(draft.routingMode, 'rules');
      assert.equal(draft.rules.length, 2);
      assert.ok(draft.rules[0].conditionTree !== null);
      assert.equal(draft.rules[0].id, 'code-to-big');
      const r1 = draft.rules[1];
      assert.ok(r1.conditionTree !== null && 'signalType' in r1.conditionTree);
      assert.equal(r1.conditionTree.signalType, 'min_chars');
      assert.equal(r1.conditionTree.signalValue, 4000);
    },
  },

  {
    name: 'round-trip: build → parse → rebuild produces identical routing block',
    run() {
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
      assert.ok(Array.isArray(rebuilt.routing.rules[0].match.all));
    },
  },

  {
    name: 'round-trip: semantic_similarity preserves default_label and on_error',
    run() {
      const original = {
        name: 'SemRT', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules',
        classifiers: [{
          id: 's', type: 'semantic_similarity', model: 'embed',
          referencePhrases: { topic: ['hello'] },
          defaultLabel: 'topic',
          onError: 'match_true',
        }],
        rules: [rule('r', 'b', leaf('classifier', { classifierId: 's', minScore: 0.6 }))],
      };
      const built = build(original);
      const parsed = parse(built.model_name, built.routing, built.components);
      const rebuilt = build(parsed);
      const clf = rebuilt.routing.classifiers[0];
      assert.equal(clf.default_label, 'topic', 'default_label must survive round-trip');
      assert.equal(clf.on_error, 'match_true', 'on_error must survive round-trip');
    },
  },

  // ── Outputs field ──────────────────────────────────────────────────────

  {
    name: 'outputs — preserved when set',
    run() {
      const req = build({
        name: 'R', candidates: ['a', 'b'], defaultModel: 'a',
        routingMode: 'rules', classifiers: [],
        rules: [rule('r', 'b', leaf('keywords_any', { signalValue: 'ssn' }), { verdict: 'warn' })],
      });
      assert.deepEqual(req.routing.rules[0].outputs, { verdict: 'warn' });
    },
  },

  {
    name: 'outputs — omitted when not set',
    run() {
      const req = build({
        name: 'R', candidates: ['a'], defaultModel: 'a',
        routingMode: 'rules', classifiers: [],
        rules: [rule('r', 'a', leaf('keywords_any', { signalValue: 'hello' }))],
      });
      assert.ok(!('outputs' in req.routing.rules[0]));
    },
  },

];

module.exports = { tests };
