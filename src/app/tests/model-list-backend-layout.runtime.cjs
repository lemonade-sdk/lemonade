const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const componentPath = path.join(root, 'src/components/ModelListPanel.tsx');
const managerPath = path.join(root, 'src/components/ModelManager.tsx');
const presentationPath = path.join(root, 'src/modelPresentation.ts');
const stylesPath = path.join(root, 'src/styles/styles.css');

const component = fs.readFileSync(componentPath, 'utf8');
const manager = fs.readFileSync(managerPath, 'utf8');
const presentation = fs.readFileSync(presentationPath, 'utf8');
const styles = fs.readFileSync(stylesPath, 'utf8');

for (const [fileName, source] of [
  [componentPath, component],
  [managerPath, manager],
  [presentationPath, presentation],
]) {
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

assert.match(component, /import \{ backendCompactLabel, backendLabel \}/);
assert.doesNotMatch(component, /backendColor|--list-backend-color/,
  'model rows must not carry backend identity colors');
assert.match(component, /function modelListBackendLabel\(recipe: string\): string \{[\s\S]*return backendCompactLabel\(recipe\);/);
assert.doesNotMatch(component, /backendCompactLabel\(recipe\)\.toUpperCase\(\)/,
  'backend labels must preserve product casing such as llama.cpp and vLLM');
assert.match(component, /export function modelBackendReadiness\(/);
for (const state of ['installed', 'update_required', 'update_available', 'installable', 'action_required', 'unsupported']) {
  assert.ok(component.includes(`state === '${state}'`), `missing backend readiness handling for ${state}`);
}
assert.match(component, /tone: 'attention',[\s\S]*backend is not installed on this server/);
assert.match(component, /className="model-list-item__footer"/);
assert.match(component, /const neutralCollectionGuide = modelIsOmni\(model\) \|\| modelIsRouter\(model\);/);
assert.match(component, /className="model-list-item__footer-info"[\s\S]*recipe && !neutralCollectionGuide[\s\S]*className="model-list-item__backend"[\s\S]*\{displayedBackend\}/);
assert.match(component, /model-list-item__status model-list-item__status--\$\{statusTone\}/);
assert.doesNotMatch(component, /model-list-item__dot/,
  'active readiness must replace the hollow ring instead of nesting a dot inside it');
assert.match(component, /data-backend-state=\{backendReadiness\?\.state \|\| statusTone\}/);
assert.match(component, /aria-label=\{`\$\{displayName\}[\s\S]*\$\{readinessLabel \? `, \$\{readinessLabel\}` : ''\}`\}/);
assert.match(component, /model-list-item__pin row__pin[\s\S]*?<Icon name="pin" size=\{12\}/);

const bodyPosition = component.indexOf('className="model-list-item__body"');
const footerPosition = component.indexOf('className="model-list-item__footer"');
const pinPosition = component.indexOf('model-list-item__pin row__pin');
assert.ok(bodyPosition >= 0 && footerPosition > bodyPosition && pinPosition > footerPosition,
  'model information, lower backend guide, and upper-right pin must render in that order');

assert.match(manager, /const \[systemInfo, setSystemInfo\] = useState<Record<string, unknown> \| null>/);
assert.match(manager, /setSystemInfo\(info\)/);
assert.match(manager, /systemInfo=\{systemInfo\}/);

for (const expected of ['llama.cpp', 'vLLM', 'FLM', 'SD.cpp', 'Moonshine', 'OpenMOSS', 'Collection']) {
  assert.ok(presentation.includes(`compact: '${expected}'`), `missing complete compact backend label: ${expected}`);
}

assert.match(styles, /\.model-list-item\s*\{[^}]*position:\s*relative;[^}]*min-height:\s*54px;[^}]*display:\s*block;[^}]*padding:[^;]*15px/s);
assert.match(styles, /\.model-list-item__footer\s*\{[^}]*position:\s*absolute;[^}]*inset-block-end:\s*5px;/s);
assert.match(styles, /\.model-list-item__footer::before\s*\{[^}]*inset-inline:\s*0 5px;[^}]*height:\s*1px;[^}]*linear-gradient\([^}]*transparent 0%[^}]*var\(--model-list-line\) 100%/s);
assert.doesNotMatch(styles, /--list-backend-color|\.model-list-item--neutral-guide \.model-list-item__footer::before/,
  'the lower guide must be one neutral right-to-left fade for every model row');
assert.doesNotMatch(styles, /\.model-list-item__footer::after/,
  'the neutral guide must remain a single one-pixel paint layer');
assert.match(styles, /\.model-list-item__footer-info\s*\{[^}]*inset-inline-end:\s*16px;[^}]*display:\s*inline-flex;[^}]*font-weight:\s*var\(--weight-regular\);/s);
assert.match(styles, /\.model-list-item__backend\s*\{[^}]*text-transform:\s*none;/s);
assert.match(styles, /\.model-list-item__status\s*\{[^}]*inset-inline-end:\s*0;[^}]*width:\s*11px;[^}]*display:\s*grid;[^}]*place-items:\s*center;/s);
assert.match(styles, /\.model-list-item__status--available::before\s*\{[^}]*width:\s*9px;[^}]*height:\s*9px;[^}]*border:\s*1px solid var\(--model-list-line\);/s);
assert.match(styles, /\.model-list-item__status--ready::before,[\s\S]*\.model-list-item__status--downloading::before\s*\{[^}]*width:\s*7px;[^}]*height:\s*7px;[^}]*border:\s*0;/s);
assert.match(styles, /\.model-list-item__status--attention::before\s*\{[^}]*background:\s*var\(--status-warning\);/s);
assert.doesNotMatch(styles, /\.model-list-item__dot/,
  'the status marker must use one painted shape at every state');
assert.match(styles, /\.model-list-item__pin\s*\{[^}]*position:\s*absolute;[^}]*inset-block-start:\s*6px;[^}]*inset-inline-end:\s*6px;[^}]*border:\s*0;/s);
assert.doesNotMatch(styles, /model-list-item__backend-cluster/,
  'the previous custom backend badge/cluster design must be removed');
assert.doesNotMatch(styles, /clip-path:\s*polygon\([\s\S]*model-list-item__backend/,
  'the backend label must not retain the cut-corner badge geometry');

console.log('Model list backend line/readiness layout contract checks passed.');
