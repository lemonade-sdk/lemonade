const { assert, fixture, load, test, run, app, bridge, path, fs, renderer } = require('./helpers/catalogHarness.cjs');
bridge();
const React = require(path.join(app, 'node_modules/react'));
const { renderToStaticMarkup } = require(path.join(app, 'node_modules/react-dom/server'));
const { parseBackendSchema, controlFor } = load('recipes/backendSchema.ts');
const f = load('recipes/recipeForm.ts');
const { SchemaRecipeOptions } = load('components/SchemaRecipeOptions.tsx');
const make = (name = 'controls') => {
  const recipe = parseBackendSchema(fixture(name)).recipes[0];
  const values = Object.fromEntries(recipe.options.map(option => [option.name, option.default_value]));
  return f.createForm(recipe, { recipe: recipe.recipe, model_name: 'user.Test', saved: {}, effective: values, defaults: values }, 'one', 0);
};
const render = (form, props = {}) => renderToStaticMarkup(React.createElement(SchemaRecipeOptions, { form, backends: ['cpu'], onChange: () => {}, ...props }));
test('fourth option renders literal label, numeric value/default/bounds/inherit', () => {
  const html = render(make('catalog-fourth'));
  for (const text of ['Batch size', 'type="number"', 'value="2"', 'min="1"', 'max="8"', 'Schema default: 2', 'server default: 2', 'inherited', 'Use inherited value']) assert(html.includes(text), text);
});
test('all five native controls and no request-control fields', () => {
  const html = render(make());
  for (const text of ['type="number"', 'type="checkbox"', 'type="text"', '<textarea', '<select', 'Selected server CPU']) assert(html.includes(text), text);
  assert(!html.includes('Pin Model'));
  assert(!html.includes('save_options'));
});
test('enum index keeps boolean choice type and selected state', () => {
  const html = render(make());
  assert.match(html, /<option value="0" selected="">false<\/option>/);
  assert.match(html, /<option value="1">true<\/option>/);
});
test('exact option order and missing optional UI fallbacks', () => {
  const form = make(); const html = render(form);
  const labels = ['Server backend', 'Text', 'Long text', 'Integer', 'Enabled', 'Typed enum', 'fraction', 'sentinel'];
  for (let i = 1; i < labels.length; i++) assert(html.indexOf('>' + labels[i - 1] + '</label>') < html.indexOf('>' + labels[i] + '</label>'));
  assert.equal(controlFor(form.recipe.options[6]), 'number');
  const other = { ...form.recipe.options[4], ui: undefined };
  assert.equal(controlFor(other), 'checkbox');
  assert.equal(controlFor({ ...other, enum: [false, true] }), 'select');
});
test('labels/help are escaped as text with accessible label association', () => {
  const form = make('catalog-fourth');
  form.recipe.options[0].ui.label = '<script>alert(1)</script>';
  form.recipe.options[0].help = '<img onerror=alert(1)>';
  const html = render(form);
  assert(!html.includes('<script>'));
  assert(!html.includes('<img '));
  assert(html.includes('&lt;script&gt;'));
  assert.match(html, /for="[^"]+"/);
  assert(html.includes('aria-describedby='));
});
test('validation, disabled, removed edits, review and no-backend states remain visible', () => {
  let form = make();
  form = f.editNumeric(form, form.recipe.options[3], '9');
  form.edits.set('removed', { mode: 'set', value: 7 });
  form.review = true;
  const html = render(form, { backends: [], disabled: true, availability: 'Server setup required' });
  for (const text of ['disabled=""', 'aria-invalid="true"', 'role="alert"', 'review your edits', 'Discard removed edit', 'Server setup required', 'No usable backends']) assert(html.includes(text), text);
});
test('a genuinely empty compatible recipe is explicit', () => {
  const form = make('catalog-fourth'); form.recipe.options = [];
  assert(render(form).includes('no configurable execution options'));
});
for (const [control, type] of [['slider', 'integer'], ['checkbox', 'string'], ['number', 'boolean'], ['textarea', 'number'], ['select', 'string']]) {
  test('incompatible control/type is explicit: ' + control + '/' + type, () => {
    const wire = fixture('catalog-fourth'); const option = wire.recipes[0].options[0];
    option.ui.control = control; option.value_type = type;
    option.default_value = type === 'string' ? '' : type === 'boolean' ? false : 2;
    delete option.minimum; delete option.maximum;
    const recipe = parseBackendSchema(wire).recipes[0];
    assert.match(recipe.incompatibility, /Unsupported option/);
    const form = make('catalog-fourth'); form.recipe = recipe;
    assert(render(form).includes('Unsupported option'));
  });
}
for (const field of ['schema_version', 'control_version', 'definition_schema_version']) test('unsupported ' + field + ' disables the catalog contract', () => {
  const wire = fixture('catalog-fourth'); wire[field] = 2;
  assert.throws(() => parseBackendSchema(wire), /Incompatible/);
});
for (const corrupt of [null, {}, { recipes: [] }, { ...fixture('catalog-fourth'), recipes: [null] }, { ...fixture('catalog-fourth'), catalog_id: 4 }]) test('malformed schema is explicit: ' + JSON.stringify(corrupt).slice(0, 45), () => {
  assert.throws(() => parseBackendSchema(corrupt), /schema|version/);
});
test('duplicate recipe/option and malformed enum cannot hide controls', () => {
  const wire = fixture('catalog-fourth'); wire.recipes.push(wire.recipes[0]);
  assert.throws(() => parseBackendSchema(wire), /Malformed/);
  wire.recipes.pop(); wire.recipes[0].options.push(wire.recipes[0].options[0]);
  assert.throws(() => parseBackendSchema(wire), /Malformed/);
  wire.recipes[0].options.pop(); wire.recipes[0].options[0].enum = [{}];
  assert.throws(() => parseBackendSchema(wire), /Malformed/);
});
test('legacy context slider helpers retain numeric and auto semantics', () => {
  const filename = path.join(renderer, 'ModelOptionsModal.tsx');
  const ts = require(path.join(app, 'node_modules/typescript'));
  const Module = require('node:module');
  const compiled = new Module(filename, module);
  compiled.filename = filename; compiled.paths = Module._nodeModulePaths(path.dirname(filename));
  const source = fs.readFileSync(filename, 'utf8') + '\nexport {getContextSliderMarks, contextSizeToSliderValue, sliderValueToContextSize};';
  compiled._compile(ts.transpileModule(source, { fileName: filename, compilerOptions: { module: ts.ModuleKind.CommonJS, jsx: ts.JsxEmit.ReactJSX, esModuleInterop: true, target: ts.ScriptTarget.ES2020 } }).outputText, filename);
  const helpers = compiled.exports;
  assert.deepEqual(helpers.getContextSliderMarks(10000), [2048, 4096, 8192, 10000]);
  assert.deepEqual(helpers.getContextSliderMarks(1024), []);
  assert.equal(helpers.contextSizeToSliderValue(4096, 8192), 12);
  assert.equal(helpers.contextSizeToSliderValue(0, 8192), 13);
  assert.equal(helpers.sliderValueToContextSize(13, 8192), 8192);
});
run();
