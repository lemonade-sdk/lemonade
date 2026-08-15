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
assert.match(component, /list\.readiness\.\$\{backendReadiness\?\.state \|\| 'attention'\}/,
  'backend attention messages must resolve through the translation catalog');

/* ── The row is the shared component, filled by priority ──────────── */

assert.match(component, /<WorkspaceListRow/, 'the catalog must render the shared row');
assert.match(component, /capability=\{primaryCapability\}/,
  'the lead glyph carries the primary modality');

// The lead-glyph rule is shared, not restated per list.
const capabilities = fs.readFileSync(path.join(root, 'src/modelCapabilities.ts'), 'utf8');
assert.match(capabilities, /export function isRouterRecipe\(/);
assert.match(capabilities, /export function identityFromModelInfo\(model: ModelInfo\): ModelIdentity/,
  'collections lead with their task identity, not a backend one');
assert.match(capabilities, /export type ModelIdentity = ModelCapability \| 'omni' \| 'router'/,
  'omni and router are structural identities, not capabilities');
assert.doesNotMatch(component, /function modelPrimaryCapability/,
  'the per-list copy of the lead-glyph rule is replaced by rowCapability');
assert.match(component, /const primaryCapability = identityFromModelInfo\(model\);/);
assert.match(component, /const neutralCollectionGuide = isCollectionRecipe\(recipe\);/,
  'the collection check reuses the shared structural predicate');
assert.match(component, /anchor=\{recipe && !neutralCollectionGuide \? displayedBackend : undefined\}/,
  'the engine is the meta-anchor and collections have none');
assert.match(component, /secondaryTags = capTags\.filter\(tag => tag !== \(primaryCapability as string\)\)/,
  'the primary modality must not repeat in the secondary glyph run');

// Status is transient-only, and never carried by dot colour alone: a row that
// is doing something says so in words, and a steady row says nothing at all.
assert.match(component, /list\.readiness\.statusDownloading/);
assert.match(component, /list\.readiness\.\$\{backendReadiness\?\.state \|\| 'attention'\}/);
assert.match(panels, /export type WorkspaceListRowStatus = 'live' \| 'busy' \| 'attention' \| 'error';/,
  'the status vocabulary is only the states that render; steady states are section facts');
assert.doesNotMatch(component + manager, /status=\{[^}]*'(absent|ready|unknown)'/,
  'being downloaded is a fact about the section, not a status on every row');
assert.match(component, /statusText=\{statusText\}/,
  'the catalog hands its state to statusText, not to the facts slot');
assert.match(component, /const meta = model\.size/,
  'the meta slot carries facts only; state lives in statusText');

/* Sections carry what every row in a run shares. Grouping is structural so the
   headings stay true under every sort, not just the status-ranked default. */
assert.match(component, /const listSections = useMemo/,
  'the catalog groups its rows into labelled sections');
for (const key of ['list.sections.pinned', 'list.sections.downloaded', 'list.sections.available']) {
  assert.ok(component.includes(`t('${key}')`), `missing localized catalog section: ${key}`);
}
assert.match(panels, /<li role="group" aria-label=\{label\}/,
  'a section is a listbox group so its rows stay addressable');
assert.match(panels, /<ul role="none" className="workspace-list-group__items">/,
  'the inner list drops its own semantics so rows attach to the group');
assert.match(panels, /const metaParts = showStatus/,
  'a status message replaces the glyph run inside the row, not at each call site');
assert.match(panels, /workspace-list-row__status--\$\{status\}/,
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
/* A list you pick from is a listbox of options; a readout you merely act on is
   a plain list, where the row's own button is a legal control rather than an
   illegal nested one. Same row template either way — only the roles differ. */
assert.match(panels, /role=\{selectable \? 'option' : 'listitem'\}/,
  'the row takes its role from whether the list is selectable');
assert.match(panels, /role=\{selectable \? 'listbox' : 'list'\}/,
  'the list takes its role from whether its rows are chosen from');
assert.match(panels, /aria-selected=\{selectable \? selected : undefined\}/,
  'a plain list item must not claim a selection state it cannot have');
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

assert.match(chatView, /className="composer__model-results"[\s\S]{0,200}label=\{t\('model\.models'\)\}/,
  'the model picker renders through WorkspaceList');
assert.doesNotMatch(chatView, /composer__model-option\b|composer__model-option-row/,
  'the picker must not keep a private option row');
assert.doesNotMatch(styles, /\.composer__model-option/,
  'the picker option styles are replaced by the shared row');
assert.match(chatView, /icon: 'eject'/, 'eject occupies the row action slot');
assert.match(chatView, /const isCollection = structure !== 'single'/,
  'the picker suppresses the engine anchor for collections, like the catalog');

assert.match(manager, /const \[systemInfo, setSystemInfo\] = useState<Record<string, unknown> \| null>/);
assert.match(manager, /setSystemInfo\(info\)/);
assert.match(manager, /systemInfo=\{systemInfo\}/);

for (const expected of ['llama.cpp', 'vLLM', 'FLM', 'SD.cpp', 'Moonshine', 'OpenMOSS', 'Collection']) {
  assert.ok(presentation.includes(`compact: '${expected}'`), `missing complete compact backend label: ${expected}`);
}

/* ── Row geometry ────────────────────────────────────────────────── */

assert.match(styles, /\.workspace-list-row\s*\{[^}]*display:\s*grid;[^}]*grid-template-columns:\s*16px minmax\(0, 1fr\);[^}]*min-height:\s*52px;/s,
  'the row is a lead glyph and a flexible body; the action shares the anchor slot');
