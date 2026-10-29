const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const componentFile = path.join(root, 'src/components/ChatReadinessProgress.tsx');
const componentSource = fs.readFileSync(componentFile, 'utf8');
const parsed = ts.createSourceFile(componentFile, componentSource, ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
const errors = parsed.parseDiagnostics || [];
assert.equal(errors.length, 0, errors.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'));

const helpers = ['estimatedModelLoadPercent', 'relevantChatDownload'];
const functions = helpers.map(name => {
  const declaration = parsed.statements.find(node => ts.isFunctionDeclaration(node) && node.name?.text === name);
  assert.ok(declaration, `${name} must be present`);
  return declaration.getText(parsed);
}).join('\n');
const compiled = ts.transpileModule(`${functions}\nmodule.exports = { ${helpers.join(', ')} };`, {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 },
  reportDiagnostics: true,
});
assert.deepEqual((compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error), []);
const moduleExports = {};
const context = { module: { exports: moduleExports }, exports: moduleExports, Math, Number };
vm.runInNewContext(compiled.outputText, context);
const { estimatedModelLoadPercent: estimate, relevantChatDownload: select } = context.module.exports;

assert.equal(estimate(1, 0), 0);
assert.equal(estimate(1, 1000 + 1024 ** 3 / 1_000_000_000 * 1000), 99);
assert.equal(estimate(1, 1_000_000), 99, 'loading must never show 100%');
assert.equal(estimate(8, 1000), 10, 'larger models take proportionally longer');
assert.equal(estimate(undefined, 2000), null, 'unknown size is indeterminate');
assert.equal(estimate(0, 2000), null);
assert.equal(estimate(-1, 2000), null);
assert.equal(estimate(1, -1000), 0);

const model = { id: 'model:My-Model', modelName: 'My-Model', downloadType: 'model', status: 'downloading', percent: 62 };
const backend = { id: 'backend:llamacpp:rocm', modelName: 'llamacpp:rocm', downloadType: 'backend', status: 'downloading', percent: 38 };
const foreign = { id: 'model:Other', modelName: 'Other', downloadType: 'model', status: 'downloading', percent: 75 };
assert.equal(select([foreign], 'My-Model', 'llamacpp'), null, 'foreign model downloads are not relevant');
assert.equal(select([backend], 'My-Model', 'vllm'), null, 'foreign recipe installs are not relevant');
assert.equal(select([backend], 'My-Model', 'llamacpp').kind, 'backend', 'observed backend jobs must be shown');
assert.equal(select([backend, model], 'my-model', 'llamacpp').kind, 'model', 'a model download takes precedence');
assert.equal(select([{ ...model, percent: 100 }], 'My-Model', 'llamacpp').percent, 99);
assert.equal(select([{ ...model, status: 'completed', running: false }], 'My-Model', 'llamacpp'), null);
assert.equal(select([{ ...model, status: 'paused', percent: 52 }], 'My-Model', 'llamacpp').paused, true);
assert.equal(select([{ ...model, status: 'completed', running: true }], 'My-Model', 'llamacpp').percent, 62,
  'server finalization is still considered active');

const chatSource = fs.readFileSync(path.join(root, 'src/components/ChatView.tsx'), 'utf8');
assert.match(chatSource, /<ChatReadinessProgress[\s\S]*modelSizeGiB=/);
const indicatorAt = chatSource.indexOf('<ChatReadinessProgress');
const modelStackAt = chatSource.indexOf('<div className="composer__model-stack">');
const entryAt = chatSource.indexOf('<div className={`composer__entry${hasComposerSettings');
assert.ok(modelStackAt > 0 && modelStackAt < indicatorAt && indicatorAt < entryAt,
  'readiness must sit directly under the model pill, outside the image/3D settings');
assert.ok(indicatorAt >= 0 && entryAt > indicatorAt,
  'readiness must sit above the entire chat/image/3D composer entry');
assert.equal(chatSource.indexOf('<ChatReadinessProgress', indicatorAt + 1), -1,
  'readiness must render once, not inside capability-specific panels');
assert.doesNotMatch(chatSource, /data-model-preparation=/, 'readiness belongs above the composer, not as a full assistant message');
assert.doesNotMatch(chatSource, /prepareChatBackend|installBackend\(/, 'chat must never orchestrate installation');
assert.match(chatSource, /await loadModelForChat\(modelName, info \|\| initialInfo\)/, 'normal loading path remains intact');
assert.match(chatSource, /phase=\{modelPreparation\?\.phase \|\| \(modelPickerLoading \? 'loading' : null\)\}/, 'direct model selection gets the loading indicator');
const css = fs.readFileSync(path.join(root, 'src/styles/styles.css'), 'utf8');
assert.match(componentSource, /className="composer__readiness-anchor"/, 'indicator should use the model pill-aligned wrapper');
assert.doesNotMatch(componentSource, /className="composer__live-dot"/, 'readiness must not show a leading dot');
assert.match(componentSource, /import \{ Icon \} from '\.\/Icon';/, 'readiness reuses the existing GUI icon');
assert.match(componentSource, /<Icon name="clock" size=\{14\} className="composer__readiness-icon" \/>/, 'clock matches the 14px model pill icon');
assert.doesNotMatch(componentSource, /composer__readiness-percent/, 'no visible percentage on the readiness indicator');
assert.doesNotMatch(componentSource, /aria-valuenow=\{percent/, 'estimated loading progress should not announce numeric values');
assert.match(componentSource, /aria-valuenow=\{download \? percent \?\? undefined : undefined\}/,
  'only real download progress should expose a numeric accessibility value');
assert.match(componentSource, /aria-valuetext=\{download \? undefined : label\}/,
  'screen readers use the real download value; estimated model loading stays non-numeric');
assert.doesNotMatch(componentSource, /\$\{percent\}%`\}/, 'estimated percentages should not be announced');
assert.match(css, /\.composer__readiness-anchor \{ width: 100%; \}/);
assert.match(css, /\.composer__model-stack \{[\s\S]*?width: max-content;[\s\S]*?min-width: 220px;/);
assert.match(css, /\.composer__readiness \{[\s\S]*?width: 100%;[\s\S]*?border-radius: var\(--radius-pill\);/);
assert.match(css, /\.composer__readiness \.composer__live-meter span \{ border-radius: var\(--radius-pill\); \}/);
assert.match(css, /\.composer__readiness \{[\s\S]*?margin: 0;/);
assert.match(css, /\.composer__readiness \{[\s\S]*?padding: 5px var\(--space-3\);/, 'match model pill horizontal padding and intrinsic height');
assert.match(css, /\.composer__readiness \.composer__live-head \{[\s\S]*?font-size: var\(--text-sm\);[\s\S]*?font-weight: var\(--weight-medium\);/, 'match model pill font sizing and height');
assert.match(css, /\.composer__readiness-icon \{ flex-shrink: 0; color: var\(--text-tertiary\); \}/);
assert.doesNotMatch(css, /\.composer__readiness-percent/);
console.log('Chat readiness progress regression tests passed');

assert.doesNotMatch(componentSource, /prepareChatBackend/, 'indicator must only observe downloads');
console.log('No backend pre-install orchestration regression check passed');

const backendManagerFile = path.join(root, 'src/components/BackendManager.tsx');
const backendManagerSource = fs.readFileSync(backendManagerFile, 'utf8');
const backendAst = ts.createSourceFile(backendManagerFile, backendManagerSource, ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
const matcherSource = ['backendKey', 'backendDownloadId', 'backendDownloadName', 'backendDownloadMatches']
  .map(name => {
    const node = backendAst.statements.find(statement => ts.isFunctionDeclaration(statement) && statement.name?.text === name);
    assert.ok(node, `Missing ${name}`);
    return node.getText(backendAst);
  }).join('\n');
const matcherModule = { exports: {} };
const matcherCode = ts.transpileModule(`${matcherSource}\nmodule.exports = { backendDownloadMatches };`, {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 },
}).outputText;
vm.runInNewContext(matcherCode, { module: matcherModule });
const matches = matcherModule.exports.backendDownloadMatches;
const backendJob = (recipe, variant) => ({
  id: `backend:${recipe}:${variant}`, modelName: `${recipe}:${variant}`, downloadType: 'backend',
});
assert.equal(matches(backendJob('llamacpp', 'rocm-stable'), 'llamacpp', 'rocm'), true);
assert.equal(matches(backendJob('llamacpp', 'rocm-nightly'), 'llamacpp', 'rocm'), true);
assert.equal(matches(backendJob('llamacpp', 'cpu'), 'llamacpp', 'cpu'), true);
assert.equal(matches(backendJob('llamacpp', 'cuda'), 'llamacpp', 'rocm'), false);
assert.equal(matches(backendJob('other', 'rocm-stable'), 'llamacpp', 'rocm'), false);
console.log('Canonical ROCm backend download alias matching passed');
