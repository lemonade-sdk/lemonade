const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const ts = require('typescript');

function transpile(sourcePath, outputPath) {
  const source = fs.readFileSync(sourcePath, 'utf8');
  const result = ts.transpileModule(source, {
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
    fileName: sourcePath,
    reportDiagnostics: true,
  });
  const errors = (result.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error);
  assert.equal(errors.length, 0, errors.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'));
  fs.writeFileSync(outputPath, result.outputText);
}

const root = path.resolve(__dirname, '..');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'gui3-router-runtime-'));
try {
  transpile(path.join(root, 'src/features/router/routerTypes.ts'), path.join(tmp, 'routerTypes.js'));
  transpile(path.join(root, 'src/features/router/routerRuntime.ts'), path.join(tmp, 'routerRuntime.js'));
  const runtime = require(path.join(tmp, 'routerRuntime.js'));

  const router = {
    id: 'user.router', model_name: 'user.router', recipe: 'collection.router', downloaded: true,
    components: ['fast-chat', 'smart-chat', 'route-classifier'],
    routing: {
      candidates: ['fast-chat', 'smart-chat'],
      default_model: 'smart-chat',
      classifiers: [{ id: 'intent', type: 'classifier', model: 'route-classifier' }],
      rules: [{ id: 'intent-route', match: { classifier: 'intent', label: 'match', min_score: 0.5 }, route_to: 'fast-chat' }],
    },
  };
  const models = [
    router,
    { id: 'fast-chat', model_name: 'fast-chat', downloaded: true, recipe: 'llamacpp' },
    { id: 'smart-chat', model_name: 'smart-chat', downloaded: true, recipe: 'llamacpp' },
    { id: 'route-classifier', model_name: 'route-classifier', downloaded: true, recipe: 'llamacpp' },
  ];
  const ok = runtime.preflightRouter(router, models, []);
  assert.equal(ok.ok, true, ok.errors.join(' '));
  assert.deepEqual(ok.dependencies.find(d => d.name === 'route-classifier').roles.sort(), ['classifier', 'component']);

  const missingClassifier = runtime.preflightRouter(router, models.filter(m => m.model_name !== 'route-classifier'), []);
  assert.equal(missingClassifier.ok, false);
  assert.match(runtime.routerPreflightError(missingClassifier), /route-classifier/);
  assert.match(runtime.routerPreflightError(missingClassifier), /not present/i);

  const notDownloaded = runtime.preflightRouter(router, models.map(m => m.model_name === 'route-classifier' ? { ...m, downloaded: false } : m), []);
  assert.equal(notDownloaded.ok, false);
  assert.match(runtime.routerPreflightError(notDownloaded), /not downloaded\/local/i);

  const badDefault = runtime.preflightRouter({ ...router, routing: { ...router.routing, default_model: 'other-chat' } }, models, []);
  assert.equal(badDefault.ok, false);
  assert.match(runtime.routerPreflightError(badDefault), /not in routing\.candidates/i);

  const chat = fs.readFileSync(path.join(root, 'src/components/ChatView.tsx'), 'utf8');
  assert.match(chat, /deferredUntilSend: isRouterModelInfo\(info\) \|\| Boolean\(configuredDefault\)/);
  const routerGuard = chat.indexOf('if (isRouterModelInfo(info)) {', chat.indexOf('const ensureChatModelReady'));
  const ordinaryLoad = chat.indexOf('await loadModelWithPolicy(modelName', routerGuard);
  assert.ok(routerGuard >= 0 && ordinaryLoad > routerGuard, 'Chat readiness must handle Router before ordinary /load');
  assert.match(chat.slice(routerGuard, ordinaryLoad), /preflightRouter/);

  assert.match(chat, /isRouterModelInfo\(selectedInfo\)/, 'Router selection must remain usable without all_models_loaded');
  assert.match(chat, /<ModelModeIcons capability=\{currentCapability\} recipe=\{currentRecipe\}/, 'chat must render model identity through the current ModelModeIcons path');
  const styles = fs.readFileSync(path.join(root, 'src/styles/styles.css'), 'utf8');
  assert.match(styles, /\.composer__model-mode--router\s*\{[^}]*color:\s*var\(--cap-router\)/, 'Router chat mode must use the shared Router task token');

  const apiSource = fs.readFileSync(path.join(root, 'src/api.ts'), 'utf8');
  assert.match(apiSource, /route_trace/);
  assert.match(apiSource, /Router selected/);
  const registrationStart = apiSource.indexOf('async registerModelDefinition');
  const registrationEnd = apiSource.indexOf('async pullModel', registrationStart);
  assert.ok(registrationStart >= 0 && registrationEnd > registrationStart, 'registerModelDefinition source must be inspectable');
  const registrationSource = apiSource.slice(registrationStart, registrationEnd);
  assert.match(registrationSource, /\/api\/v1\/models\/register/, 'definition registration must use /models/register');
  assert.doesNotMatch(registrationSource, /\/api\/v1\/pull/, 'definition registration must never fall back to /pull');

  const loadStart = apiSource.indexOf('async loadModel(');
  const loadEnd = apiSource.indexOf('async effectiveLoadCommand', loadStart);
  assert.ok(loadStart >= 0 && loadEnd > loadStart, 'loadModel source must be inspectable');
  const loadSource = apiSource.slice(loadStart, loadEnd);
  assert.doesNotMatch(loadSource, /registerModelDefinition/, 'Router load must never write a cached Router definition back to the server');
  assert.match(loadSource, /status: 'ready', mode: 'router', virtual: true/, 'Router load must remain a virtual readiness no-op');

  const editor = fs.readFileSync(path.join(root, 'src/components/RouterEditorPanel.tsx'), 'utf8');
  assert.match(editor, /const dependencyPreflight = preflightRouter\(nextRequest as any, models, \[\]\)/);
  assert.match(editor, /if \(!dependencyPreflight\.ok\)/, 'Router editor must block registration when a dependency preflight fails');
  const editorSaveStart = editor.indexOf('const save = async () => {');
  const serverAck = editor.indexOf('await onRegister(nextRequest, submittedDraft.name.trim());', editorSaveStart);
  assert.ok(editorSaveStart >= 0 && serverAck > editorSaveStart, 'Router editor must wait for server registration acknowledgement');
  assert.doesNotMatch(editor, /upsertRouterRecord\(/, 'server-owned Router definitions must not be written to browser storage');

  const routerStore = fs.readFileSync(path.join(root, 'src/features/router/routerStore.ts'), 'utf8');
  assert.doesNotMatch(routerStore, /localStorage\./, 'Router definitions are server-owned and must not persist in browser storage');
  assert.doesNotMatch(routerStore, /ROUTER_RECORDS_CHANGED_EVENT/, 'server-owned Router definitions do not need a browser-storage event bus');
  assert.match(routerStore, /export function loadRouterRecords\(\): RouterRecord\[\] \{\s*return \[\];\s*\}/, 'legacy Router record loader must remain a no-op compatibility shim');

  const manager = fs.readFileSync(path.join(root, 'src/components/ModelManager.tsx'), 'utf8');
  const readyStart = manager.indexOf('const ensureServerRouterReady = async');
  const runtimeStart = manager.indexOf('const loadModelRuntime = async');
  assert.ok(readyStart >= 0 && runtimeStart > readyStart, 'model manager must define a server-fresh Router readiness helper before runtime loading');
  const readySource = manager.slice(readyStart, runtimeStart);
  assert.match(readySource, /await api\.models\(true\)/, 'Router readiness must refresh the server registry');
  assert.match(readySource, /preflightRouter\(serverRouter, fresh\.data/, 'Router readiness must preflight the fresh server-owned Router definition');
  assert.doesNotMatch(readySource, /registerModelDefinition/, 'Router readiness must be read-only');

  const runtimeRouterGuard = manager.indexOf('if (isRouterModelInfo(info)) {', runtimeStart);
  const runtimeRouterEnd = manager.indexOf('const components =', runtimeRouterGuard);
  assert.ok(runtimeStart >= 0 && runtimeRouterGuard > runtimeStart && runtimeRouterEnd > runtimeRouterGuard, 'model manager must treat Router as virtual before collection/runtime loading');
  const runtimeRouterSource = manager.slice(runtimeRouterGuard, runtimeRouterEnd);
  assert.match(runtimeRouterSource, /ensureServerRouterReady\(name\)/, 'runtime Router path must use server-fresh readiness');
  assert.doesNotMatch(runtimeRouterSource, /registerModelDefinition/, 'runtime Router path must never overwrite the server definition');

  const handleLoadStart = manager.indexOf('const handleLoad = async');
  const handleUnloadStart = manager.indexOf('const handleUnload = async', handleLoadStart);
  const handleLoadSource = manager.slice(handleLoadStart, handleUnloadStart);
  assert.match(handleLoadSource, /ensureServerRouterReady\(name\)/, 'Use Router click must read/validate the server definition');
  assert.doesNotMatch(handleLoadSource, /registerModelDefinition/, 'Use Router click must never write a stale local Router definition');
  assert.match(handleLoadSource, /loadWithGlobalPolicy\(model, overrideOptions\)/, 'ordinary models must preserve the new-base load-options path');

  const saveStart = manager.indexOf('const handleRegisterRouter = async');
  const saveEnd = manager.indexOf('const handleRouterSaved', saveStart);
  const saveSource = manager.slice(saveStart, saveEnd);
  assert.match(saveSource, /await api\.registerModelDefinition\(request\.model_name/, 'explicit Router save remains the definition write path');
  assert.doesNotMatch(manager, /ROUTER_RECORDS_CHANGED_EVENT/, 'model manager must use server state instead of browser Router events');
  assert.match(
    manager,
    /registerModelDefinition\(canonicalCustomModelName\(component\), customRegistrationOptions\(component\)\)/,
    'Custom Router dependencies must register with their canonical user.* definition name',
  );
  assert.match(manager, /const preflight = preflightRouter\(routerInfo, fresh\.data/, 'Router save must preflight dependencies against fresh server registry data');

  assert.match(chat, /if \(isRouterRecipe\(currentRecipe\)\) return true;/, 'router chat must accept images so has_images rules are reachable');
  assert.match(chat, /const modeSupportsMcp = modeSupportsChatCompletions;/, 'router chat must retain MCP/Lemonade tool support');
  assert.match(chat, /streaming\.send\(convoId, requestModelName, chatMessages, toolRuntime, thinkingMode\)/, 'selected tools must reach router chat requests through the current streaming signature');

  const tools = fs.readFileSync(path.join(root, 'src/tools/lemonadeTools.ts'), 'utf8');
  const loadCase = tools.slice(tools.indexOf("case 'load_model'"), tools.indexOf("case 'unload_model'"));
  const special = loadCase.indexOf('if (isRouterModel(resolved))');
  const runtimeLoad = loadCase.indexOf('api.loadModel');
  assert.ok(special >= 0 && runtimeLoad > special, 'Tool must finish Router readiness branch before ordinary api.loadModel');
  assert.match(loadCase.slice(special, runtimeLoad), /status: 'ready'/);
  assert.match(loadCase.slice(special, runtimeLoad), /preflightRouter/);

  console.log('GUI3 Router load regression checks passed.');
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
