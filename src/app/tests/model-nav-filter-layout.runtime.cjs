const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const navPath = path.join(root, 'src/components/ModelNavRail.tsx');
const listPath = path.join(root, 'src/components/ModelListPanel.tsx');
const managerPath = path.join(root, 'src/components/ModelManager.tsx');
const backendVisibilityPath = path.join(root, 'src/features/models/backendRailVisibility.ts');
const stylesPath = path.join(root, 'src/styles/styles.css');
const a11yPath = path.join(root, 'tests/a11y.spec.ts');

const nav = fs.readFileSync(navPath, 'utf8');
const list = fs.readFileSync(listPath, 'utf8');
const manager = fs.readFileSync(managerPath, 'utf8');
const backendVisibility = fs.readFileSync(backendVisibilityPath, 'utf8');
const styles = fs.readFileSync(stylesPath, 'utf8');
const a11y = fs.readFileSync(a11yPath, 'utf8');

for (const [fileName, source] of [[navPath, nav], [listPath, list], [managerPath, manager], [backendVisibilityPath, backendVisibility], [a11yPath, a11y]]) {
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
assert.match(nav, /visibleBackends\.map/);
assert.match(nav, /model-nav-rail__backend-more/);
assert.match(nav, /hiddenBackendCount > 0/);
assert.match(nav, /backendShortcutsRemovable/);
assert.match(nav, /model-nav-rail__backend-remove/);
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
assert.match(list, /const FILTER_TABS:[\s\S]*?key: 'llm', label: 'Chat'[\s\S]*?key: 'embedding', label: 'Embed'/,
  'task filter labels must remain defined because the selected-task live region depends on FILTER_TABS');
assert.match(list, /Array\.from\(taskFilters\)\.map\(task => FILTER_TABS\.find/,
  'selected task labels must remain wired to FILTER_TABS');
assert.doesNotMatch(list, /model-list-panel__filter-btn/, 'middle panel must not expose a duplicate filter control (#2936)');
assert.doesNotMatch(list, /TextFilterRule/, 'removed middle-panel text-filter implementation must not linger as dead code');
assert.doesNotMatch(list, /capabilityFilter/, 'middle-panel capability filter state must not linger after #2936');

assert.match(manager, /const \[taskFilters, setTaskFilters\] = useState<Set<FilterTab>>/);
assert.match(manager, /const \[backendFilters, setBackendFilters\] = useState<Set<string>>/);
assert.match(manager, /const \[tagFilters, setTagFilters\] = useState<Set<string>>/);
assert.match(manager, /modelMatchesTasks\(info, taskFilters\)/);
assert.match(manager, /modelMatchesBackends\(info, backendFilters\)/);
assert.match(manager, /modelMatchesTags\(info, tagFilters\)/);
assert.doesNotMatch(manager, /capabilityFilter/, 'ModelManager must not retain hidden funnel state after #2936');

assert.match(styles, /\.model-nav-rail__chip-list\s*\{[^}]*flex-wrap:\s*wrap;/s);
assert.match(styles, /\.model-nav-rail__task-icon\s*\{[^}]*color:\s*var\(--filter-chip-color\);/s);
assert.doesNotMatch(nav, /backendColor/, 'backend filters must not receive per-backend identity colors');
assert.match(styles, /\.model-nav-rail__backend-chip\s*\{[^}]*--filter-chip-color:\s*var\(--text-tertiary\);[^}]*color:\s*var\(--text-secondary\);/s);
assert.match(styles, /\.model-nav-rail__backend-chip\.is-active\s*\{[^}]*color:\s*var\(--text-primary\);/s);
assert.doesNotMatch(styles, /\.model-nav-rail__backend-chip::before\s*\{/,
  'backend filters must stay neutral without introducing a colored identity marker');
assert.match(styles, /\.model-nav-rail__backend-wrap\.is-removable \.model-nav-rail__backend-chip\s*\{[^}]*padding-inline-end:\s*19px;/s);
assert.match(styles, /\.model-nav-rail__backend-remove,/);
assert.doesNotMatch(styles, /\.model-list-panel__filter-(?:btn|popover|option|clear)/, 'removed middle filter styles must not remain');

assert.doesNotMatch(a11y, /A95 — funnel filter button has aria-expanded and aria-haspopup/,
  'Playwright must not require the #2936-removed middle-panel funnel button');
assert.doesNotMatch(a11y, /A96 — funnel filter popover opens on button click and has role=dialog/,
  'Playwright must not require the #2936-removed funnel dialog');
assert.match(a11y, /A95 — models middle panel has no duplicate funnel dialog trigger/,
  'Playwright should lock in the absence of the duplicate middle-panel filter');
assert.match(a11y, /A96 — left-rail task filtering remains usable after funnel removal/,
  'Playwright should exercise the surviving left-rail task filter path');
assert.match(a11y, /model-list-panel__count[\s\S]*?toContainText\('\(Chat\)'\)/,
  'the regression test must render the selected task label, catching missing FILTER_TABS at runtime');

const visibilityCompiled = ts.transpileModule(backendVisibility, {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
  fileName: backendVisibilityPath,
});
const visibilityModule = { exports: {} };
Function('exports', 'require', 'module', visibilityCompiled.outputText)(
  visibilityModule.exports,
  require,
  visibilityModule,
);

const {
  DEFAULT_VISIBLE_BACKEND_COUNT,
  resolveBackendRailVisibility,
  revealNextBackend,
  hideBackendShortcut,
} = visibilityModule.exports;

const backendValues = ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'];
const initialVisibility = resolveBackendRailVisibility(backendValues, null);
assert.equal(DEFAULT_VISIBLE_BACKEND_COUNT, 6);
assert.deepEqual(initialVisibility, { order: backendValues, visibleCount: 6 });

const sevenVisible = revealNextBackend(initialVisibility);
assert.equal(sevenVisible.visibleCount, 7, 'the plus pill reveals exactly one backend');

const curatedVisibility = hideBackendShortcut(sevenVisible, 'a');
assert.equal(curatedVisibility.visibleCount, 6);
assert.deepEqual(curatedVisibility.order, ['b', 'c', 'd', 'e', 'f', 'g', 'h', 'a']);
assert.deepEqual(
  revealNextBackend(curatedVisibility).order.slice(0, 7),
  ['b', 'c', 'd', 'e', 'f', 'g', 'h'],
  'removing a top backend must advance to the next hidden backend instead of immediately restoring it',
);

assert.deepEqual(
  hideBackendShortcut(initialVisibility, 'a'),
  initialVisibility,
  'backend shortcuts cannot be removed while only the default six are visible',
);

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
    if (request === 'react') return {
      __esModule: true,
      default: { createElement: (...args) => ({ type: args[0], props: args[1], children: args.slice(2) }) },
      useCallback(fn) { return fn; },
      useEffect() {},
      useRef(value = null) { return { current: value }; },
      useMemo(factory) { return factory(); },
      useState(initial) { return [typeof initial === 'function' ? initial() : initial, () => {}]; },
    };
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

assert.doesNotThrow(() => helpers.ModelListPanel({
  allModels: [chatModel, audioModel, routerModel, omniModel],
  loadedNames: new Set(),
  pulling: {},
  downloadItems: [],
  selectedModelId: null,
  onSelectModel() {},
  searchQuery: '',
  onSearchChange() {},
  taskFilters: new Set(['llm']),
  primaryFilter: 'all',
  backendFilters: new Set(),
  tagFilters: new Set(),
}), 'selecting a left-rail task must render the model list without throwing');

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
