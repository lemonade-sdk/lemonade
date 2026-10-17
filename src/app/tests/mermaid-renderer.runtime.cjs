const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const helper = path.join(root, 'src/mermaidRenderer.ts');
const source = fs.readFileSync(helper, 'utf8');
const compiled = ts.transpileModule(source, {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020, esModuleInterop: true },
  fileName: helper,
  reportDiagnostics: true,
});
assert.equal((compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error).length, 0);

function createHarness(mermaid) {
  const nodes = new Set();
  const document = {
    getElementById: id => nodes.has(id) ? { remove: () => nodes.delete(id) } : null,
  };
  const mod = { exports: {} };
  const requireMock = spec => {
    assert.equal(spec, 'mermaid');
    return { __esModule: true, default: mermaid };
  };
  Function('exports', 'require', 'module', 'document', compiled.outputText)(
    mod.exports, requireMock, mod, document,
  );
  return {
    render: mod.exports.renderMermaidSvg,
    attach: id => { nodes.add(`d${id}`); nodes.add(`i${id}`); },
    nodes,
  };
}

async function run() {
  let initCalls = 0;
  let renderCalls = 0;
  const old = createHarness({
    initialize: options => {
      initCalls++;
      assert.equal(options.securityLevel, 'strict');
      assert.equal(options.suppressErrorRendering, true);
    },
    parse: source => source !== 'invalid',
    render: (id, source) => {
      renderCalls++;
      old.attach(id);
      if (source === 'render throws') throw new Error('renderer failed');
      return '<svg>9</svg>';
    },
  });
  assert.equal(await old.render('graph LR\nA-->B'), '<svg>9</svg>');
  assert.equal(await old.render('graph LR\nA-->C'), '<svg>9</svg>');
  assert.equal(initCalls, 1);
  assert.equal(old.nodes.size, 0);
  await assert.rejects(old.render('invalid'), /syntax error/);
  assert.equal(renderCalls, 2, 'invalid syntax must not reach render()');
  await assert.rejects(old.render('render throws'), /renderer failed/);
  assert.equal(old.nodes.size, 0, 'Mermaid 9 error SVG must not leak into body');

  let newInitCalls = 0;
  const current = createHarness({
    initialize: () => { newInitCalls++; },
    parse: async () => ({ diagramType: 'flowchart' }),
    render: async id => {
      current.attach(id);
      return { svg: '<svg>11</svg>' };
    },
  });
  assert.equal(await current.render('graph LR\nA-->B'), '<svg>11</svg>');
  assert.equal(newInitCalls, 1);
  assert.equal(current.nodes.size, 0);

  let attempts = 0;
  const rejected = createHarness({
    initialize: () => {},
    parse: async () => { throw new Error('No diagram type detected'); },
    render: () => { attempts++; },
  });
  await assert.rejects(rejected.render('timeline'), /No diagram type detected/);
  assert.equal(attempts, 0);

  const missingParser = createHarness({
    initialize: () => {},
    render: async id => {
      missingParser.attach(id);
      return { svg: '<svg>fallback</svg>' };
    },
  });
  assert.equal(await missingParser.render('graph LR'), '<svg>fallback</svg>',
    'the distro Mermaid stub without parse() must keep working');
  assert.equal(missingParser.nodes.size, 0);

  const empty = createHarness({
    initialize: () => {},
    parse: () => true,
    render: id => { empty.attach(id); return ''; },
  });
  await assert.rejects(empty.render('graph LR'), /no diagram markup/);
  assert.equal(empty.nodes.size, 0);

  const ids = [];
  const concurrent = createHarness({
    initialize: () => {},
    parse: async () => true,
    render: async id => {
      ids.push(id);
      concurrent.attach(id);
      await Promise.resolve();
      return { svg: '<svg />' };
    },
  });
  await Promise.all([concurrent.render('graph LR'), concurrent.render('graph TD')]);
  assert.equal(new Set(ids).size, 2, 'concurrent renders require distinct IDs');
  assert.equal(concurrent.nodes.size, 0);

  const markdown = fs.readFileSync(path.join(root, 'src/components/MarkdownMessage.tsx'), 'utf8');
  assert.match(markdown, /await renderMermaidSvg\(source\)/);
  console.log('Mermaid 9/11 rendering and cleanup checks passed.');
}

run().catch(err => { console.error(err); process.exitCode = 1; });
