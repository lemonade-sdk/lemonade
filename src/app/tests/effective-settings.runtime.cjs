const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const apiSource = fs.readFileSync(path.join(root, 'src/api.ts'), 'utf8');
const chatSource = fs.readFileSync(path.join(root, 'src/components/ChatView.tsx'), 'utf8');
const effectiveSource = fs.readFileSync(path.join(root, 'src/components/EffectiveSettingsModal.tsx'), 'utf8');

assert.match(apiSource, /launch_command: launchCommand\.length > 0 \? launchCommand : undefined/,
  'health normalization must retain the server-reported launch command');
assert.doesNotMatch(apiSource, /effectiveLoadCommand/,
  'the retired command-preview API must not be restored');

assert.match(chatSource, /aria-label="Effective settings"/,
  'the chat toolbar must expose Effective Settings');
assert.match(chatSource, /<EffectiveSettingsModal/,
  'the chat view must render the Effective Settings modal');
assert.match(chatSource, /!isCollectionModel\(currentEffectiveSettingsModelInfo\)/,
  'virtual collection models must not expose Effective Settings without a concrete runtime');
assert.doesNotMatch(chatSource, /isModelLoaded=\{!!currentLoadedModel\}/,
  'the modal action state must not be fixed to a parent health snapshot');

assert.match(effectiveSource, /health\.all_models_loaded\.find/,
  'effective settings must resolve the running model from health');
assert.match(effectiveSource, /formatCommand\(launchCommand\)/,
  'effective settings must render the server-reported launch command');
assert.match(effectiveSource, /Launch command unavailable because this model is not currently loaded\./,
  'effective settings must explain when no runtime command is available');
assert.match(effectiveSource, /const sessionArgs = getSessionArgsOverride\(modelName\)\?\.args;[\s\S]*?sessionArgs \?\? \(argsField \? options\.effective\?\.\[argsField\]/,
  'session overrides must initialize the draft before server effective values');
assert.match(effectiveSource, /const runtimeStatePending = runtimeLoading \|\| runtimeStateModelName !== modelName;[\s\S]*?const isRuntimeModelLoaded = !runtimeStatePending && !!runtimeModel;/,
  'load and reload actions must use the fresh runtime snapshot');
assert.match(effectiveSource, /catch \(healthError\)[\s\S]*?const fallbackModel = loadedModelRef\.current;[\s\S]*?setRuntimeModel\(fallbackModel\);/,
  'parent runtime props may only be used when the health request fails');
assert.match(effectiveSource, /const configurationRequestRef = useRef\(0\);/);
assert.match(effectiveSource, /const runtimeRequestRef = useRef\(0\);/);
assert.match(effectiveSource, /const draftRevisionRef = useRef\(0\);/);
assert.match(effectiveSource, /draftRevision === draftRevisionRef\.current/,
  'stale configuration requests must not overwrite an edited draft');
assert.match(effectiveSource, /\}, \[open, modelName, loadConfiguration\]\);/,
  'configuration initialization must not rerun for parent runtime object refreshes');
assert.match(effectiveSource, /\}, \[open, modelName, refreshRuntime\]\);/,
  'runtime refreshes must be scoped to opening or changing models');
assert.match(effectiveSource, /if \(healthFailedRef\.current\)[\s\S]*?setRuntimeModel\(loadedModel \|\| null\);/,
  'parent runtime snapshots must remain fallback-only after health failures');
assert.match(effectiveSource, /value=\{draft\}[\s\S]*?disabled=\{busy\}/,
  'the draft must not remain editable while a submitted value is being refreshed');
assert.match(effectiveSource, /onClick=\{resetOverride\} disabled=\{busy \|\| runtimeStatePending\}/,
  'reset must wait for fresh runtime state before choosing reload behavior');

console.log('Effective Settings runtime contract checks passed.');
