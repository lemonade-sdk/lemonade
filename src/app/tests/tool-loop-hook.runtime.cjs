const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const hook = fs.readFileSync(path.join(root, 'src/hooks/useChatStreaming.ts'), 'utf8');
const start = hook.indexOf('onToolCalls: async (toolCalls) => {');
const end = hook.indexOf('\n          onDone: (stats) => {', start);
assert.ok(start >= 0 && end > start, 'real onToolCalls callback must be present');
const callback = hook.slice(start + 'onToolCalls: '.length, end).trim().replace(/,$/, '');

const detectorPath = path.join(root, 'src/hooks/toolLoopDetector.ts');
const detector = ts.transpileModule(fs.readFileSync(detectorPath, 'utf8'), {
  fileName: detectorPath,
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
}).outputText;
const detectorModule = { exports: {} };
Function('module', 'exports', detector)(detectorModule, detectorModule.exports);

// Execute the production callback with a minimal controlled runtime, so the
// test covers actual tool execution, skipped results and message correlation.
const harness = ts.transpileModule(`
function makeHarness(executeTool) {
  const toolLoopDetector = new ToolLoopDetector();
  const runtime = { execute: executeTool };
  const controller = { signal: { aborted: false } };
  const ownsConversation = () => true;
  const dropTokenBufferIfOwned = () => {};
  const cleanupIfOwned = () => {};
  const onError = (_id, error) => { throw new Error(error); };
  const resolve = () => {};
  const reject = error => { throw error; };
  const convoId = 'test';
  const TOOL_FOLLOWUP_PROMPT = 'Use the tool result';
  const summarizeResult = (_name, data) => JSON.stringify(data);
  let fullMessages = [];
  const allToolCalls = [];
  let toolsDisabled = false;
  let blockedOnlyRounds = 0;
  let activeStreams = {};
  let followups = 0;
  const setActiveStreams = update => { activeStreams = update(activeStreams); };
  const runCompletion = async () => { followups++; };
  const onToolCalls = ${callback};
  return { onToolCalls, snapshot: () => ({ fullMessages, allToolCalls, toolsDisabled, activeStreams, followups }) };
}
`, {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
  reportDiagnostics: true,
});
assert.equal((harness.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error).length, 0);
const makeHarness = Function('ToolLoopDetector', harness.outputText + '\nreturn makeHarness;')(
  detectorModule.exports.ToolLoopDetector,
);

const called = [];
const active = makeHarness(async tc => {
  called.push(tc);
  const bad = tc.function.name === 'validate';
  return { role: 'tool', tool_call_id: tc.id, content: JSON.stringify(bad
    ? { error: 'invalid mesh' } : { changed: true }), error: bad };
});
const bad = i => ({ id: `bad_${i}`, type: 'function', function: { name: 'validate', arguments: '{"mesh":"bad.stl"}' } });
const good = i => ({ id: `good_${i}`, type: 'function', function: { name: 'edit_line', arguments: JSON.stringify({ step: i }) } });

async function main() {
  for (let i = 0; i < 7; i++) {
    await active.onToolCalls([bad(i), good(i)]);
    assert.equal(active.snapshot().toolsDisabled, false, 'productive tools must remain available');
  }
  assert.equal(called.filter(tc => tc.function.name === 'validate').length, 6,
    'another successful tool permits one validation retry after blocking');
  assert.equal(called.filter(tc => tc.function.name === 'edit_line').length, 7);
  const { fullMessages, allToolCalls } = active.snapshot();
  const skipped = fullMessages.find(msg => msg.role === 'tool' && msg.tool_call_id === 'bad_6');
  assert.match(JSON.parse(skipped.content).error, /Skipped: validate/);
  assert.equal(allToolCalls.find(entry => entry.name === 'validate' && entry.result.startsWith('Skipped:')).status, 'error');
  assert.ok(fullMessages.find(msg => msg.role === 'tool' && msg.tool_call_id === 'good_6'));

  // A repaired error can be validated with the original arguments.
  let repaired = false;
  const recoveryCalls = [];
  const recovering = makeHarness(async tc => {
    recoveryCalls.push(tc);
    const broken = tc.function.name === 'validate' && !repaired;
    return { role: 'tool', tool_call_id: tc.id,
      content: JSON.stringify(broken ? { error: 'invalid mesh' } : { changed: true }),
      error: broken };
  });
  for (let i = 0; i < 5; i++) await recovering.onToolCalls([bad(i), good(i)]);
  repaired = true;
  await recovering.onToolCalls([bad(5), good(5)]);
  assert.equal(recoveryCalls.filter(tc => tc.function.name === 'validate').length, 6);
  assert.deepEqual(JSON.parse(recovering.snapshot().fullMessages.find(msg =>
    msg.role === 'tool' && msg.tool_call_id === 'bad_5').content), { changed: true });
  await recovering.onToolCalls([bad(6)]);
  assert.equal(recoveryCalls.filter(tc => tc.function.name === 'validate').length, 7);
  assert.equal(recovering.snapshot().toolsDisabled, false);

  // Without any other completed tool, two exclusively blocked rounds finish safely.
  const stalledCalls = [];
  const stalled = makeHarness(async tc => {
    stalledCalls.push(tc);
    return { role: 'tool', tool_call_id: tc.id,
      content: JSON.stringify({ error: 'invalid mesh' }), error: true };
  });
  for (let i = 0; i < 5; i++) await stalled.onToolCalls([bad(i)]);
  await stalled.onToolCalls([bad(5)]);
  assert.equal(stalled.snapshot().toolsDisabled, false);
  await stalled.onToolCalls([bad(6)]);
  assert.equal(stalled.snapshot().toolsDisabled, true);
  assert.equal(stalledCalls.length, 5);
  assert.match(stalled.snapshot().fullMessages.at(-1).content, /Tools are now disabled/);
  console.log('Tool-loop hook: retries, recovery, per-call skipping and final fallback passed.');
}

main().catch(err => { console.error(err); process.exitCode = 1; });
