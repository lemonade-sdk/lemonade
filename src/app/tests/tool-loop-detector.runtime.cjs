const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const filename = path.join(__dirname, '../src/hooks/toolLoopDetector.ts');
const compiled = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
  fileName: filename,
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
  reportDiagnostics: true,
});
assert.equal((compiled.diagnostics || []).filter(e => e.category === ts.DiagnosticCategory.Error).length, 0);
const sandbox = { exports: {} };
Function('exports', 'module', compiled.outputText)(sandbox.exports, sandbox);
const { ToolLoopDetector } = sandbox.exports;

const call = (name, args) => ({ function: { name, arguments: JSON.stringify(args) } });
const result = (data, error = false) => ({ content: JSON.stringify(data), error });
const observe = (guard, tool, response) => guard.observe([tool], [response]);

// Long productive CAD jobs are never constrained by an arbitrary round count.
const cad = new ToolLoopDetector();
for (let i = 0; i < 250; i++) {
  assert.equal(observe(cad, call('edit_line', { line: i, x: i * 2 }), result({ ok: true })), null);
}

// Reusing an invocation is legitimate when the result changes with progress.
const download = new ToolLoopDetector();
for (let progress = 0; progress < 100; progress++) {
  assert.equal(observe(download, call('get_download_status', { id: 3 }),
    result({ status: 'ready', progress })), null);
}

// Reusing an edit instruction can also make progress in a stateful CAD engine.
const revisions = new ToolLoopDetector();
for (let i = 0; i < 60; i++) {
  assert.equal(observe(revisions, call('increment_rotation', { degrees: 1 }),
    result({ ok: true, revision: i })), null);
}
const explicitChanges = new ToolLoopDetector();
for (let i = 0; i < 60; i++) {
  assert.equal(observe(explicitChanges, call('increment_rotation', { degrees: 1 }),
    result({ changed: true, status: 'ok' })), null);
}

const failing = new ToolLoopDetector();
const invalid = call('inspect_geometry', { file: 'broken.stl' });
assert.equal(observe(failing, invalid, result({ error: 'unsupported' }, true)), null);
assert.equal(observe(failing, invalid, result({ error: 'unsupported' }, true)), null);
assert.equal(observe(failing, invalid, result({ error: 'unsupported' }, true)).action, 'warn');
assert.equal(observe(failing, invalid, result({ error: 'unsupported' }, true)), null);
assert.equal(observe(failing, invalid, result({ error: 'unsupported' }, true)).action, 'warn');
assert.match(failing.blockedReason(invalid), /blocked/);
assert.equal(failing.blockedReason(call('inspect_geometry', { file: 'valid.stl' })), null);

// JSON key order, transient request identifiers and argument whitespace do not hide loops.
const shuffled = new ToolLoopDetector();
for (let i = 0; i < 5; i++) {
  const action = { function: { name: 'get_model_info', arguments: i % 2 ? '{"b":2,"a":1}' : '{"a":1,"b":2}' } };
  const response = result(i % 2 ? { error: 'not found', request_id: `${i}`, timestamp: i } :
    { timestamp: i, request_id: `${i}`, error: 'not found' }, true);
  const verdict = observe(shuffled, action, response);
  if (i === 2) assert.equal(verdict?.action, 'warn');
  else if (i === 4) assert.equal(verdict?.action, 'warn');
  else assert.equal(verdict, null);
}
assert.match(shuffled.blockedReason(call('get_model_info', { b: 2, a: 1 })), /blocked/);

// Cycles involving multiple different tool calls should be caught too.
const alternating = new ToolLoopDetector();
for (let i = 0; i < 8; i++) {
  const name = i % 2 ? 'get_server_health' : 'list_models';
  const verdict = observe(alternating, call(name, { status: 'local' }), result({ count: 7 }));
  if (i === 5) assert.equal(verdict?.action, 'warn');
  else if (i === 7) assert.equal(verdict?.action, 'stop');
  else assert.equal(verdict, null);
}

// A running job without updated percentages is not evidence of a stuck agent.
const waiting = new ToolLoopDetector();
for (let i = 0; i < 90; i++) {
  const notice = observe(waiting, call('get_build_status', { job: 'cad' }),
    result({ status: 'running', timestamp: i }));
  if (i === 7) assert.equal(notice?.action, 'warn');
  else assert.equal(notice, null);
}

// A successful mutation in a batch resets the stalled-loop suspicion.
const mixed = new ToolLoopDetector();
for (let i = 0; i < 45; i++) {
  assert.equal(mixed.observe([
    call('get_status', { id: 10 }), call('edit_line', { id: 10 }),
  ], [result({ status: 'ready' }), result({ changed: true })]), null);
}

// A pending read must not hide a failing tool repeated in the same batch.
const mixedWait = new ToolLoopDetector();
for (let i = 0; i < 5; i++) {
  const notice = mixedWait.observe([
    call('get_build_status', { job: 1 }), call('inspect_geometry', { file: 'bad.stl' }),
  ], [result({ status: 'running' }), result({ error: 'syntax' }, true)]);
  if (i === 2) assert.equal(notice?.action, 'warn');
  else if (i === 4) assert.equal(notice?.action, 'warn');
  else assert.equal(notice, null);
}

