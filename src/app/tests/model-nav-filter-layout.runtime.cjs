const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const navPath = path.join(root, 'src/components/ModelNavRail.tsx');
const listPath = path.join(root, 'src/components/ModelListPanel.tsx');
const managerPath = path.join(root, 'src/components/ModelManager.tsx');
const stylesPath = path.join(root, 'src/styles/styles.css');

const nav = fs.readFileSync(navPath, 'utf8');
const list = fs.readFileSync(listPath, 'utf8');
const manager = fs.readFileSync(managerPath, 'utf8');
const styles = fs.readFileSync(stylesPath, 'utf8');

for (const [fileName, source] of [[navPath, nav], [listPath, list], [managerPath, manager]]) {
  const compiled = ts.transpileModule(source, {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext, jsx: ts.JsxEmit.ReactJSX },
    fileName,
    reportDiagnostics: true,
  });
  const errors = (compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error);
  assert.equal(errors.length, 0, errors.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'));
}

assert.match(nav, /<span>\{mobileOpen \? 'Categories' : 'Task'\}<\/span>/);
assert.match(nav, /key: 'llm', label: 'Chat'/);
assert.match(nav, /key: 'router', label: 'Router'/);
assert.match(nav, /model-nav-rail__task-chip/);
assert.match(nav, /model-nav-rail__backend-chip/);
assert.match(nav, /model-nav-rail__tag-chip/);
assert.match(nav, /aria-pressed=\{active\}/);
assert.match(nav, /onTaskFiltersChange\(next\)/);
assert.match(nav, /onBackendFiltersChange\(next\)/);
assert.match(nav, /onTagFiltersChange\(next\)/);
assert.match(nav, /modelHasFilterableBackend\(m\)/);
assert.match(nav, /CUSTOM_TAGS_STORAGE_KEY/);
assert.match(nav, /addCustomTag/);
assert.match(nav, /Remove custom tag/);
assert.doesNotMatch(nav, /nav-backend-select/);
assert.doesNotMatch(nav, /\+ Recommended \+/);

assert.match(list, /export function modelMatchesTasks/);
assert.match(list, /export function modelMatchesBackends/);
assert.match(list, /export function modelMatchesTags/);
assert.match(list, /if \(filter === 'router'\) return modelIsRouter\(m\);/);
assert.match(list, /if \(filter === 'omni'\) return modelIsOmni\(m\);/);
assert.match(list, /if \(filter === 'llm'\) return cap === 'chat' && !modelIsRouter\(m\);/);
assert.match(list, /raw\.recommended === true \|\| raw\.is_recommended === true \|\| raw\.featured === true \|\| raw\.suggested === true/);

assert.match(manager, /const \[taskFilters, setTaskFilters\] = useState<Set<FilterTab>>/);
assert.match(manager, /const \[backendFilters, setBackendFilters\] = useState<Set<string>>/);
assert.match(manager, /const \[tagFilters, setTagFilters\] = useState<Set<string>>/);
assert.match(manager, /modelMatchesTasks\(info, taskFilters\)/);
assert.match(manager, /modelMatchesBackends\(info, backendFilters\)/);
assert.match(manager, /modelMatchesTags\(info, tagFilters\)/);

assert.match(styles, /\.model-nav-rail__chip-list\s*\{[^}]*flex-wrap:\s*wrap;/s);
assert.match(styles, /\.model-nav-rail__task-icon\s*\{[^}]*color:\s*var\(--filter-chip-color\);/s);
assert.match(styles, /\.model-nav-rail__backend-chip\s*\{[^}]*color:\s*var\(--filter-chip-color\);/s);

console.log('Model nav Task/Backend/Tag multi-select contract checks passed.');

function loadListHelpers() {
  const source = fs.readFileSync(listPath, 'utf8');
  const compiled = ts.transpileModule(source, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS,
      jsx: ts.JsxEmit.React,
      esModuleInterop: true,
    },
    fileName: listPath,
    reportDiagnostics: true,
  });
  const errors = (compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error);
  assert.equal(errors.length, 0, errors.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'));

  const capabilitiesPath = path.join(root, 'src/modelCapabilities.ts');
  const capSource = fs.readFileSync(capabilitiesPath, 'utf8');
  const capCompiled = ts.transpileModule(capSource, {
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
    fileName: capabilitiesPath,
  });
  const capModule = { exports: {} };
  Function('exports', 'require', 'module', capCompiled.outputText)(capModule.exports, require, capModule);

  const module = { exports: {} };
  const customRequire = request => {
    if (request === 'react') return { __esModule: true, default: {}, useCallback() {}, useEffect() {}, useRef() {}, useMemo() {}, useState() {} };
    if (request === '../modelCapabilities') return capModule.exports;
    if (request === './Icon') return { Icon() {}, CapabilityIcon() {} };
    if (request.includes('downloadStore')) return { activeDownloadForModel() { return undefined; } };
    if (request === './WorkspacePanels') return { WorkspaceActionButton() {}, WorkspaceActionGroup() {}, WorkspaceListPanel() {} };
    if (request === '../modelPresentation') return {
      backendColor: recipe => `color:${recipe}`,
      backendCompactLabel: recipe => recipe,
      backendLabel: recipe => recipe,
    };
    throw new Error(`Unexpected dependency while loading ModelListPanel helpers: ${request}`);
  };
  Function('exports', 'require', 'module', '__filename', '__dirname', compiled.outputText)(
    module.exports,
    customRequire,
    module,
    listPath,
    path.dirname(listPath),
  );
  return module.exports;
}

const helpers = loadListHelpers();
const chatModel = { id: 'Qwen-Chat', name: 'Qwen-Chat', labels: ['chat'], recipe: 'llamacpp', suggested: true };
const audioModel = { id: 'Whisper', name: 'Whisper', labels: ['audio'], recipe: 'whispercpp' };
const routerModel = { id: 'user.router', name: 'user.router', labels: ['custom'], recipe: 'collection.router' };
const omniModel = { id: 'user.omni', name: 'user.omni', labels: ['custom'], recipe: 'collection.omni' };

assert.equal(helpers.modelMatchesTasks(routerModel, new Set(['router'])), true);
assert.equal(helpers.modelMatchesTasks(routerModel, new Set(['llm'])), false, 'Router must not double-count as Chat');
assert.equal(helpers.modelMatchesTasks(chatModel, new Set(['llm', 'audio'])), true);
assert.equal(helpers.modelMatchesTasks(audioModel, new Set(['llm', 'audio'])), true);
assert.equal(helpers.modelMatchesBackends(chatModel, new Set(['llamacpp', 'whispercpp'])), true);
assert.equal(helpers.modelMatchesBackends(audioModel, new Set(['llamacpp', 'whispercpp'])), true);
assert.equal(helpers.modelHasFilterableBackend(routerModel), false);
assert.equal(helpers.modelHasFilterableBackend(omniModel), false);
assert.equal(helpers.modelMatchesTasks(omniModel, new Set(['omni'])), true);
assert.equal(helpers.modelMatchesTags(chatModel, new Set(['Recommended', 'Llama'])), true);
assert.equal(helpers.modelMatchesTags(audioModel, new Set(['Recommended', 'Llama'])), false);

console.log('Model nav filter behavior checks passed.');
