const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const componentPath = path.join(root, 'src/components/ModelListPanel.tsx');
const presentationPath = path.join(root, 'src/modelPresentation.ts');
const stylesPath = path.join(root, 'src/styles/styles.css');

const component = fs.readFileSync(componentPath, 'utf8');
const presentation = fs.readFileSync(presentationPath, 'utf8');
const styles = fs.readFileSync(stylesPath, 'utf8');

for (const [fileName, source] of [[componentPath, component], [presentationPath, presentation]]) {
  const compiled = ts.transpileModule(source, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2022,
      module: ts.ModuleKind.ESNext,
      jsx: ts.JsxEmit.ReactJSX,
    },
    fileName,
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
}

assert.match(component, /import \{ backendColor, backendCompactLabel, backendLabel \}/);
assert.match(component, /function modelListBackendLabel\(recipe: string\): string \{[\s\S]*backendCompactLabel\(recipe\)\.toUpperCase\(\)/);
assert.match(component, /const displayedBackend = recipe \? modelListBackendLabel\(recipe\) : '';/);
assert.match(component, /title=\{backendLabel\(recipe\)\}/);
assert.match(component, /\{displayedBackend\}/);

const bodyPosition = component.indexOf('className="model-list-item__body"');
const backendPosition = component.indexOf('className="model-list-item__backend"');
assert.ok(bodyPosition >= 0 && backendPosition > bodyPosition,
  'backend plate must render to the right of the name/capability block');

for (const expected of ['llama.cpp', 'vLLM', 'FLM', 'SD.cpp', 'Moonshine', 'OpenMOSS', 'Collection']) {
  assert.ok(presentation.includes(`compact: '${expected}'`), `missing complete compact backend label: ${expected}`);
}

assert.match(styles, /\.model-list-item\s*\{[^}]*display:\s*grid;[^}]*grid-template-columns:\s*minmax\(0, 1fr\) 82px auto auto;/s);
assert.match(styles, /\.model-list-item__backend\s*\{[^}]*grid-column:\s*2;[^}]*width:\s*82px;[^}]*text-transform:\s*uppercase;[^}]*white-space:\s*nowrap;/s);
assert.match(styles, /\.model-list-item__backend::before\s*\{[^}]*width:\s*2px;[^}]*background:\s*var\(--list-backend-color/s);
assert.match(styles, /\.model-list-item__backend::after\s*\{[^}]*background:\s*var\(--border-strong\);[^}]*mask-composite:\s*exclude;/s);
assert.match(styles, /\.model-list-item__body\s*\{[^}]*grid-column:\s*1;/s);

const backendBlock = styles.match(/\.model-list-item__backend\s*\{([\s\S]*?)\n\}/)?.[1] || '';
assert.doesNotMatch(backendBlock, /max-width:/);
assert.doesNotMatch(backendBlock, /text-overflow:\s*ellipsis/);
assert.doesNotMatch(backendBlock, /overflow:\s*hidden/);

console.log('Model list backend layout contract checks passed.');
