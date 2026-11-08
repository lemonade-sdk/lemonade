/**
 * Stop -> resend -> the stopped request's tool settles.
 *
 * Tool execution takes no abort signal, so a stopped request keeps running and
 * its callbacks resume long after Stop. By then the user may have sent a new
 * request in the same conversation. Stream state is keyed by conversation, not
 * by request, so the late callbacks must not touch the newer request's state.
 */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const hookPath = path.join(root, 'src/hooks/useChatStreaming.ts');
const apiPath = path.join(root, 'src/api.ts');

const source = fs.readFileSync(hookPath, 'utf8');
const chatView = fs.readFileSync(path.join(root, 'src/components/ChatView.tsx'), 'utf8');
assert.match(chatView, /const canInterruptAndContinue = isStreaming && !interruptPending/);
assert.match(chatView, /!currentStream\?\.toolCalls\.length/, 'steering must not overlap tool execution');
assert.match(chatView, /connectionStatus === 'connected'/, 'offline requests must not be interrupted');
assert.match(chatView, /currentStream\?\.interruptible === true/, 'eligibility must follow the actual in-flight request');
assert.match(chatView, /const interruptible = !hasImages && !userMessage\.imageIds\?\.length/);
assert.match(chatView, /deletedConversationIdsRef\.current\.add\(id\)/);
assert.match(chatView, /streaming\.hasActiveRequest\(activeId\)/, 'stale React stream state must not permit a second send');
assert.match(chatView, /startAttemptRef\.current\.get\(convoId\) !== startAttempt/, 'obsolete preparation must not start');
assert.match(chatView, /startAttemptRef\.current\.set\(activeId, Symbol\('chat-edit'\)\)/);
assert.match(chatView, /if \(deletedConversationIdsRef\.current\.has\(convoId\)/);
assert.match(chatView, /const partial = interrupting \? streaming\.stop\(convoId, true\)/);
assert.match(chatView, /imageRegistry\.addUploads\(/, 'image registry must survive main integration');
assert.match(chatView, /const isNewConvo = !convoId/, 'new chat handling must survive main integration');
assert.match(chatView, /messages: \[\.\.\.conversation\.messages, \.\.\.\(interruptedMessage/);
assert.match(chatView, /await startAssistantResponse\([\s\S]*?priorMessages,/);

const compiled = ts.transpileModule(source, {
  compilerOptions: {
    target: ts.ScriptTarget.ES2020,
    module: ts.ModuleKind.CommonJS,
    esModuleInterop: true,
  },
  fileName: hookPath,
  reportDiagnostics: true,
});
const errors = (compiled.diagnostics || []).filter(
  diagnostic => diagnostic.category === ts.DiagnosticCategory.Error,
);
assert.equal(
  errors.length,
  0,
  errors.map(diagnostic => ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n')).join('\n'),
);

assert.match(
  source,
  /controllersRef\.current\.get\(convoId\) === controller/,
  'late callbacks must compare ownership against their own controller',
);

/* ── Minimal hook runtime ────────────────────────────────────────────────── */

let slots = [];
let hookIndex = 0;

function useState(initial) {
  const i = hookIndex++;
  if (!(i in slots)) slots[i] = typeof initial === 'function' ? initial() : initial;
  const set = value => { slots[i] = typeof value === 'function' ? value(slots[i]) : value; };
  return [slots[i], set];
}

function useRef(initial) {
  const i = hookIndex++;
  if (!(i in slots)) slots[i] = { current: initial };
  return slots[i];
}

const reactStub = {
  useState,
  useRef,
  useCallback: fn => fn,
  useEffect: () => {},
};
reactStub.default = reactStub;
reactStub.__esModule = true;

/* ── Controllable timers so the token flush is deterministic ─────────────── */

let timers = [];
const windowStub = {
  setTimeout(fn) { timers.push(fn); return timers.length; },
  clearTimeout() {},
};
function runTimers() {
  const pending = timers;
  timers = [];
  for (const fn of pending) fn();
}

/* ── Fake chat completion: the test drives the callbacks ─────────────────── */

const calls = [];
const apiStub = {
  chatCompletion(model, messages, callbacks) {
    calls.push(callbacks);
    return new Promise(() => {});
  },
};

// Load the real implementation: this test checks cancellation while a tool
// settles, so mocking out loop detection would hide integration regressions.
const detectorPath = path.join(root, 'src/hooks/toolLoopDetector.ts');
const detectorCompiled = ts.transpileModule(fs.readFileSync(detectorPath, 'utf8'), {
  compilerOptions: {
    target: ts.ScriptTarget.ES2020,
    module: ts.ModuleKind.CommonJS,
  },
  fileName: detectorPath,
  reportDiagnostics: true,
});
const detectorErrors = (detectorCompiled.diagnostics || []).filter(
  diagnostic => diagnostic.category === ts.DiagnosticCategory.Error,
);
assert.equal(detectorErrors.length, 0);
const detectorModule = { exports: {} };
new Function('exports', 'module', detectorCompiled.outputText)(
  detectorModule.exports, detectorModule,
);

// Exercise the real direct image follow-up helper in this streaming harness.
const directAnalysisPath = path.join(root, 'src/features/images/directImageAnalysis.ts');
const directAnalysisCompiled = ts.transpileModule(fs.readFileSync(directAnalysisPath, 'utf8'), {
  fileName: directAnalysisPath,
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
});
const directAnalysisModule = { exports: {} };
new Function('module', 'exports', directAnalysisCompiled.outputText)(
  directAnalysisModule, directAnalysisModule.exports,
);

/* ── Module wiring ───────────────────────────────────────────────────────── */

const originalWindow = global.window;
const originalReact = require.cache[require.resolve('react')];

global.window = windowStub;
require.cache[apiPath] = { id: apiPath, filename: apiPath, loaded: true, exports: apiStub };
require.cache[require.resolve('react')] = {
  id: require.resolve('react'),
  filename: require.resolve('react'),
  loaded: true,
  exports: reactStub,
};

const hookModule = { exports: {} };
new Function('exports', 'require', 'module', '__filename', '__dirname', compiled.outputText)(
  hookModule.exports,
  request => {
    if (request === 'react') return reactStub;
    if (request === '../api') return apiStub;
    if (request === './toolLoopDetector') return detectorModule.exports;
    if (request === '../features/images/directImageAnalysis') return directAnalysisModule.exports;
    throw new Error(`Unexpected dependency while loading useChatStreaming: ${request}`);
  },
  hookModule,
  hookPath,
  path.dirname(hookPath),
);

const { useChatStreaming } = hookModule.exports;

/* ── Harness ─────────────────────────────────────────────────────────────── */

const tick = () => new Promise(resolve => setImmediate(resolve));

function reset() {
  slots = [];
  hookIndex = 0;
  timers = [];
  calls.length = 0;
}

function render() {
  hookIndex = 0;
  return useChatStreaming(() => {}, () => {});
}

function toolRuntime(execute) {
  return { tools: [{ type: 'function' }], execute };
}

const TOOL_CALL = { id: 'call_1', function: { name: 'pull_model', arguments: '{}' } };

async function startRequestWithPendingTool() {
  const hook = render();
  let settleTool;
  const pending = new Promise((resolve, reject) => { settleTool = { resolve, reject }; });

  hook.send('c1', 'm', [{ role: 'user', content: 'download it' }], toolRuntime(() => pending));
  await tick();

  assert.equal(calls.length, 1, 'the first request must reach chatCompletion');
  calls[0].onToolCalls([TOOL_CALL]);
  await tick();

  return { settleTool };
}

async function main() {

/* ── Case 1: stop, no resend, tool resolves ──────────────────────────────── */
/* The stopped conversation must not come back when the orphaned tool lands.  */
{
  reset();
  const { settleTool } = await startRequestWithPendingTool();

  render().stop('c1');
  assert.equal(render().streamingConvoIds.has('c1'), false, 'Stop must clear the streaming state');

  settleTool.resolve({ content: '{}', tool_call_id: 'call_1' });
  await tick();
  await tick();

  assert.equal(
    render().streamingConvoIds.has('c1'),
    false,
    'a stopped conversation must stay stopped when its orphaned tool finishes',
  );
  assert.equal(calls.length, 1, 'a stopped request must not start a follow-up completion');
}

/* ── Case 2: stop, resend, old tool REJECTS ──────────────────────────────── */
/* The old error path must not tear down the new request.                     */
{
  reset();
  const { settleTool } = await startRequestWithPendingTool();

  render().stop('c1');

  render().send('c1', 'm', [{ role: 'user', content: 'write a story' }], null);
  await tick();
  assert.equal(calls.length, 2, 'the resend must reach chatCompletion');
  const newSignal = calls[1].signal;
  assert.equal(render().streamingConvoIds.has('c1'), true, 'the resend must stream');

  settleTool.reject(new Error('tool blew up'));
  await tick();
  await tick();

  assert.equal(
    render().streamingConvoIds.has('c1'),
    true,
    'a rejected orphaned tool must not clear the new request stream',
  );
  assert.equal(
    newSignal.aborted,
    false,
    'a rejected orphaned tool must not abort the new request controller',
  );
}

/* ── Case 3: stop, resend, old tool RESOLVES ─────────────────────────────── */
/* The old success path must not discard the new request's buffered tokens.   */
{
  reset();
  const { settleTool } = await startRequestWithPendingTool();

  render().stop('c1');

  render().send('c1', 'm', [{ role: 'user', content: 'write a story' }], null);
  await tick();
  const newSignal = calls[1].signal;

  calls[1].onToken('Hello', 'Hello');

  settleTool.resolve({ content: '{}', tool_call_id: 'call_1' });
  await tick();
  await tick();

  runTimers();

  assert.equal(
    render().getStream('c1')?.content,
    'Hello',
    'a resolved orphaned tool must not discard the new request token buffer',
  );
  assert.equal(
    newSignal.aborted,
    false,
    'a resolved orphaned tool must not abort the new request controller',
  );
}

/* Case 4: stop before the 50ms flush must keep both content and reasoning. */
{
  reset();
  render().send('c1', 'm', [{ role: 'user', content: 'Start with Python' }], null);
  await tick();
  assert.equal(calls.length, 1);
  calls[0].onReasoning('Think', 'Think about Python');
  calls[0].onToken('Code', 'Partial code');
  const firstId = render().getStream('c1')?.requestId;
  assert.equal(typeof firstId, 'number');
  const partial = render().stop('c1');
  assert.deepEqual(partial, { content: 'Partial code', thinking: 'Think about Python' });
  assert.equal(calls[0].signal.aborted, true);
  assert.equal(render().streamingConvoIds.has('c1'), false);

  render().send('c1', 'm', [
    { role: 'user', content: 'Start with Python' },
    { role: 'assistant', content: partial.content },
    { role: 'user', content: 'Use HTML instead' },
  ], null);
  await tick();
  assert.equal(calls.length, 2);
  assert.ok(render().getStream('c1')?.requestId > firstId, 'resumed request must have a new generation');
  calls[0].onToken('late', 'late');
  calls[1].onToken('HTML', 'HTML response');
  runTimers();
  assert.equal(render().getStream('c1')?.content, 'HTML response');
  assert.equal(calls[1].signal.aborted, false);
}

/* Case 5: an active request with no tokens still returns a stop receipt. */
{
  reset();
  render().send('c1', 'm', [{ role: 'user', content: 'Think' }], null);
  await tick();
  assert.deepEqual(render().stop('c1'), { content: '', thinking: undefined });
}

/* Case 6: tokens flushed after a render snapshot must survive Stop. */
{
  reset();
  render().send('c1', 'm', [{ role: 'user', content: 'Think' }], null);
  await tick();
  const staleStop = render().stop;
  calls[0].onReasoning('thinking', 'all reasoning');
  calls[0].onToken('answer', 'all content');
  runTimers(); // Token buffer emptied; staleStop still has old React state.
  assert.deepEqual(staleStop('c1'), { content: 'all content', thinking: 'all reasoning' });
}

/* Case 7: a token flush must not remove tool metadata or permit a tool abort. */
{
  reset();
  let settleTool;
  const pending = new Promise(resolve => { settleTool = resolve; });
  render().send('c1', 'm', [{ role: 'user', content: 'download it' }], toolRuntime(() => pending), 'normal', true);
  await tick();
  calls[0].onToken('draft', 'partial output');
  calls[0].onToolCalls([TOOL_CALL]);
  await tick();
  runTimers();
  assert.equal(render().getStream('c1')?.toolCalls.length, 1);
  assert.equal(render().stop('c1', true), null, 'cannot interrupt after a tool call');
  assert.equal(calls[0].signal.aborted, false);
  render().stop('c1'); // Explicit Stop remains available.
  settleTool({ content: '{}', tool_call_id: 'call_1' });
  await tick();
}

/* Case 8: a media request stays non-interruptible even after UI errors. */
{
  reset();
  render().send('c1', 'm', [{ role: 'user', content: 'audio request' }], null, 'normal', false);
  await tick();
  assert.equal(render().getStream('c1')?.interruptible, false);
  assert.equal(render().stop('c1', true), null);
  assert.equal(calls[0].signal.aborted, false);
  assert.notEqual(render().stop('c1'), null, 'explicit Stop still works');
}

/* Case 9: an eligible text request can be interrupted. */
{
  reset();
  render().send('c1', 'm', [{ role: 'user', content: 'text request' }], null, 'normal', true);
  await tick();
  assert.equal(render().getStream('c1')?.interruptible, true);
  assert.notEqual(render().stop('c1', true), null);
  assert.equal(calls[0].signal.aborted, true);
}

/* Case 10: an overlapping send aborts the old controller and owns the stream. */
{
  reset();
  render().send('c1', 'm', [{ role: 'user', content: 'first' }], null, 'normal', true);
  await tick();
  const oldSignal = calls[0].signal;
  render().send('c1', 'm', [{ role: 'user', content: 'edited' }], null, 'normal', true);
  await tick();
  assert.equal(calls.length, 2);
  assert.equal(oldSignal.aborted, true, 'the superseded request must be cancelled');
  assert.equal(calls[1].signal.aborted, false);
  calls[0].onToken('old', 'old result');
  calls[1].onToken('new', 'edited result');
  runTimers();
  assert.equal(render().getStream('c1')?.content, 'edited result');
  assert.equal(calls[1].signal.aborted, false);
}

/* Case 11: a stale second send must never abort an active tool. */
{
  reset();
  let settleTool;
  const pending = new Promise(resolve => { settleTool = resolve; });
  render().send('c1', 'm', [{ role: 'user', content: 'use tool' }], toolRuntime(() => pending));
  await tick();
  const oldSignal = calls[0].signal;
  calls[0].onToolCalls([TOOL_CALL]);
  await tick();
  await assert.rejects(
    render().send('c1', 'm', [{ role: 'user', content: 'second' }], null),
    /Cannot replace a chat request after tools have started/,
  );
  assert.equal(calls.length, 1, 'no second inference should start');
  assert.equal(oldSignal.aborted, false, 'tool-running request must remain intact');
  assert.equal(render().getStream('c1')?.toolCalls[0]?.status, 'running');
  render().stop('c1');
  settleTool({ content: '{}', tool_call_id: 'call_1' });
  await tick();
}

}

main().then(() => {
  if (originalReact) require.cache[require.resolve('react')] = originalReact;
  else delete require.cache[require.resolve('react')];
  delete require.cache[apiPath];
  if (originalWindow === undefined) delete global.window;
  else global.window = originalWindow;

  console.log('chat-streaming-abort: all assertions passed');
}, err => {
  console.error(err);
  process.exit(1);
});
