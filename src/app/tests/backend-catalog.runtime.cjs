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

const suite = path.resolve(__dirname, '../../../test/app');
let failed = false;
for (const file of ['backendCatalog.test.cjs', 'recipeForm.test.cjs', 'schemaRecipeOptions.test.cjs']) {
  const result = spawnSync(process.execPath, [path.join(suite, file)], { stdio: 'inherit' });
  if (result.error || result.status !== 0) failed = true;
}
if (failed) process.exitCode = 1;
else console.log('Lemonade App backend catalog regression checks passed.');