// A productive parallel call must not mask a failing invocation.
const productive = new ToolLoopDetector();
const badValidation = call('validate', { mesh: 'bad.stl' });
for (let i = 0; i < 5; i++) {
  const notice = productive.observe([
    badValidation, call('edit_line', { step: i }),
  ], [result({ error: 'invalid geometry' }, true), result({ changed: true })]);
  assert.equal(notice?.action, i === 2 || i === 4 ? 'warn' : undefined);
}
assert.match(productive.blockedReason(badValidation), /blocked/);
assert.equal(productive.blockedReason(call('edit_line', { step: 4 })), null);
assert.equal(productive.blockedReason(call('validate', { mesh: 'fixed.stl' })), null);

// A changed search query must not make repeated load failures invisible.
const search = new ToolLoopDetector();
const badLoad = call('load_model', { model_name: 'typo' });
for (let i = 0; i < 5; i++) {
  const notice = search.observe([
    badLoad, call('list_models', { query: `q${i}` }),
  ], [result({ error: 'not found', request_id: `req_${i}` }), result({ models: ['qwen'] })]);
  assert.equal(notice?.action, i === 2 || i === 4 ? 'warn' : undefined);
}
assert.match(search.blockedReason(badLoad), /blocked/);
assert.equal(search.blockedReason(call('list_models', { query: 'q5' })), null);

// Distinct errors are not repeated failures, and a success resets the count.
const recovered = new ToolLoopDetector();
const retry = call('load_model', { model_name: 'correct' });
for (let i = 0; i < 12; i++) {
  assert.equal(observe(recovered, retry, result({ error: `failure ${i}` }, true)), null);
}
assert.equal(recovered.blockedReason(retry), null);
for (let i = 0; i < 4; i++) observe(recovered, retry, result({ error: 'temporary' }, true));
assert.equal(recovered.blockedReason(retry), null);
assert.equal(observe(recovered, retry, result({ status: 'ready' })), null);
for (let i = 0; i < 4; i++) observe(recovered, retry, result({ error: 'temporary' }, true));
assert.equal(recovered.blockedReason(retry), null);

// Repeated successful read-only calls still trigger the original round-level guard.
const readOnly = new ToolLoopDetector();
for (let i = 0; i < 5; i++) {
  const notice = observe(readOnly, call('list_models', { status: 'local' }), result({ count: 1 }));
  if (i === 2) assert.equal(notice?.action, 'warn');
  else if (i === 4) assert.equal(notice?.action, 'stop');
  else assert.equal(notice, null);
}

// Parallel calls are order-insensitive; changes to array element order are meaningful.
const batch = new ToolLoopDetector();
for (let i = 0; i < 5; i++) {
  const entries = [call('inspect_geometry', { file: 'A.stl' }), call('inspect_geometry', { file: 'B.stl' })];
  if (i % 2) entries.reverse();
  const replies = entries.map((_, j) => result({ error: j + (i % 2 ? 1 : 0) }, true));
  // B fails identically even though call ordering and A's error change.
  const notice = batch.observe(entries, replies);
  if (i === 2 || i === 4) assert.equal(notice?.action, 'warn');
  else assert.equal(notice, null);
}

// Parallel tool-call ordering does not mask a real stalled batch.
const permuted = new ToolLoopDetector();
for (let i = 0; i < 5; i++) {
  const entries = [call('inspect', { item: 'A' }), call('inspect', { item: 'B' })];
  if (i % 2) entries.reverse();
  const notice = permuted.observe(entries, entries.map(entry => result({ value: entry.function.arguments })));
  if (i === 2) assert.equal(notice?.action, 'warn');
  else if (i === 4) assert.equal(notice?.action, 'stop');
  else assert.equal(notice, null);
}

// Artifacts from repeated image/CAD generation count as observable output.
const media = new ToolLoopDetector();
for (let i = 0; i < 35; i++) {
  assert.equal(observe(media, call('generate_image', { prompt: 'apple' }),
    { ...result({ status: 'success' }), artifacts: [{ url: `blob://image-${i}` }] }), null);
}

// If the same error occurs again after the model changes arguments, it is new work.
const changed = new ToolLoopDetector();
for (let i = 0; i < 100; i++) {
  assert.equal(observe(changed, call('inspect_geometry', { file: `mesh_${i}.stl` }),
    result({ error: 'syntax' }, true)), null);
}
// The streaming hook must be governed by observed results, not by a fixed round count.
const hook = fs.readFileSync(path.join(__dirname, '../src/hooks/useChatStreaming.ts'), 'utf8');
assert.match(hook, /const toolLoopDetector = new ToolLoopDetector\(\)/);
assert.match(hook, /toolLoopDetector\.observe\(executedCalls, executedResults\)/);
assert.match(hook, /toolLoopDetector\.blockedReason\(tc\)/);
assert.match(hook, /tools: !toolsDisabled &&/);
assert.match(hook, /if \(toolsDisabled\) \{/);
assert.match(hook, /withoutToolSystemPrompt/);
assert.doesNotMatch(hook, /MAX_TOOL_ROUNDS|toolBudget\.consume/);

console.log('Tool loop detection: long productive tasks, retries, cycles and polling passed.');
