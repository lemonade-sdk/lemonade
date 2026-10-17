const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');

function loadTs(filename, mocks) {
  const compiled = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
    fileName: filename,
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS, esModuleInterop: true },
    reportDiagnostics: true,
  });
  const errors = (compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error);
  assert.equal(errors.length, 0, errors.map(e => ts.flattenDiagnosticMessageText(e.messageText, '\n')).join('\n'));
  const module = { exports: {} };
  const requireMock = name => {
    assert.ok(Object.prototype.hasOwnProperty.call(mocks, name), `unexpected import: ${name}`);
    return mocks[name];
  };
  Function('exports', 'require', 'module', compiled.outputText)(module.exports, requireMock, module);
  return module.exports;
}

const feedbackFile = path.join(root, 'src/mermaidFeedback.ts');
const renderer = {
  mermaidValidationAvailable: () => true,
  renderMermaidSvg: async source => {
    if (/\btimeline\b/.test(source)) throw Error('No diagram type detected for text: timeline');
    if (/bad label/.test(source)) throw Error('Parse error on line 3: bad label');
    return '<svg></svg>';
  },
};
const feedback = loadTs(feedbackFile, { './mermaidRenderer': renderer });

const good = 'flowchart LR\n  A --> B';
const bad = 'timeline\n  2026 : bad';
const badTwo = 'flowchart LR\n  A[bad label] --> B';
const fence = source => `\`\`\`mermaid\n${source}\n\`\`\``;

async function run() {
  assert.deepEqual(await feedback.validateMermaidSource(good, renderer.renderMermaidSvg), { valid: true });
  assert.match((await feedback.validateMermaidSource(bad, renderer.renderMermaidSvg)).error, /No diagram type/);
  assert.equal((await feedback.validateMermaidSource(' ', renderer.renderMermaidSvg)).valid, false);
  assert.equal((await feedback.validateMermaidSource(good, null)).unavailable, true);

  const original = ['Intro (keep).', fence(good), 'Between (keep).', fence(bad), 'End (keep).'].join('\n\n');
  const session = new feedback.MermaidRepairSession(renderer.renderMermaidSvg);
  const first = await session.process(original);
  assert.equal(first.kind, 'retry');
  assert.equal(first.attempt, 1);
  assert.match(first.prompt, /No diagram type detected/);
  assert.match(first.prompt, /exactly 1 fenced/);
  const final = await session.process(fence('flowchart TD\n  X --> Y'));
  assert.equal(final.kind, 'done');
  assert.ok(final.content.startsWith('Intro (keep).\n\n' + fence(good)));
  assert.match(final.content, /Between \(keep\)/);
  assert.match(final.content, /X --> Y/);
  assert.ok(final.content.endsWith('End (keep).'));
  assert.equal(final.content.includes('timeline'), false);

  const multiple = new feedback.MermaidRepairSession(renderer.renderMermaidSvg);
  const base = [fence(bad), 'Unmodified prose', fence(good), fence(badTwo)].join('\n\n');
  const attempts = await multiple.process(base);
  assert.equal(attempts.kind, 'retry');
  assert.match(attempts.prompt, /exactly 2 fenced/);
  const replaced = await multiple.process([fence('flowchart LR\n A --> Z'), fence('sequenceDiagram\n A->>B: ping')].join('\n'));
  assert.equal(replaced.kind, 'done');
  assert.ok(replaced.content.includes(fence(good)), 'valid diagrams must remain unchanged');
  assert.ok(replaced.content.includes('Unmodified prose'));
  assert.ok(replaced.content.includes('sequenceDiagram'));

  const invalid = new feedback.MermaidRepairSession(renderer.renderMermaidSvg);
  await invalid.process(original);
  const again = await invalid.process('Sorry, here is a fix:\n' + fence('flowchart LR\n X --> Y'));
  assert.equal(again.kind, 'retry', 'a malformed correction gets one retry');
  const exhausted = await invalid.process('still not just a fenced diagram');
  assert.equal(exhausted.kind, 'done', 'retry count must be bounded');
  assert.equal(exhausted.content, original, 'failed correction may not overwrite chat history');
  assert.equal(invalid.attempts, 2);

  const noMermaid = new feedback.MermaidRepairSession(renderer.renderMermaidSvg);
  assert.deepEqual(await noMermaid.process('ordinary chat'), { kind: 'done', content: 'ordinary chat' });
  const unavailable = new feedback.MermaidRepairSession(null);
  assert.deepEqual(await unavailable.process(original), { kind: 'done', content: original });

  const secondChance = new feedback.MermaidRepairSession(renderer.renderMermaidSvg);
  assert.equal((await secondChance.process(fence(bad))).kind, 'retry');
  const secondTry = await secondChance.process(fence(badTwo));
  assert.equal(secondTry.kind, 'retry', 'a syntactically wrong correction should get one more chance');
  const repaired = await secondChance.process(fence(good));
  assert.equal(repaired.kind, 'done');
  assert.equal(repaired.content, fence(good));

  let inflight = 0;
  let peak = 0;
  let unblock = () => {};
  const waitFirst = new Promise(resolve => { unblock = resolve; });
  const serial = loadTs(path.join(root, 'src/mermaidRenderer.ts'), {
    mermaid: { __esModule: true, default: {
      initialize: () => {},
      parse: () => true,
      render: async (_id, source) => {
        inflight++;
        peak = Math.max(peak, inflight);
        if (source === 'first') await waitFirst;
        inflight--;
        return '<svg />';
      },
    } },
  });
  const priorDocument = global.document;
  global.document = { getElementById: () => null };
  try {
    const first = serial.renderMermaidSvg('first');
    const second = serial.renderMermaidSvg('second');
    await new Promise(resolve => setTimeout(resolve, 0));
    assert.equal(peak, 1, 'Mermaid 9 must not render concurrently');
    unblock();
    assert.deepEqual(await Promise.all([first, second]), ['<svg />', '<svg />']);
    assert.equal(peak, 1);
  } finally {
    global.document = priorDocument;
  }

  const chatView = fs.readFileSync(path.join(root, 'src/components/ChatView.tsx'), 'utf8');
  const hook = fs.readFileSync(path.join(root, 'src/hooks/useChatStreaming.ts'), 'utf8');
  assert.doesNotMatch(chatView, /buildMermaidValidationRuntime|mermaidRuntime|mermaidTools/);
  assert.equal(fs.existsSync(path.join(root, 'src/tools/mermaidTools.ts')), false);
  assert.match(hook, /MermaidRepairSession/);
  assert.match(hook, /!isMermaidCorrection\(\)/);
  console.log('Mermaid automatic feedback, code-only repairs, and retry limits passed.');
}

run().catch(err => { console.error(err); process.exitCode = 1; });
