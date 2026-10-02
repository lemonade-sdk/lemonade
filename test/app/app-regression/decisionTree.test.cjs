// Decision-tree summarization - app regression tests.
//
// Guards summarizeMatchExpr() in decisionTree.tsx. The routing parser permits
// multiple deterministic conditions in a single leaf object (e.g.
// {"min_chars": 4000, "has_tools": true}), compiled as an implicit `all` -
// see make_leaf_factory in src/cpp/server/routing_policy.cpp. The tree,
// tooltip, and accessible label must render every condition in a leaf, not
// just the first key, since all of them jointly decide the match.

for (const key of Object.keys(process.env)) {
  if (key.startsWith('npm_') || key === 'INIT_CWD') delete process.env[key];
}

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const Module = require('node:module');

const repoRoot = path.resolve(__dirname, '..', '..', '..');
const appRoot = path.join(repoRoot, 'src', 'app');

// ── TypeScript loader ──────────────────────────────────────────────────────

let ts = null;
try { ts = require(path.join(appRoot, 'node_modules', 'typescript')); }
catch (_) {
  try { ts = require('typescript'); } catch (_2) { ts = null; }
}

if (!ts) {
  // Don't crash the whole runner when deps are absent - surface one explicit
  // skip instead. CI cannot hit this path: the workflow's type-check step
  // already fails hard without node_modules.
  module.exports = {
    tests: [{
      name: 'decision tree suite',
      run: () => ({ skip: true, reason: "typescript not installed - run 'npm ci' in src/app first" }),
    }],
  };
  return;
}

const originalTsxLoader = require.extensions['.tsx'];
require.extensions['.tsx'] = function loadTypeScript(module, filename) {
  const source = fs.readFileSync(filename, 'utf8');
  const output = ts.transpileModule(source, {
    compilerOptions: {
      esModuleInterop: true, module: ts.ModuleKind.CommonJS,
      moduleResolution: ts.ModuleResolutionKind.NodeJs, target: ts.ScriptTarget.ES2020,
      jsx: ts.JsxEmit.React,
    },
    fileName: filename,
  }).outputText;
  module._compile(output, filename);
};

// decisionTree.tsx imports `react` only to build JSX inside renderDecisionTree();
// none of the tests below call that function, so a minimal stub keeps this
// suite dependency-free (no real React runtime install required).
const originalModuleLoad = Module._load;
Module._load = function loadWithReactStub(request, parent, isMain) {
  if (request === 'react') {
    return { createElement: () => null };
  }
  return originalModuleLoad.apply(this, arguments);
};

const decisionTree = require(
  path.join(appRoot, 'src', 'renderer', 'utils', 'decisionTree.tsx'),
);

Module._load = originalModuleLoad;
if (originalTsxLoader) require.extensions['.tsx'] = originalTsxLoader;
else delete require.extensions['.tsx'];

const { summarizeMatchExpr } = decisionTree;

// ── Tests ────────────────────────────────────────────────────────────────

const tests = [

  {
    name: 'summarizeMatchExpr - single deterministic condition renders plainly',
    run() {
      assert.equal(summarizeMatchExpr({ min_chars: 4000 }), 'min_chars: 4000');
    },
  },

  {
    name: 'summarizeMatchExpr - multi-condition leaf (implicit AND) renders every condition',
    run() {
      // The engine compiles {min_chars, has_tools} in one leaf object as an
      // implicit `all` (make_leaf_factory in routing_policy.cpp) - both
      // conditions co-decide the match, so both must be visible, not just
      // the first object key.
      const text = summarizeMatchExpr({ min_chars: 4000, has_tools: true });
      assert.ok(text.includes('min_chars: 4000'), `expected min_chars in: ${text}`);
      assert.ok(text.includes('has_tools: true'), `expected has_tools in: ${text}`);
      assert.ok(text.includes(' AND '), `expected an explicit AND joiner, got: ${text}`);
    },
  },

  {
    name: 'summarizeMatchExpr - three-condition leaf joins all parts with AND',
    run() {
      const text = summarizeMatchExpr({ min_chars: 100, max_chars: 500, has_images: true });
      const parts = text.split(' AND ');
      assert.equal(parts.length, 3, `expected 3 AND-joined parts, got: ${text}`);
      assert.ok(parts.includes('min_chars: 100'));
      assert.ok(parts.includes('max_chars: 500'));
      assert.ok(parts.includes('has_images: true'));
    },
  },

  {
    name: 'summarizeMatchExpr - classifier-band leaf keeps its own comma-joined rendering',
    run() {
      // Classifier bands (classifier/label/min_score/max_score) are handled
      // by their dedicated branch and must not fall into the generic
      // AND-joined path even though they also carry multiple fields.
      const text = summarizeMatchExpr({ classifier: 'pii', label: 'PII', min_score: 0.5 });
      assert.ok(text.startsWith('classifier: pii'), `got: ${text}`);
      assert.ok(!text.includes(' AND '), `classifier leaves must not use the AND joiner, got: ${text}`);
    },
  },

  {
    name: 'summarizeMatchExpr - empty leaf object renders (empty)',
    run() {
      assert.equal(summarizeMatchExpr({}), '(empty)');
    },
  },

  {
    name: 'summarizeMatchExpr - any/all/not composites are unaffected by the multi-condition fix',
    run() {
      assert.equal(
        summarizeMatchExpr({ any: [{ min_chars: 10 }, { has_tools: true }] }),
        'any(min_chars: 10, has_tools: true)',
      );
      assert.equal(
        summarizeMatchExpr({ not: { has_images: true } }),
        'not(has_images: true)',
      );
    },
  },

];

module.exports = { tests };
