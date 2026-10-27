const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const helperPath = path.join(root, 'src/features/browserPrompt.ts');
const source = fs.readFileSync(helperPath, 'utf8');
const compiled = ts.transpileModule(source, {
  fileName: helperPath,
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  reportDiagnostics: true,
});
assert.equal((compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error).length, 0);
const moduleValue = { exports: {} };
Function('exports', 'module', compiled.outputText)(moduleValue.exports, moduleValue);
const prompt = moduleValue.exports;
const values = new Map();
let location;
let historyCalls;
global.sessionStorage = {
  getItem: key => values.has(key) ? values.get(key) : null,
  setItem: (key, value) => { values.set(key, String(value)); },
  removeItem: key => { values.delete(key); },
};
function navigate(href) {
  location = new URL(href);
  historyCalls = 0;
  global.window = {
    location: {
      get href() { return location.href; },
      get search() { return location.search; },
    },
    history: {
      state: { app: true },
      replaceState(state, _title, url) {
        assert.deepEqual(state, { app: true });
        location = new URL(url);
        historyCalls++;
      },
    },
  };
}

navigate('http://localhost:13305/?q=Summarize%20article&view=models&logs=true&source=firefox#/models');
assert.equal(prompt.readBrowserPromptDraft(), 'Summarize article');
prompt.captureBrowserPromptFromUrl();
assert.equal(location.search, '?source=firefox');
assert.equal(location.hash, '#/chat');
assert.equal(historyCalls, 1);
assert.equal(prompt.readBrowserPromptDraft(), 'Summarize article', 'pending draft survives a reload');
assert.equal(values.get(prompt.BROWSER_PROMPT_DRAFT_KEY), 'Summarize article');
assert.match(prompt.BROWSER_PROMPT_DRAFT_KEY, /^lemonade:/, 'Clear permitted local data must include the pending draft');
prompt.captureBrowserPromptFromUrl();
assert.equal(historyCalls, 1, 'refresh must not push or rewrite the clean URL');
prompt.saveBrowserPromptDraft('Summarize revised text');
assert.equal(prompt.readBrowserPromptDraft(), 'Summarize revised text', 'edits persist in the same tab');
prompt.saveBrowserPromptDraft('');
assert.equal(prompt.readBrowserPromptDraft(), '', 'submission and cancellation clear the draft');

prompt.saveBrowserPromptDraft('stale');
navigate('http://localhost:13305/?q=New%20task');
prompt.captureBrowserPromptFromUrl();
assert.equal(prompt.readBrowserPromptDraft(), 'New task', 'new URL prompt takes precedence');
navigate('http://localhost:13305/?q=');
prompt.captureBrowserPromptFromUrl();
assert.equal(prompt.readBrowserPromptDraft(), '', 'empty new URL prompt clears stale content');

assert.equal(prompt.captureBrowserDraftSend(undefined, false, 'pending', false), true);
assert.equal(prompt.captureBrowserDraftSend(undefined, true, 'pending', false), true);
assert.equal(prompt.captureBrowserDraftSend('option-response', false, 'pending', false), false);
assert.equal(prompt.captureBrowserDraftSend(undefined, false, 'pending', true), false);
assert.equal(prompt.captureBrowserDraftSend(undefined, false, '', false), false);
assert.equal(prompt.hasBrowserPromptContext([]), false);
assert.equal(prompt.hasBrowserPromptContext([{ role: 'user' }]), false);
assert.equal(prompt.hasBrowserPromptContext([{ fromBrowserPrompt: true }, { role: 'assistant' }]), true);
assert.equal(prompt.hasBrowserPromptContext([{ role: 'user' }, { fromBrowserPrompt: true }]), true);

const chat = fs.readFileSync(path.join(root, 'src/components/ChatView.tsx'), 'utf8');
const app = fs.readFileSync(path.join(root, 'src/App.tsx'), 'utf8');
assert.ok(app.indexOf('captureBrowserPromptFromUrl();') < app.indexOf('const App: React.FC'), 'capture must happen before first render');
assert.match(chat, /readBrowserPromptDraft/);
assert.match(chat, /current \|\| \(incomingPrompt && !incomingPromptHandledRef\.current \? null : storedActiveId\)/);
assert.match(chat, /if \(!isVisible \|\| activeId \|\| !incomingPrompt/);
assert.match(chat, /const isExternalPrompt = captureBrowserDraftSend\(overrideText/);
assert.match(chat, /const audioFiles = isExternalPrompt \? \[\]/);
assert.match(chat, /const hasImages = !isExternalPrompt/);
assert.match(chat, /if \(!isExternalPrompt\) \{\s*setPendingImages\(\[\]\);/);
assert.match(chat, /if \(overrideText === undefined\) setInputValue\(''\)/);
assert.match(chat, /const allowTools = !hasBrowserPromptContext\(\[\.\.\.priorMessages, userMessage\]\)/);
assert.match(chat, /disabled=\{!modeSupportsMcp \|\| toolsUnavailable\}/);
assert.match(chat, /saveBrowserPromptDraft\(draft\);/);
assert.match(chat, /saveBrowserPromptDraft\(''\);/);
assert.doesNotMatch(chat, /incomingPromptHandledRef\.current = true;\s*setShowLoadedOverview/,
  'auto effect must leave consumption to successful send');

for (const [name, text] of [['App.tsx', app], ['ChatView.tsx', chat]]) {
  const result = ts.transpileModule(text, {
    fileName: name,
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext, jsx: ts.JsxEmit.ReactJSX },
    reportDiagnostics: true,
  });
  assert.deepEqual((result.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error), [], `${name} syntax`);
}
console.log('Firefox browser prompt regression tests passed');
