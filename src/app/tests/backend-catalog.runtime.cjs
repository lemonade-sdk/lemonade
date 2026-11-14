const assert = require('node:assert/strict');
const fs = require('node:fs');
const { spawnSync } = require('node:child_process');
const path = require('node:path');

const read = name => fs.readFileSync(path.resolve(__dirname, '../src', name), 'utf8');
const detail = read('components/ModelDetailPanel.tsx');
const manager = read('components/ModelManager.tsx');
const client = read('api.ts');

assert.match(detail, /schemaRecipe\?\.source === 'external'/,
  'the Configuration tab must hand external recipes to the schema form');
assert.match(detail, /<SchemaModelOptions[\s\S]*?loadOptionsRef=\{loadOptionsRef\}/,
  'the schema form must publish the load options the panel Load action applies');
assert.match(detail, /catalog\.status === 'missing'[\s\S]*?t\(LEGACY_OPTIONS_NOTICE\)/,
  'a server without the schema endpoint must show the legacy-options notice');
assert.match(detail, /recipeOptionChoices\(activeRecipe, String\(key\)\)/,
  'schema enums must render as a select for built-in recipes too');
assert.match(detail, /const fromServer = serverRecipeDisplayName\(n\);/,
  'recipe labels must prefer the name the server reports');
assert.match(manager, /await loadThroughCatalog\(name, info, overrideOptions, custom\)/,
  'model loads must go through the catalog flow before the plain load');
assert.match(manager, /externalRecipeChoices\(\)/,
  'the add-model panel must list external recipes from the catalog');
assert.match(client, /body: options\.dryRun \? \{ \.\.\.changes, dry_run: true \} : changes/,
  'option saves must support dry_run');
assert.doesNotMatch(client, /options\[`\$\{recipe\}_backend`\]/,
  'the API client must not guess the backend option from the recipe name');
assert.match(detail, /builtinOptionErrors\(\s*schemaRecipe\?\.source === 'builtin' \? schemaRecipe : undefined/,
  'built-in recipes must validate their draft against the schema row');
assert.match(detail, /disabled=\{!serverOptionsLoaded \|\| schemaOptionErrors\.size > 0\}/,
  'Save must stay disabled while a built-in option fails schema validation');
assert.match(client, /if \(catalogRow\?\.source === 'builtin'\) \{[\s\S]*?await prepareBuiltinLoad\(modelName, cachedModelInfo, body\);/,
  'built-in loads must run the catalog preflight before posting the load');
assert.match(client, /private async _json<T = unknown>[\s\S]*?this\.assertCurrentConnection\(connection\);\s*return data;/,
  'JSON answers must be dropped when the connection changed while they were in flight');
assert.match(client, /if \(!this\.isCurrentConnection\(connection\)\) \{\s*await reader\.cancel\(\)/,
  'a chat stream must stop when the connection changes');
for (const stream of ['install', 'pull']) {
  assert.match(client, new RegExp(`_fetch\\('/api/v1/${stream}'[\\s\\S]*?_readForConnection\\(connection, reader\\)`),
    `the ${stream} stream must stop when the connection changes`);
}
assert.match(client, /new WebSocket\(wsUrl\);\s*const current = this\._bindSocketToConnection\(socket, connection\);/,
  'WebSocket streams must be bound to the connection that opened them');
assert.doesNotMatch(client, /resp\.blob\(\)[\s\S]{0,200}createObjectURL/,
  'generated files must go through the connection-bound download helper');
assert.match(manager, /<ExternalCheckpointFields\s+roles=\{selectedExternalRoles \?\? \[\]\}/,
  'the add-model panel must render the declared checkpoint roles');
assert.match(manager, /externalCheckpointRoles\(selectedCustomRecipeName\)/,
  'external recipes must take their checkpoint fields from the schema row');
const presentation = read('modelPresentation.ts');
assert.match(presentation, /return serverRecipeDisplayName\(recipe\) \|\| backendPresentation\(recipe\)\?\.compact/,
  'compact labels must prefer the server name over the curated table');
for (const view of ['components/ChatView.tsx', 'components/BackendManager.tsx', 'components/Monitor.tsx']) {
  assert.match(read(view), /useServerRecipeNames\(\);/, `${view} must re-render when server recipe names change`);
}
assert.match(read('components/BackendManager.tsx'), /new Proxy\(CURATED_RECIPE_LABELS, \{\s*get: \(curated, recipe\) => typeof recipe !== 'string' \? undefined\s*: serverRecipeDisplayName\(recipe\)/,
  'the Backends view must read recipe names from the server before its curated table');
assert.match(manager, /const backendCatalog = useBackendCatalog\(\);/,
  'the Models view, which owns the model list, must re-render when server recipe names change');

const suite = path.resolve(__dirname, '../../../test/app');
let failed = false;
for (const file of ['backendCatalog.test.cjs', 'connectionStreams.test.cjs', 'recipeForm.test.cjs', 'schemaRecipeOptions.test.cjs']) {
  const result = spawnSync(process.execPath, [path.join(suite, file)], { stdio: 'inherit' });
  if (result.error || result.status !== 0) failed = true;
}
if (failed) process.exitCode = 1;
else console.log('Lemonade App backend catalog regression checks passed.');
