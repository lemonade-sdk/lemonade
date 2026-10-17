/*
 * parseOtelSpan appends the recorded answer to `trace.messages` so the Messages
 * tab can show it. That message is not part of the request, so every path that
 * rebuilds the request has to drop it again — otherwise Replay re-sends the
 * model its own answer and the model continues from it instead of answering.
 *
 * `capturedOutput` marks that one message. Role cannot be the test: a traced
 * request may legitimately contain earlier assistant turns, and dropping those
 * would destroy the conversation. These checks pin both halves of that: the
 * flag survives a localStorage round trip (including traces saved before the
 * flag existed), and every consumer that rebuilds a request honours it.
 */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const TRACES_KEY = 'lemonade_inspect_traces';

function makeStorage(seed) {
  const map = new Map(Object.entries(seed || {}));
  return {
    getItem: key => (map.has(key) ? map.get(key) : null),
    setItem: (key, value) => { map.set(key, String(value)); },
    removeItem: key => { map.delete(key); },
  };
}

// inspectStore builds its singleton at import time, and the constructor is what
// runs the migration. Re-evaluate the module per case so each case gets its own
// localStorage seed.
function loadInspectStore(traces) {
  const filename = path.join(root, 'src/inspectStore.ts');
  const compiled = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS,
      esModuleInterop: true,
    },
    fileName: filename,
    reportDiagnostics: true,
  });
  const diagnostics = compiled.diagnostics || [];
  assert.equal(
    diagnostics.length,
    0,
    diagnostics.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'),
  );

  const apiStub = new Proxy({ baseUrl: 'http://localhost:8000', sessionHeadersEnabled: false }, {
    get: (target, prop) => (prop in target ? target[prop] : () => {}),
    set: (target, prop, value) => { target[prop] = value; return true; },
  });

  const customRequire = request => {
    if (request === 'react') return { useState: initial => [initial, () => {}], useEffect: () => {} };
    if (request === './api') return { __esModule: true, default: apiStub };
    throw new Error(`Unexpected dependency while loading inspectStore: ${request}`);
  };

  const previous = { localStorage: global.localStorage, WebSocket: global.WebSocket };
  global.localStorage = makeStorage({ [TRACES_KEY]: JSON.stringify(traces) });
  global.WebSocket = class {
    constructor() { this.readyState = 0; }
    close() {}
    send() {}
    addEventListener() {}
  };

  try {
    const module = { exports: {} };
    Function('exports', 'require', 'module', '__filename', '__dirname', compiled.outputText)(
      module.exports,
      customRequire,
      module,
      filename,
      path.dirname(filename),
    );
    return module.exports.inspectStore;
  } finally {
    global.localStorage = previous.localStorage;
    global.WebSocket = previous.WebSocket;
  }
}

function tracesById(store) {
  const byId = new Map();
  for (const trace of store.getState().traces) byId.set(trace.id, trace);
  return byId;
}

const flagsOf = trace => trace.messages.map(m => !!m.capturedOutput);

// ---------------------------------------------------------------------------
// Saved traces: the flag has to be recovered for anything written before it
// existed, otherwise the defect simply survives in every stored trace.
// ---------------------------------------------------------------------------

