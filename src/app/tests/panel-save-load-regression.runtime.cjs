const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const filename = path.resolve(__dirname, '../src/components/ModelDetailPanel.tsx');
const source = ts.createSourceFile(filename, fs.readFileSync(filename, 'utf8'),
  ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
const declarations = new Map();
let selectionEffect;
function visit(node) {
  if (ts.isCallExpression(node) && node.expression.getText(source) === 'useLayoutEffect'
    && node.arguments[0].getText(source).includes('saveSelectionRef.current')) {
    selectionEffect = node.arguments[0].getText(source);
  }
  if (ts.isVariableDeclaration(node) && node.initializer) {
    declarations.set(node.name.getText(source), node.initializer.getText(source));
  }
  if (ts.isFunctionDeclaration(node) && node.name) {
    declarations.set(node.name.text, node.getText(source));
  }
  ts.forEachChild(node, visit);
}
visit(source);
function compile(body, dependencies) {
  return Function(...dependencies, ts.transpileModule(body, {
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
  }).outputText);
}
const keys = ['llamacpp_backend', 'llamacpp_args'];
const args = '--temp 0.6 --top-k 20 --parallel 1 --threads 2';
const effectiveArgs = '--parallel 1 --temp 0.6 --threads 2 --top-k 20';
const saved = { ctx_size: 8192, llamacpp_backend: 'metal', llamacpp_args: args };
const response = { saved, effective: { ...saved, llamacpp_args: effectiveArgs }, defaults: {} };
const draft = { ctxSize: '8192', recipe: { llamacpp_backend: 'metal', llamacpp_args: args } };
const initial = { ctxSize: '8192', recipe: { llamacpp_backend: 'metal', llamacpp_args: '--parallel 1' } };

function loadPayload(state) {
  return compile(`return (${declarations.get('buildLoadOptions')})();`,
    ['buildConfigOptions', 'recipeKeys', 'argsKeys', 'argsKeyNames', 'recipeDraft', 'savedLoadSettings', 'supportsContextSize'])(
    () => ({ ...Object.fromEntries(Object.entries(state.draft.recipe).filter(([, value]) => value)),
      ctx_size: Number(state.draft.ctxSize) }),
    keys, ['llamacpp_args'], new Set(['llamacpp_args']), state.draft.recipe, state.baseline, true);
}

async function saveAndLoad(editDuringSave, { failure, showNotice = true, stale = false, deferDraftUpdate = false } = {}) {
  const state = { draft: structuredClone(draft), baseline: structuredClone(initial), notices: [] };
  const saveSelectionRef = { current: 0 };
  let selectedName = 'Test';
  let serverChange;
  const mountSelection = compile(`return (${selectionEffect})();`, ['saveSelectionRef', 'api']);
  const subscription = { onServerChange(callback) {
    serverChange = callback;
    return () => { serverChange = undefined; };
  } };
  let cleanup = mountSelection(saveSelectionRef, subscription);
  let queuedDraftUpdate;
  let finishSave;
  let rejectSave;
  const pending = new Promise((resolve, reject) => { finishSave = resolve; rejectSave = reject; });
  let submitted;
  const dependencies = {
    serverOptionsLoaded: true,
    saveSelectionRef,
    buildConfigOptions: () => ({ ...draft.recipe, ctx_size: 8192 }),
    serverSavedRecipeOptions: {},
    recipeKeys: keys,
    argsKeyNames: new Set(['llamacpp_args']),
    savedLoadSettings: initial,
    recipeDraft: draft.recipe,
    supportsContextSize: true,
    ctxSizeDraft: draft.ctxSize,
    serverDefaultRecipeOptions: {},
    name: 'Test',
    api: { saveModelOptions: (_name, patch) => { submitted = patch; return pending; } },
    applyServerOptions: result => {
      state.baseline = { ctxSize: String(result.effective.ctx_size), recipe: {
        llamacpp_backend: result.saved.llamacpp_backend,
        llamacpp_args: result.effective.llamacpp_args,
      } };
    },
    sanitizeRecipeOptions: value => value,
    offersResolvedValue: key => key === 'llamacpp_backend',
    setRecipeDraft: update => {
      if (deferDraftUpdate) queuedDraftUpdate = update;
      else state.draft.recipe = update(state.draft.recipe);
    },
    setCtxSizeDraft: update => { state.draft.ctxSize = update(state.draft.ctxSize); },
    setNotice: notice => { state.notices.push(notice); },
    t: text => text,
    friendlyErrorMessage: error => error.message,
  };
  const save = compile(`${declarations.get('fieldValue')}
${declarations.get('draftFromResolvedOptions')}
return ${declarations.get('saveConfig')};`, Object.keys(dependencies))(...Object.values(dependencies));
  const request = save(showNotice);
  assert.equal(submitted.llamacpp_args, args);
  const selectModel = name => {
    if (selectedName !== name) {
      cleanup();
      selectedName = name;
      cleanup = mountSelection(saveSelectionRef, subscription);
      state.draft = { ctxSize: '4096', recipe: { llamacpp_backend: 'vulkan', llamacpp_args: args } };
      state.baseline = structuredClone(state.draft);
    }
  };
  editDuringSave?.(state.draft, selectModel, serverChange);
  if (failure) rejectSave(new Error('save rejected'));
  else finishSave(response);
  state.saved = await request;
  if (!stale) assert.equal(state.saved, !failure);
  if (queuedDraftUpdate) {
    if (deferDraftUpdate === 'server') serverChange();
    else selectModel('Other');
    state.draft.recipe = queuedDraftUpdate(state.draft.recipe);
  }
  return state;
}

(async () => {
  const unchanged = await saveAndLoad();
  assert.deepEqual(unchanged.notices, ['Saved for future loads']);
  assert.deepEqual(loadPayload(unchanged), { llamacpp_backend: 'metal', ctx_size: 8192 },
    'after Save reorders effective arguments, an unchanged panel Load must use saved arguments without a privileged override');
  unchanged.draft.ctxSize = '4096';
  assert.deepEqual(loadPayload(unchanged), { llamacpp_backend: 'metal', ctx_size: 4096 },
    'context-only changes after Save must not resend arguments');

  const edited = await saveAndLoad((current, selectModel) => {
    selectModel('Test');
    current.recipe.llamacpp_args = '--threads 4';
    current.ctxSize = '4096';
  });
  assert.equal(edited.draft.recipe.llamacpp_args, '--threads 4');
  assert.equal(edited.draft.ctxSize, '4096');
  assert.deepEqual(loadPayload(edited), {
    llamacpp_backend: 'metal', llamacpp_args: '--threads 4', ctx_size: 4096, merge_args: false,
  }, 'edits typed while Save is pending must remain explicit privileged overrides');

  const cleared = await saveAndLoad(current => { current.recipe.llamacpp_args = ''; });
  assert.equal(loadPayload(cleared).llamacpp_args, null);
  assert.equal(loadPayload(cleared).merge_args, false,
    'clearing arguments while Save is pending must remain a protected change');
  const switched = await saveAndLoad((_current, selectModel) => selectModel('Other'), { stale: true });
  assert.equal(switched.baseline.recipe.llamacpp_backend, 'vulkan',
    'a Save response for the previous model must not replace the selected model server layers');
  assert.equal(switched.draft.recipe.llamacpp_args, args,
    'a previous-model Save must not normalize arguments in the selected model draft');
  assert.deepEqual(switched.notices, [], 'a previous-model Save must not show a success notice');
  assert.equal(switched.saved, false);

  const returned = await saveAndLoad((_current, selectModel) => {
    selectModel('Other');
    selectModel('Test');
  }, { stale: true });
  assert.equal(returned.baseline.recipe.llamacpp_backend, 'vulkan',
    'returning to the original model must not accept a Save from its earlier selection');
  assert.equal(returned.draft.recipe.llamacpp_args, args);
  assert.deepEqual(returned.notices, []);
  assert.equal(returned.saved, false);

  for (const failure of [false, true]) {
    const switchedServer = await saveAndLoad((_current, _selectModel, switchServer) => {
      switchServer();
      switchServer();
    }, { stale: true, failure });
    assert.deepEqual(switchedServer.baseline, initial);
    assert.deepEqual(switchedServer.draft, draft);
    assert.deepEqual(switchedServer.notices, []);
    assert.equal(switchedServer.saved, false);
  }
  const queuedServer = await saveAndLoad(undefined, { deferDraftUpdate: 'server' });
  assert.deepEqual(queuedServer.draft, draft, 'a deferred draft update must ignore a server switch');
  const failed = await saveAndLoad((_current, selectModel) => selectModel('Other'), {
    stale: true, failure: true,
  });
  assert.deepEqual(failed.notices, [], 'a previous-model Save failure must not replace the selected model notice');
  const currentFailure = await saveAndLoad(undefined, { failure: true });
  assert.deepEqual(currentFailure.notices, ['Could not save model options: save rejected']);

  const queued = await saveAndLoad(undefined, { deferDraftUpdate: true });
  assert.equal(queued.draft.recipe.llamacpp_args, args,
    'a queued draft updater must not normalize a model selected after the response');
  console.log('Panel Save to Load regression checks passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
