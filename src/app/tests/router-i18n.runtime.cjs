const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const files = {
  editor: 'src/components/RouterEditorPanel.tsx',
  node: 'src/components/RouterNodeEditor.tsx',
  picker: 'src/components/RouterModelPicker.tsx',
  graph: 'src/components/RouterRuleGraph.tsx',
  manager: 'src/components/ModelManager.tsx',
  presentation: 'src/features/router/routerPresentation.ts',
};
const sources = Object.fromEntries(Object.entries(files).map(([key, rel]) => [key, fs.readFileSync(path.join(root, rel), 'utf8')]));
for (const rel of Object.values(files)) {
  const filename = path.join(root, rel);
  const compiled = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext, jsx: ts.JsxEmit.ReactJSX },
    fileName: filename,
    reportDiagnostics: true,
  });
  const errors = (compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error);
  assert.equal(errors.length, 0, errors.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'));
}
const en = JSON.parse(fs.readFileSync(path.join(root, 'src/i18n/locales/en-US/router.json'), 'utf8'));
const de = JSON.parse(fs.readFileSync(path.join(root, 'src/i18n/locales/de-DE/router.json'), 'utf8'));

assert.match(sources.editor, /useI18n\('router'\)/);
assert.match(sources.editor, /routerTranslateRef\.current = t;/, 'locale changes must not become Router hydration dependencies');
assert.doesNotMatch(sources.editor, /\[initialModel, t\]/);
assert.match(sources.picker, /picker\.staleValue/);
assert.match(sources.node, /node\.types\.keywordsAny/);
assert.match(sources.graph, /graph\.toolbox|graph\.emptyTitle/);
assert.match(sources.graph, /localizeRouterGraphError/);
assert.match(sources.graph, /\[blank, commit, node, reject, t\]/, 'localized graph rejection callback must track the active translator');
assert.match(sources.editor, /localizeRouterValidationMessage/);
assert.match(sources.editor, /localizeRouterImportError/);
assert.match(sources.presentation, /export function localizeRouterImportError/);
assert.match(sources.manager, /tRouter\('managerDelete\./);
assert.equal(en.graph.emptyTitle, 'Build the first condition');
assert.equal(de.graph.emptyTitle, 'Erste Bedingung erstellen');
assert.equal(en.managerDelete.title, 'Delete router definition?');
assert.equal(de.managerDelete.title, 'Router-Definition löschen?');
assert.equal(en.node.types.minChars, 'Minimum UTF-8 bytes');
assert.equal(de.node.types.minChars, 'Minimale UTF-8-Bytes');
assert.equal(en.importErrors.missingRouting, 'Router JSON is missing routing.');
assert.equal(de.importErrors.missingRouting, 'Im Router-JSON fehlt routing.');

for (const [name, source, stale] of [
  ['editor', sources.editor, ['<h3>Candidate Models</h3>', '<h3>Routing Strategy</h3>', '<strong>Natural-Language Router</strong>', 'Dismiss notification']],
  ['picker', sources.picker, ["placeholder = 'Select model'", 'Current value <code>']],
  ['node', sources.node, ['One keyword per line', 'Every available condition identity is already used in this gate']],
  ['graph', sources.graph, ['>Logic Gates</div>', 'placeholder="Search conditions"', '<strong>Build the first condition</strong>', '<strong>Node Inspector</strong>']],
  ['manager', sources.manager, ['title="Delete router definition?"']],
]) {
  for (const literal of stale) assert.equal(source.includes(literal), false, `${name} still contains raw Router UI: ${literal}`);
}
console.log('Router i18n integration checks passed.');
