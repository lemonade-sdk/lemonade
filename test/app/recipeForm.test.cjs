const { assert, fixture, load, test, run, bridge } = require('./helpers/catalogHarness.cjs');
bridge();
const schema = load('recipes/backendSchema.ts');
const f = load('recipes/recipeForm.ts');
const legacy = load('recipes/recipeOptionsConfig.ts');
const literal = fixture('legacy');
const backends = ['cpu', 'server-gpu'];
function formFrom(name = 'controls') {
  const recipe = schema.parseBackendSchema(fixture(name)).recipes[0];
  const defaults = Object.fromEntries(recipe.options.map(option => [option.name, option.default_value]));
  const snapshot = { recipe: recipe.recipe, model_name: 'user.Test', saved: {}, effective: { ...defaults }, defaults };
  return f.createForm(recipe, snapshot, 'original', 0);
}
const option = (form, name) => form.recipe.options.find(item => item.name === name);
const payload = form => JSON.parse(JSON.stringify(f.mutationPayload(form, backends)));
for (const [recipe, keys] of Object.entries(literal.recipes)) test('legacy list/defaults/reset: ' + recipe, () => {
  assert(legacy.isLegacyRecipe(recipe));
  assert.deepEqual(legacy.getOptionsForRecipe(recipe), keys);
  const defaults = legacy.createDefaultOptions(recipe);
  const expected = Object.fromEntries(keys.map(key => [key, { value: literal.definitions[key].default, useDefault: true }]));
  assert.deepEqual(defaults, { recipe, ...expected });
  const reset = Object.fromEntries(keys.map(key => [literal.aliases[key] ?? key, literal.definitions[key].default]));
  assert.deepEqual(legacy.recipeOptionsToApi(defaults), reset);
  assert.deepEqual(legacy.apiToRecipeOptions(recipe, {}), defaults);
  assert.deepEqual(keys.map(key => legacy.getOptionDefinition(key).label), keys.map(key => literal.definitions[key].label));
});
for (const [name, definition] of Object.entries(literal.definitions)) test('legacy definition independent snapshot: ' + name, () => {
  const { description, ...actual } = legacy.getOptionDefinition(name);
  assert.deepEqual(actual, definition);
});
for (const [name, apiName] of Object.entries(literal.aliases)) test('legacy exact alias round trip: ' + name, () => {
  assert.equal(legacy.toApiOptionName(name), apiName);
  assert.equal(legacy.toFrontendOptionName(apiName), name);
});
test('untrusted identifiers do not select inherited legacy properties', () => {
  for (const name of ['constructor', '__proto__', 'toString']) {
    assert.equal(legacy.isLegacyRecipe(name), false);
    assert.deepEqual(legacy.getOptionsForRecipe(name), []);
    assert.equal(legacy.getOptionDefinition(name), undefined);
    assert.equal(legacy.toApiOptionName(name), name);
  }
});
test('built-in presentation remains independent of absent UI metadata and configured arms', () => {
  for (const recipe of ['thinksound', 'ryzenai-llm', 'llamacpp']) assert(legacy.isLegacyRecipe(recipe));
  assert.equal(legacy.isLegacyRecipe('ds4'), false);
  const row = { ...fixture('catalog-fourth').recipes[0], recipe: 'ds4', source: 'builtin', options: [] };
  const wire = fixture('catalog-fourth'); wire.recipes = [row];
  assert.equal(schema.parseBackendSchema(wire).recipes[0].source, 'builtin');
  assert.equal(legacy.createDefaultOptions('llamacpp').ctxSize.value, 4096);
  assert.equal(legacy.apiToRecipeOptions('llamacpp', { ctx_size: -1 }).ctxSize.value, -1);
});
test('untouched effective values produce an empty mutation', () => assert.deepEqual(payload(formFrom()), {}));
for (const [name, value] of [['boolean', false], ['integer', 0], ['integer', -1], ['text', ''], ['fraction', 0.25]]) {
  test('exact typed API value retained: ' + name + '=' + JSON.stringify(value), () => {
    const form = formFrom();
    assert.deepEqual(payload(f.editValue(form, option(form, name), value)), { [name]: value });
  });
}
for (const value of [null, -1, 'auto']) test('advertised sentinel becomes explicit inherit: ' + JSON.stringify(value), () => {
  const form = formFrom();
  const edited = f.editValue(form, option(form, 'sentinel'), value);
  assert.deepEqual(payload(edited), { sentinel: null });
  assert.equal(edited.edits.get('sentinel').mode, 'inherit');
});
for (const text of ['12abc', '', ' ', 'NaN', 'Infinity', '1e309', '0x10', '1.', '+2', '01', '2.5', '9007199254740993', '9', '-2']) {
  test('strict numeric draft rejects ' + JSON.stringify(text), () => {
    const form = formFrom();
    const edited = f.editNumeric(form, option(form, 'integer'), text);
    assert(edited.edits.get('integer').draftText !== undefined);
    assert(f.formErrors(edited, backends).has('integer'));
    assert.throws(() => payload(edited), /validation/);
  });
}
for (const text of ['4', '4.0', '4e0', ' 4 ']) test('complete numeric draft retains a typed integer: ' + text, () => {
  const form = formFrom('catalog-fourth');
  assert.deepEqual(payload(f.editNumeric(form, form.recipe.options[0], text)), { catalog_fourth_batch: 4 });
});
test('numeric sentinel outside range resolves before bounds', () => {
  const form = formFrom();
  assert.deepEqual(payload(f.editNumeric(form, option(form, 'sentinel'), '-1')), { sentinel: null });
});
test('inherit previews server defaults, omits untouched fields and excludes pinned/control keys', () => {
  let form = formFrom();
  form.snapshot.saved.integer = 4;
  form.snapshot.effective.integer = 4;
  form = f.setEdit(form, 'integer', { mode: 'inherit' });
  for (const key of ['pinned', 'save_options', 'dry_run', 'model_name']) form = f.setEdit(form, key, { mode: 'set', value: true });
  assert.equal(f.fieldValue(form, option(form, 'integer')), 0);
  assert.deepEqual(payload(form), { integer: null });
  assert.equal(form.snapshot.saved.integer, 4);
});
test('unsafe current/default/enum integers disable editing rather than rounding', () => {
  const form = formFrom(); form.snapshot.effective.integer = Number.MAX_SAFE_INTEGER + 1;
  assert.match(f.formErrors(form, backends).get('integer'), /round-trip/);
  const wire = fixture('controls'); wire.recipes[0].options[3].default_value = Number.MAX_SAFE_INTEGER + 1;
  assert.match(schema.parseBackendSchema(wire).recipes[0].incompatibility, /round-trip/);
});
test('server and schema defaults, effective and saved values are independent', () => {
  let form = formFrom('catalog-fourth');
  form.snapshot.defaults.catalog_fourth_batch = 3;
  form.snapshot.saved.catalog_fourth_batch = 4;
  form.snapshot.effective.catalog_fourth_batch = 4;
  assert.equal(f.fieldValue(form, form.recipe.options[0]), 4);
  form = f.setEdit(form, 'catalog_fourth_batch', { mode: 'inherit' });
  assert.equal(f.fieldValue(form, form.recipe.options[0]), 3);
  assert.equal(form.recipe.options[0].default_value, 2);
});
test('dirty reconciliation preserves valid values and surfaces removed/invalid edits', () => {
  let form = formFrom('catalog-fourth');
  form = f.editNumeric(form, form.recipe.options[0], '4');
  const changed = structuredClone(form.recipe); changed.options[0].maximum = 3;
  const updated = f.reconcileForm(form, changed, form.snapshot, 'changed');
  assert.equal(updated.review, true);
  assert.equal(updated.edits.get('catalog_fourth_batch').value, 4);
  assert.match(f.formErrors(updated, []).get('catalog_fourth_batch'), /range/);
  changed.options = [];
  const removed = f.reconcileForm(updated, changed, form.snapshot, 'removed');
  assert.match(f.formErrors(removed, []).get('catalog_fourth_batch'), /removed/);
  assert.throws(() => payload(removed), /validation/);
});
test('changed backend choices and types revalidate retained dirty edits', () => {
  let form = formFrom();
  form = f.editValue(form, option(form, 'constructor'), 'server-gpu');
  assert.equal(f.formErrors(form, backends).size, 0);
  assert.match(f.formErrors(form, ['cpu']).get('constructor'), /unavailable/);
  const changed = structuredClone(form.recipe); changed.options[0].value_type = 'integer';
  assert.match(f.formErrors(f.reconcileForm(form, changed, form.snapshot, 'new'), backends).get('constructor'), /integer/);
});
test('model snapshot verifies exact recipe/id and scalar shape', () => {
  const form = formFrom();
  assert.equal(f.parseModelOptions(form.snapshot, 'constructor', 'user.Test'), form.snapshot);
  assert.throws(() => f.parseModelOptions(form.snapshot, 'other', 'user.Test'), /recipe changed/);
  assert.throws(() => f.parseModelOptions({ ...form.snapshot, effective: { value: {} } }, 'constructor', 'user.Test'), /Malformed/);
});
run();