assert.match(styles, /\.workspace-list-row__lead\s*\{[^}]*grid-row:\s*1 \/ span 2;/s);
assert.match(styles, /\.workspace-list-row__title\s*\{[^}]*grid-column:\s*2;[^}]*grid-row:\s*1;[^}]*padding-inline-end:\s*var\(--workspace-list-row-action\);[^}]*font-size:\s*var\(--text-sm\);/s,
  'the title owns its line and reserves the action width so the two never collide');
assert.match(styles, /\.workspace-list-row__anchor\s*\{[^}]*margin-inline-start:\s*auto;/s,
  'the anchor ends the meta line rather than competing with the title');

/* The dot and its words are one unit inside the meta line. Split apart — dot in
   the trailing column, words in the meta line — colour carried state on its own
   and the mark sat diagonally away from what it marked. */
assert.match(styles, /\.workspace-list-row__status\s*\{[^}]*display:\s*inline-flex;[^}]*align-items:\s*center;/s,
  'the status dot and its words render as one inline unit');
assert.doesNotMatch(styles, /\.workspace-list-row__status\s*\{[^}]*grid-column:/s,
  'the status dot must not return to its own grid cell away from its words');

/* The action overlays the row's right edge at full height instead of owning a
   column. A column reserved on every row put a button beside every timestamp in
   the app, and stopped the anchor short of the edge. */
assert.match(styles, /\.workspace-list-row__action\s*\{[^}]*position:\s*absolute;[^}]*inset-block:\s*0;[^}]*width:\s*var\(--workspace-list-row-action\);[^}]*visibility:\s*hidden;/s,
  'the action fills the row height at its right edge and hides until hover');
assert.match(styles, /\.workspace-list-row:hover \.workspace-list-row__anchor,[\s\S]*?\{\s*visibility:\s*hidden;\s*\}/,
  'hovering hands the right edge to the action without reflowing the meta line');
assert.match(styles, /\.workspace-list-row__action--latched\s*\{\s*visibility:\s*visible;\s*\}/,
  'a latched action holds the edge permanently, replacing the anchor');
assert.match(panels, /showAnchor = Boolean\(anchor && !action\?\.latched\)/,
  'a latched action replaces the anchor rather than crowding it');
assert.match(panels, /latched\?: boolean;/,
  'the row action declares whether it holds the slot or waits for hover');
assert.match(component, /latched: pinned,/,
  'a pinned model shows its pin in place of its engine');
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

/* ── Keyboard reach ──────────────────────────────────────────────── */

/* A listbox option owns the only tab stop in its row, so a row action that is a
   real control can never be tabbed to. Arrow keys carry focus in and back out
   instead, which is what keeps deleting a conversation, cancelling a download or
   ejecting a model available without a pointer. */
assert.match(panels, /case 'ArrowRight': \{[\s\S]*?querySelector<HTMLElement>\('button\.workspace-list-row__action'\)[\s\S]*?\.focus\(\);/,
  'ArrowRight moves focus from the row onto its action button');
assert.match(panels, /case 'ArrowLeft': \{[\s\S]*?options\[current\]\.focus\(\);/,
  'ArrowLeft returns focus from the action button to its row');
assert.match(panels, /aria-keyshortcuts=\{selectable \? 'ArrowRight' : undefined\}/,
  'a listbox row advertises the key that reaches its action');

/* Results arrive with nothing selected, so keying the tab stop off selection
   alone left a whole provider list unreachable by Tab. */
assert.match(manager, /const remoteRovingId = [\s\S]*?return selected \?\? results\[0\]\?\.id \?\? null;/,
  'the first remote result stands in as the tab stop until one is selected');
assert.match(manager, /tabIndex=\{rovingId === result\.id \? 0 : -1\}/,
  'the remote row takes its tab stop from the roving id, not from selection');

/* ── Sections must state a truth ─────────────────────────────────── */

/* A model still being fetched is not on this machine yet, and a heading that
   says otherwise invites treating a partial download as ready. */
assert.match(component, /entry\.status !== 'available' && entry\.status !== 'downloading'/,
  'a download in flight is not grouped under Downloaded');

/* ── Retired treatments must not come back ───────────────────────── */

assert.doesNotMatch(styles, /\.model-list-item|\.rail__item|--model-list-line/,
  'the per-tab row styles are replaced by the shared row');
assert.doesNotMatch(styles, /\.workspace-list-row__footer|\.workspace-list-row__guide/,
  'the gradient guide line and its terminus are retired');
assert.doesNotMatch(styles, /--list-backend-color/,
  'the backend label must not regain an identity color');

console.log('Shared list row layout, status, and readiness contract checks passed.');
