const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const filename = path.resolve(__dirname, '../src/features/chatHistory/toolCallPersistence.ts');
const compiled = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  fileName: filename,
  reportDiagnostics: true,
});
assert.deepEqual((compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error), []);
const mod = { exports: {} };
Function('exports', 'module', compiled.outputText)(mod.exports, mod);
const { prepareToolCallsForStorage } = mod.exports;

const image = `data:image/png;base64,${'a'.repeat(1800)}`;
const audio = `data:audio/wav;base64,${'b'.repeat(1800)}`;
const rawArgs = JSON.stringify({ question: 'Which model?', choices: ['Model A'], image });
const calls = [{
  name: 'ask_question', args: `image: ${image}`, rawArgs,
  result: `Result: ${audio}`, status: 'done',
  artifacts: [{ type: 'image', url: image }, { type: 'audio', url: audio }],
}, {
  name: 'other', args: 'count: 5', result: 'Done', status: 'done',
  artifacts: [{ type: 'model3d', url: 'blob:local-result' }],
}];
const persisted = prepareToolCallsForStorage(calls);
assert.notEqual(persisted, calls);
assert.ok(calls[0].artifacts.length === 2, 'live artifacts must remain untouched');
assert.equal(persisted[0].artifacts, undefined);
assert.equal(persisted[1].artifacts, undefined);
assert.doesNotMatch(JSON.stringify(persisted), /data:(?:image|audio)\//);
assert.doesNotMatch(JSON.stringify(persisted), /a{1024}|b{1024}/);
assert.deepEqual(JSON.parse(persisted[0].rawArgs).choices, ['Model A']);
assert.equal(persisted[1].args, 'count: 5');
assert.equal(persisted[1].result, 'Done');
assert.equal(prepareToolCallsForStorage(undefined), undefined);

// The active persistence path must actually invoke the sanitizer.
const view = fs.readFileSync(path.resolve(__dirname, '../src/components/ChatView.tsx'), 'utf8');
assert.match(view, /toolCalls: prepareToolCallsForStorage\(m\.toolCalls\)/);
console.log('Chat tool media persistence regression checks passed.');
