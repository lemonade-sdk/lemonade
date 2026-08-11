const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const componentPath = path.join(root, 'src/components/ModelListPanel.tsx');
const managerPath = path.join(root, 'src/components/ModelManager.tsx');
const panelsPath = path.join(root, 'src/components/WorkspacePanels.tsx');
const presentationPath = path.join(root, 'src/modelPresentation.ts');
const stylesPath = path.join(root, 'src/styles/styles.css');
const tokensPath = path.join(root, 'src/styles/tokens.css');

const component = fs.readFileSync(componentPath, 'utf8');
const manager = fs.readFileSync(managerPath, 'utf8');
const panels = fs.readFileSync(panelsPath, 'utf8');
const presentation = fs.readFileSync(presentationPath, 'utf8');
const styles = fs.readFileSync(stylesPath, 'utf8');
const tokens = fs.readFileSync(tokensPath, 'utf8');

for (const [fileName, source] of [
  [componentPath, component],
  [managerPath, manager],
  [panelsPath, panels],
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

/* ── Backend identity stays a text label ─────────────────────────── */

assert.match(component, /import \{ backendCompactLabel, backendLabel \}/);
assert.doesNotMatch(component, /backendColor|--list-backend-color/,
  'model rows must not carry backend identity colors');
assert.match(component, /function modelListBackendLabel\(recipe: string\): string \{[\s\S]*return backendCompactLabel\(recipe\);/);
assert.doesNotMatch(component, /backendCompactLabel\(recipe\)\.toUpperCase\(\)/,
  'backend labels must preserve product casing such as llama.cpp and vLLM');

/* ── Backend readiness still drives the row's status ──────────────── */

assert.match(component, /export function modelBackendReadiness\(/);
for (const state of ['installed', 'update_required', 'update_available', 'installable', 'action_required', 'unsupported']) {
  assert.ok(component.includes(`state === '${state}'`), `missing backend readiness handling for ${state}`);
}
assert.match(component, /tone: 'attention',[\s\S]*backend is not installed on this server/);
for (const state of ['missing', 'update_required', 'update_available', 'installable', 'action_required', 'unsupported']) {
  assert.ok(new RegExp(`${state}: '`).test(component),
    `every attention state needs a short message for the row's meta line: ${state}`);
}

/* ── The row is the shared component, filled by priority ──────────── */

assert.match(component, /<WorkspaceListRow/, 'the catalog must render the shared row');
assert.match(component, /capability=\{primaryCapability\}/,
  'the lead glyph carries the primary modality');

// The lead-glyph rule is shared, not restated per list.
const capabilities = fs.readFileSync(path.join(root, 'src/modelCapabilities.ts'), 'utf8');
assert.match(capabilities, /export function isRouterRecipe\(/);
assert.match(capabilities, /export function rowCapability\(model: ModelInfo\): ModelCapability \| 'router'/,
  'collections lead with their task identity, not a backend one');
assert.doesNotMatch(component, /function modelPrimaryCapability/,
  'the per-list copy of the lead-glyph rule is replaced by rowCapability');
assert.match(component, /const primaryCapability = rowCapability\(model\);/);
assert.match(component, /const neutralCollectionGuide = primaryCapability === 'omni' \|\| primaryCapability === 'router';/,
  'the collection check reuses the capability already computed for the row');
assert.match(component, /anchor=\{recipe && !neutralCollectionGuide \? displayedBackend : undefined\}/,
  'the engine is the meta-anchor and collections have none');
assert.match(component, /secondaryTags = capTags\.filter\(tag => tag !== \(primaryCapability as string\)\)/,
  'the primary modality must not repeat in the secondary glyph run');

// Status is never carried by dot color alone: a non-nominal state takes over
// the meta line, which is exactly the priority-5 metadata it outranks.
assert.match(component, /Downloading\$\{downloadPct != null \? ` \$\{downloadPct\.toFixed\(0\)\}%` : '…'\}/);
assert.match(component, /\? backendReadinessMessage\(backendReadiness!\)/);
assert.match(component, /glyphs=\{rowStatus === 'busy' \|\| rowStatus === 'attention'/,
  'a status message replaces the glyph run rather than sharing the line with it');
assert.match(panels, /function rowTone\(status\?: WorkspaceListRowStatus\)/,
  'tone follows from status inside the shared row, not a prop each list repeats');
assert.doesNotMatch(component + manager + panels, /metaTone=/,
  'metaTone is derived, never passed');

assert.match(component, /aria-label=|ariaLabel=\{`\$\{displayName\}[\s\S]*\$\{readinessLabel \? `, \$\{readinessLabel\}` : ''\}`\}/);
assert.match(component, /icon: 'pin'/, 'the row action is the pin');
assert.match(component, /ariaKeyShortcuts=\{onTogglePin \? 'P' : undefined\}/,
  'the keyboard pin shortcut must survive the row rewrite');

assert.doesNotMatch(component, /model-list-item__(footer|body|status|backend|pin|caps)/,
  'the per-tab catalog row markup is retired in favour of the shared row');

/* ── The shared row component's own contract ─────────────────────── */

assert.match(panels, /export const WorkspaceListRow/);
assert.match(panels, /export const WorkspaceList/);
assert.match(panels, /role="option"/);
assert.match(panels, /role="listbox"/);
assert.match(panels, /--workspace-list-row-cap/,
  'the lead glyph is tinted through a custom property, not a hard-coded hue');
assert.match(panels, /event\.stopPropagation\(\); action\.onClick\(\)/,
  'the row action must not also select the row');

/* ── The list owns roving tabindex, once ─────────────────────────
   Every selection list used to carry its own copy of the ARIA listbox
   keyboard contract, and the copies had already drifted. WorkspaceList owns
   it now; a list supplies policy and callbacks only. */

assert.match(panels, /data-row-id/, 'rows are identified to the list uniformly');
for (const key of ['ArrowDown', 'ArrowUp', 'Home', 'End', 'Enter']) {
  assert.ok(panels.includes(`'${key}'`), `WorkspaceList must handle ${key}`);
}
assert.match(panels, /wrap = false, activateOnMove = false/,
  'wrap and select-on-move are per-list policy, not forked implementations');

const chatView = fs.readFileSync(path.join(root, 'src/components/ChatView.tsx'), 'utf8');

for (const [name, source] of [
  ['ModelListPanel.tsx', component],
  ['ModelManager.tsx', manager],
  ['ChatView.tsx', chatView],
  ['TraceList.tsx', fs.readFileSync(path.join(root, 'src/components/inspect/TraceList.tsx'), 'utf8')],
]) {
  assert.doesNotMatch(source, /querySelectorAll<HTMLElement>\('\[role="option"\]'\)/,
    `${name} must not re-implement listbox keyboard navigation`);
}

/* ── The chat model picker is a selection list too ───────────────── */

assert.match(chatView, /className="composer__model-results"[\s\S]{0,200}label="Models"/,
  'the model picker renders through WorkspaceList');
assert.doesNotMatch(chatView, /composer__model-option\b|composer__model-option-row/,
  'the picker must not keep a private option row');
assert.doesNotMatch(styles, /\.composer__model-option/,
  'the picker option styles are replaced by the shared row');
assert.match(chatView, /icon: 'eject'/, 'eject occupies the row action slot');
assert.match(chatView, /const isCollection = capability === 'omni' \|\| capability === 'router'/,
  'the picker suppresses the engine anchor for collections, like the catalog');

assert.match(manager, /const \[systemInfo, setSystemInfo\] = useState<Record<string, unknown> \| null>/);
assert.match(manager, /setSystemInfo\(info\)/);
assert.match(manager, /systemInfo=\{systemInfo\}/);

for (const expected of ['llama.cpp', 'vLLM', 'FLM', 'SD.cpp', 'Moonshine', 'OpenMOSS', 'Collection']) {
  assert.ok(presentation.includes(`compact: '${expected}'`), `missing complete compact backend label: ${expected}`);
}

/* ── Row geometry ────────────────────────────────────────────────── */

assert.match(styles, /\.workspace-list-row\s*\{[^}]*display:\s*grid;[^}]*grid-template-columns:\s*16px minmax\(0, 1fr\) 18px;[^}]*min-height:\s*52px;/s,
  'the row is a fixed three-column grid so nothing reflows between states');
assert.match(styles, /\.workspace-list-row__lead\s*\{[^}]*grid-row:\s*1 \/ span 2;/s);
assert.match(styles, /\.workspace-list-row__title\s*\{[^}]*grid-row:\s*1;[^}]*font-size:\s*var\(--text-sm\);/s,
  'every list in the app sets the row title at one size');
assert.match(styles, /\.workspace-list-row__status\s*\{[^}]*grid-column:\s*3;[^}]*grid-row:\s*1;/s);
assert.match(styles, /\.workspace-list-row__action\s*\{[^}]*grid-column:\s*3;[^}]*grid-row:\s*2;/s,
  'status and action stack in the trailing column so revealing one never shifts the title');
assert.match(styles, /\.workspace-list-row__status--absent::before\s*\{[^}]*width:\s*9px;[^}]*height:\s*9px;[^}]*background:\s*transparent;/s);
assert.match(styles, /\.workspace-list-row__status::before\s*\{[^}]*width:\s*7px;[^}]*height:\s*7px;/s);
assert.match(styles, /\.workspace-list-row--selected[^{]*\{[^}]*--workspace-list-row-bg:[^;]+;\s*border-color:\s*color-mix\(in srgb, var\(--accent-fg\) 30%/s,
  'selection is one treatment shared by every list');

/* Hover and selection must stay translucent. The five lists do not share a
   ground — the catalog scrolls over --surface-base, the chat and telemetry
   rails are --surface-1, the composer's picker floats on --surface-raised, and
   the light theme collapses --surface-base and --surface-1 onto one color — so
   an opaque value tuned against one list is invisible or inverted on another. */
const stateToken = (label, pattern) => {
  const match = styles.match(pattern);
  assert.ok(match, `${label} must set --workspace-list-row-bg from a single token`);
  assert.match(match[1], /^--[a-z0-9-]+$/, `${label} token name looks wrong: ${match[1]}`);
  return match[1];
};
const rowStateTokens = [
  ['hover', stateToken('row hover',
    /\.workspace-list-row:hover\s*\{\s*--workspace-list-row-bg:\s*var\((--[a-z0-9-]+)\)/)],
  ['selection', stateToken('row selection',
    /\.workspace-list-row--selected,\s*\n\.workspace-list-row--selected:hover\s*\{\s*--workspace-list-row-bg:\s*var\((--[a-z0-9-]+)\)/)],
];
for (const [state, token] of rowStateTokens) {
  const values = [...tokens.matchAll(new RegExp(`${token}:\\s*([^;]+);`, 'g'))].map(m => m[1].trim());
  assert.ok(values.length >= 2, `${token} must be defined for both themes, found ${values.length}`);
  for (const value of values) {
    assert.match(value, /^rgba\(.*,\s*0?\.\d+\s*\)$/,
      `row ${state} resolves to "${value}" via ${token}; row states must be translucent so `
      + 'one rule reads the same on every list ground and in both themes');
  }
}

/* Selection outranks hover. These share specificity, so the cascade resolves
   them by source order and the order is the whole fix. */
const hoverAt = styles.indexOf('.workspace-list-row:hover');
const selectedHoverAt = styles.indexOf('.workspace-list-row--selected:hover');
assert.ok(hoverAt >= 0 && selectedHoverAt > hoverAt,
  'a selected row must keep its selected background while the pointer rests on it, '
  + 'so --selected:hover has to come after :hover');
assert.match(styles, /\.workspace-list-row--selected,\s*\n\.workspace-list-row--selected:hover\s*\{/,
  'a selected row must render identically hovered and unhovered — selection is '
  + 'persistent state, hover is a transient affordance for rows you can move to');
assert.ok(
  styles.indexOf('.workspace-list-row--selected.workspace-list-row--disabled:hover') > selectedHoverAt,
  'a selected row that is busy must not lose its selection on hover');

/* Degradation is by priority, driven by the list container. */
assert.match(styles, /\.workspace-list\s*\{[^}]*container-type:\s*inline-size;[^}]*container-name:\s*workspace-list;/s);
assert.match(styles, /@container workspace-list \(max-width: 260px\)\s*\{\s*\.workspace-list-row__glyphs > :nth-child\(n \+ 3\)/);
assert.match(styles, /@container workspace-list \(max-width: 210px\)\s*\{\s*\.workspace-list-row__glyphs\s*\{\s*display:\s*none/);

/* ── Retired treatments must not come back ───────────────────────── */

assert.doesNotMatch(styles, /\.model-list-item|\.rail__item|--model-list-line/,
  'the per-tab row styles are replaced by the shared row');
assert.doesNotMatch(styles, /\.workspace-list-row__footer|\.workspace-list-row__guide/,
  'the gradient guide line and its terminus are retired');
assert.doesNotMatch(styles, /--list-backend-color/,
  'the backend label must not regain an identity color');

console.log('Shared list row layout, status, and readiness contract checks passed.');
