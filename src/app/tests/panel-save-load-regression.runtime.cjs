const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const filename = path.resolve(__dirname, '../src/components/ModelDetailPanel.tsx');
const source = ts.createSourceFile(filename, fs.readFileSync(filename, 'utf8'),
  ts.ScriptTarget.Latest, true, ts.ScriptKind.TSX);
const declarations = new Map();
function visit(node) {
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

async function saveAndLoad(editDuringSave) {
  const state = { draft: structuredClone(draft), baseline: structuredClone(initial) };
  let finishSave;
  const pending = new Promise(resolve => { finishSave = resolve; });
  let submitted;
  const dependencies = {
    serverOptionsLoaded: true,
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
    setRecipeDraft: update => { state.draft.recipe = update(state.draft.recipe); },
    setCtxSizeDraft: update => { state.draft.ctxSize = update(state.draft.ctxSize); },
    setNotice: () => {},
    t: text => text,
    friendlyErrorMessage: error => { throw error; },
  };
  const save = compile(`${declarations.get('fieldValue')}
${declarations.get('draftFromResolvedOptions')}
return ${declarations.get('saveConfig')};`, Object.keys(dependencies))(...Object.values(dependencies));
  const request = save();
  assert.equal(submitted.llamacpp_args, args);
  editDuringSave?.(state.draft);
  finishSave(response);
  assert.equal(await request, true);
  return state;
}

(async () => {
  const unchanged = await saveAndLoad();
  assert.deepEqual(loadPayload(unchanged), { llamacpp_backend: 'metal', ctx_size: 8192 },
    'after Save reorders effective arguments, an unchanged panel Load must use saved arguments without a privileged override');
  unchanged.draft.ctxSize = '4096';
  assert.deepEqual(loadPayload(unchanged), { llamacpp_backend: 'metal', ctx_size: 4096 },
    'context-only changes after Save must not resend arguments');

  const edited = await saveAndLoad(current => {
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
  console.log('Panel Save to Load regression checks passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
