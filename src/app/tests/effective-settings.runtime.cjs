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

assert.match(effectiveSource, /healthResult\.value\.all_models_loaded\.find/,
  'effective settings must resolve the running model from health');
assert.match(effectiveSource, /formatCommand\(launchCommand\)/,
  'effective settings must render the server-reported launch command');
assert.match(effectiveSource, /Launch command unavailable because this model is not currently loaded\./,
  'effective settings must explain when no runtime command is available');

console.log('Effective Settings runtime contract checks passed.');