const SAVED = [
  {
    id: 'single-turn',
    traceId: 't1', spanId: 's1', kind: 'LLM', model: 'Qwen3.5-2B-GGUF',
    messages: [
      { role: 'system', content: 'instructions' },
      { role: 'user', content: "Who's Doctor Doom?" },
      { role: 'assistant', content: 'Doctor Doom is a fictional supervillain…' },
    ],
    output: 'Doctor Doom is a fictional supervillain…',
  },
  {
    id: 'multi-turn',
    traceId: 't2', spanId: 's2', kind: 'LLM', model: 'Qwen3.5-2B-GGUF',
    messages: [
      { role: 'system', content: 'instructions' },
      { role: 'user', content: "Who's Doctor Doom?" },
      { role: 'assistant', content: 'Doctor Doom is a fictional supervillain…' },
      { role: 'user', content: 'Where is Latveria?' },
      { role: 'assistant', content: 'Latveria is in Europe.' },
    ],
    output: 'Latveria is in Europe.',
  },
  {
    id: 'empty-output',
    traceId: 't3', spanId: 's3', kind: 'LLM', model: 'Qwen3.5-2B-GGUF',
    messages: [
      { role: 'system', content: 'instructions' },
      { role: 'user', content: 'call a tool' },
      { role: 'assistant', content: '' },
    ],
    output: '',
  },
  {
    id: 'already-flagged',
    traceId: 't4', spanId: 's4', kind: 'LLM', model: 'Qwen3.5-2B-GGUF',
    messages: [
      { role: 'user', content: 'hello' },
      { role: 'assistant', content: 'hi', capturedOutput: true },
    ],
    output: 'hi',
  },
  {
    id: 'trailing-mismatch',
    traceId: 't5', spanId: 's5', kind: 'LLM', model: 'Qwen3.5-2B-GGUF',
    messages: [
      { role: 'user', content: 'hello' },
      { role: 'assistant', content: 'a different answer' },
    ],
    output: 'hi',
  },
];

const store = loadInspectStore(SAVED);
const saved = tracesById(store);

assert.equal(saved.size, SAVED.length, 'every seeded trace must rehydrate');

assert.deepEqual(
  flagsOf(saved.get('single-turn')),
  [false, false, true],
  'the trailing assistant message matching the output is the captured answer',
);

assert.deepEqual(
  flagsOf(saved.get('multi-turn')),
  [false, false, false, false, true],
  'an earlier assistant turn is part of the request and must stay unflagged',
);

// An assistant tool-call turn carries empty content. Without the non-empty
// output guard it matches an empty `output` and a real request turn is deleted.
assert.deepEqual(
  flagsOf(saved.get('empty-output')),
  [false, false, false],
  'an empty output must never mark an empty trailing assistant message',
);

assert.deepEqual(
  flagsOf(saved.get('already-flagged')),
  [false, true],
  'a flag written by parseOtelSpan survives the localStorage round trip',
);

assert.deepEqual(
  flagsOf(saved.get('trailing-mismatch')),
  [false, false],
  'a trailing assistant message that is not the recorded output stays unflagged',
);

// ---------------------------------------------------------------------------
// Request rebuilds: derive the consumers from the source rather than listing
// line numbers, so a new consumer is covered the day it is added.
// ---------------------------------------------------------------------------

const CONSUMERS = ['ReplayTab.tsx', 'CurlModal.tsx', 'ImproveTab.tsx', 'MessagesTab.tsx'];
const SYSTEM_FILTER = /m\.role !== 'system'/g;

for (const file of CONSUMERS) {
  const source = fs.readFileSync(path.join(root, 'src/components/inspect', file), 'utf8');
  let match;
  let seen = 0;
  while ((match = SYSTEM_FILTER.exec(source)) !== null) {
    seen++;
    const expression = source.slice(match.index, match.index + 120);
    assert.match(
      expression,
      /!m\.capturedOutput/,
      `${file}: a filter that drops the system message rebuilds the request, so it must drop the captured answer too`,
    );
  }
  if (file === 'ReplayTab.tsx' || file === 'CurlModal.tsx') {
    assert.ok(seen > 0, `${file} must still rebuild the request from trace messages`);
  }
}

const curlModal = fs.readFileSync(path.join(root, 'src/components/inspect/CurlModal.tsx'), 'utf8');
assert.doesNotMatch(
  curlModal,
  /input: userMsgs\.map\(\(m\) => m\.content\)/,
  'the responses preview must keep each role, not flatten messages to their content',
);
assert.match(curlModal, /input: userMsgs,/, 'the responses preview sends role-bearing messages');

const messagesTab = fs.readFileSync(path.join(root, 'src/components/inspect/MessagesTab.tsx'), 'utf8');
assert.match(
  messagesTab,
  /value: selectedTrace\.messages\.filter\(\(m\) => !m\.capturedOutput\)/,
  'the OpenInference export must not repeat the answer inside input.value',
);

console.log('inspect replay captured-output contract: all checks passed');
