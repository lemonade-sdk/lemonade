#!/usr/bin/env node
const { spawn } = require('node:child_process');
const net = require('node:net');
const { assert, fs, path, app, load, bridge } = require('./helpers/catalogHarness.cjs');

const repo = path.resolve(__dirname, '../..');
const args = Object.fromEntries(process.argv.slice(2).reduce((pairs, value, index, all) =>
  value.startsWith('--') ? [...pairs, [value.slice(2), all[index + 1]]] : pairs, []));
if (!args.lemond || !args.work) {
  console.error('usage: catalogLiveExercise.cjs --lemond PATH --work EMPTY_DIRECTORY [--output FILE]');
  process.exit(2);
}
const work = path.resolve(args.work);
const transcript = { checks: [] };
const check = (condition, message) => { assert(condition, message); transcript.checks.push(message); };
const freePort = () => new Promise((resolve, reject) => {
  const server = net.createServer();
  server.once('error', reject);
  server.listen(0, '127.0.0.1', () => { const { port } = server.address(); server.close(() => resolve(port)); });
});
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));

async function main() {
  check(fs.existsSync(work) && fs.readdirSync(work).length === 0, 'work directory exists and is empty');
  const config = path.join(work, 'config');
  const cache = path.join(work, 'cache');
  fs.mkdirSync(path.join(config, 'backends.d'), { recursive: true });
  fs.mkdirSync(cache);
  fs.copyFileSync(path.join(repo, 'test/fixtures/catalog-fourth.json'), path.join(config, 'backends.d/90-fourth.json'));
  const fixture = path.join(work, 'tiny.fixture');
  fs.writeFileSync(fixture, 'deterministic CPU fixture\n');
  fs.writeFileSync(path.join(config, 'user_models.json'), JSON.stringify({
    CatalogFourth: { recipe: 'catalog-fourth', checkpoint: fixture, source: 'local_path', labels: ['chat'] },
  }));
  const port = await freePort();
  const environment = Object.fromEntries(Object.entries(process.env)
    .filter(([key]) => !(key.startsWith('LEMONADE_') && key.endsWith('_API_KEY'))));
  const log = fs.openSync(path.join(work, 'lemond.log'), 'w');
  const server = spawn(path.resolve(args.lemond), [cache, config, '--port', String(port), '--host', '127.0.0.1'],
    { stdio: ['ignore', log, log], env: environment });
  const base = `http://127.0.0.1:${port}`;
  let spawnError;
  server.once('error', error => { spawnError = error; });
  try {
    for (let attempt = 0; ; attempt += 1) {
      if (spawnError) throw spawnError;
      if (server.exitCode !== null) throw new Error('lemond exited during startup; see ' + path.join(work, 'lemond.log'));
      try { if ((await fetch(base + '/api/v1/health')).ok) break; } catch {}
      if (attempt > 150) throw new Error('lemond did not become ready');
      await delay(100);
    }
    const { api, connection } = bridge();
    connection(base, '');
    const { refreshBackendCatalog } = load('features/backends/backendCatalog.ts');
    const { backendSelector } = load('features/backends/backendSchema.ts');
    const state = load('features/backends/catalogState.ts');
    const form = load('features/backends/recipeForm.ts');
    const { loadThroughCatalog } = load('features/backends/catalogLoad.ts');
    const { externalRecipeChoices, externalCheckpointRoles } = load('features/backends/catalogRecipeOptions.ts');
    const { recipeBackendOptionName } = load('features/backends/recipeMetadata.ts');
    const { backendLabel, backendCompactLabel } = load('modelPresentation.ts');
    const React = require(path.join(app, 'node_modules/react'));
    const { renderToStaticMarkup } = require(path.join(app, 'node_modules/react-dom/server'));
    const { SchemaRecipeOptions } = load('components/SchemaRecipeOptions.tsx');
    const { I18nProvider } = load('i18n.tsx');

    const harness = path.join(repo, 'test/fake_catalog_engine.py');
    const set = await fetch(base + '/internal/set', { method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ catalog_fourth: { cpu_bin: harness } }) });
    check(set.ok, 'server accepts the fake engine binary setting');

    const catalog = await refreshBackendCatalog();
    check(catalog.status === 'ready', 'catalog client reads the live schema');
    const row = state.catalogRecipe('catalog-fourth');
    check(row && row.source === 'external' && !row.incompatibility, 'external recipe is published and compatible');
    check(backendLabel('catalog-fourth') === 'Catalog Fourth' && backendCompactLabel('catalog-fourth') === 'Catalog Fourth',
      'labels use the server display name');
    check(backendSelector(row) === null && recipeBackendOptionName(await api.systemInfo(), 'catalog-fourth') === null,
      'fixed-backend recipe has no backend option and none is guessed');
    const choices = externalRecipeChoices();
    check(choices.length === 1 && choices[0].recipe === 'catalog-fourth' && choices[0].capabilities.includes('chat'),
      'add-model offers the external recipe under its declared mode');
    transcript.checkpoint_roles = externalCheckpointRoles('catalog-fourth');
    transcript.schema_exports_required_checkpoints = Array.isArray(row.required_checkpoints);

    const listed = (await api.models(true)).data.find(item => item.recipe === 'catalog-fourth');
    check(listed, 'model list reports the external model');
    const model = listed.id;
    transcript.model = model;
    const read = async () => form.parseModelOptions(await api.getModelOptions(model), 'catalog-fourth', model);
    let draft = form.createForm(row, await read(), catalog.catalog.catalog_id, catalog.connection.generation);
    const option = row.options.find(item => item.name === 'catalog_fourth_batch');
    const html = renderToStaticMarkup(React.createElement(I18nProvider, null,
      React.createElement(SchemaRecipeOptions, { form: draft, backends: [], onChange: () => {} })));
    check(['Batch size', 'type="number"', 'min="1"', 'max="8"', 'Schema default: 2'].every(text => html.includes(text)),
      'schema form renders the live option with its label, range and default');

    draft = form.editNumeric(draft, option, '9');
    check(form.formErrors(draft, []).has('catalog_fourth_batch'), 'out-of-range draft is refused by the client');
    const rejected = await api.saveModelOptions(model, { catalog_fourth_batch: 9 }, { dryRun: true }).then(() => false, () => true);
    check(rejected, 'server dry run refuses the same out-of-range value');

    draft = form.editNumeric(draft, option, '4');
    const payload = form.mutationPayload(draft, []);
    const dry = await api.saveModelOptions(model, { ...payload }, { dryRun: true });
    check(dry.effective.catalog_fourth_batch === 4 && Object.keys(dry.saved).length === 0, 'dry run resolves the edit and saves nothing');
    check(Object.keys((await read()).saved).length === 0, 'nothing persisted after the dry run');
    const saved = await api.saveModelOptions(model, { ...payload });
    check(saved.saved.catalog_fourth_batch === 4, 'Save persists exactly the edited key');

    const info = await api.modelDetail(model);
    check(info.recipe === 'catalog-fourth' && info.downloaded === true, 'server reports the external model as downloaded');
    check(await loadThroughCatalog(model, info, { catalog_fourth_batch: 3 }), 'catalog load flow handles the external model');
    const health = await api.health();
    check(health.all_models_loaded.some(item => item.model_name === model), 'model is loaded on the server');
    const observations = fs.readFileSync(fixture.replace(/\.fixture$/, '.observations.jsonl'), 'utf8')
      .trim().split('\n').map(line => JSON.parse(line));
    const argv = observations.find(event => event.event === 'argv').argv;
    check(argv[argv.indexOf('--batch') + 1] === '3', 'transient load option reached the engine argv');
    check((await read()).saved.catalog_fourth_batch === 4, 'load did not overwrite the saved option');
    transcript.engine_argv = argv.map(value => value === fixture ? '<fixture>' : value);
    const answer = await api.chatCompletionOnce(model, [{ role: 'user', content: 'ping' }]);
    check(answer.includes('fake batch=3'), 'chat reaches the external engine: ' + answer);
    await api.unloadModel(model);
    const reset = await api.resetModelOptions(model);
    check(Object.keys(reset.saved).length === 0, 'reset clears the saved option');
    transcript.catalog_id = catalog.catalog.catalog_id;
  } finally {
    if (!spawnError && server.exitCode === null) {
      server.kill('SIGTERM');
      await Promise.race([new Promise(resolve => server.once('exit', resolve)), delay(15000)]);
      if (server.exitCode === null) server.kill('SIGKILL');
    }
    fs.closeSync(log);
  }
  transcript.result = 'passed';
}

main().catch(error => {
  transcript.result = 'failed';
  transcript.error = String(error && error.stack || error);
  process.exitCode = 1;
}).finally(() => {
  const text = JSON.stringify(transcript, null, 2) + '\n';
  if (args.output) fs.writeFileSync(path.resolve(args.output), text);
  console.log(text);
});
