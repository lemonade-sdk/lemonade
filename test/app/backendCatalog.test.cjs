const { assert, fixture, load, test, run, bridge, tick, app, path } = require('./helpers/catalogHarness.cjs');
const { api, connection: connect } = bridge();
const { BackendCatalogClient, backendCatalogClient, refreshBackendCatalog } = load('features/backends/backendCatalog.ts');
const { parseBackendSchema, backendSelector, own } = load('features/backends/backendSchema.ts');
const state = load('features/backends/catalogState.ts');
const { recipeBackendOptionName } = load('features/backends/recipeMetadata.ts');
const { catalogBackend, ensureCatalogModelReady, loadThroughCatalog } = load('features/backends/catalogLoad.ts');
const { catalogRecipeCapabilities, externalRecipeChoices } = load('features/backends/catalogRecipeOptions.ts');
let client;
const calls = [];
let reply = () => new Response(JSON.stringify(fixture('catalog-fourth')), { headers: { ETag: 'fourth-etag' } });
global.fetch = async (url, options = {}) => { calls.push({ url, options }); return reply(url, options); };
const reset = () => { client = new BackendCatalogClient(); calls.length = 0; };
const header = (call, name) => new Headers(call.options.headers).get(name);
const body = options => JSON.parse(options.body);

test('snapshots preserve proxy prefixes and stay stable between changes', () => {
  const snapshot = api.connectionSnapshot();
  assert.equal(snapshot.apiUrl, 'http://one.test/proxy/api/v1');
  assert.deepEqual(api.connectionSnapshot(), snapshot);
  api.assertCurrentConnection(snapshot);
});
test('snapshots expose opaque auth generations and never the key', () => {
  const before = api.connectionSnapshot();
  let notified = 0;
  const unsubscribe = api.onConnectionChange(() => { notified++; });
  connect('http://one.test/proxy/', 'private-second-key');
  const after = api.connectionSnapshot();
  unsubscribe();
  assert.equal(after.apiUrl, before.apiUrl);
  assert(after.authenticationGeneration > before.authenticationGeneration);
  assert(after.generation > before.generation);
  assert.equal(notified, 1);
  assert(!JSON.stringify(after).includes('private-'));
  assert(Object.isFrozen(after));
  assert.throws(() => api.assertCurrentConnection(before), /connection changed/);
});
test('an unchanged connection keeps its generation', () => {
  const before = api.connectionSnapshot();
  connect('http://one.test/proxy', 'private-second-key');
  assert.deepEqual(api.connectionSnapshot(), before);
});
test('catalog refresh deduplicates and then revalidates with ETag', async () => {
  reset();
  const pending = client.refresh(); assert.equal(pending, client.refresh());
  assert.equal((await pending).status, 'ready'); assert.equal(calls.length, 1);
  assert.equal(calls[0].url, 'http://one.test/proxy/api/v1/backends/schema');
  assert.equal(header(calls[0], 'Authorization'), 'Bearer private-second-key');
  reply = () => new Response(null, { status: 304 });
  const cached = await client.refresh();
  assert.equal(cached.status, 'ready');
  assert.equal(header(calls[1], 'If-None-Match'), 'fourth-etag');
  assert.equal(cached.catalog.catalog_id, 'sha256:fourth-fixture');
});
test('same catalog hash on a different URL cannot reuse metadata or ETag', async () => {
  connect('http://two.test/reverse/path', 'private-second-key');
  reply = () => new Response(JSON.stringify(fixture('catalog-fourth')));
  const result = await client.refresh();
  const request = calls.at(-1);
  assert.equal(result.status, 'ready');
  assert.equal(request.url, 'http://two.test/reverse/path/api/v1/backends/schema');
  assert.equal(header(request, 'If-None-Match'), null);
});
test('empty/nonempty auth transitions invalidate a same-URL cache', async () => {
  for (const key of ['', 'private-third-key', '']) {
    const previous = api.connectionSnapshot();
    connect('http://two.test/reverse/path', key);
    await client.refresh();
    assert(api.connectionSnapshot().authenticationGeneration > previous.authenticationGeneration);
    assert.equal(header(calls.at(-1), 'If-None-Match'), null);
  }
});
test('304 without a cached body refetches unconditionally', async () => {
  reset(); let request = 0;
  reply = () => ++request === 1 ? new Response(null, { status: 304 }) : new Response(JSON.stringify(fixture('catalog-fourth')));
  assert.equal((await client.refresh()).status, 'ready');
  assert.equal(calls.length, 2);
  assert(calls.every(call => header(call, 'If-None-Match') === null));
});
test('same URL restart with new catalog hash returns new metadata', async () => {
  const changed = fixture('catalog-fourth'); changed.catalog_id = 'restarted'; changed.recipes[0].options[0].maximum = 4;
  reply = () => new Response(JSON.stringify(changed), { headers: { ETag: 'new' } });
  const result = await client.refresh();
  assert.equal(result.catalog.catalog_id, 'restarted');
  assert.equal(result.catalog.recipes[0].options[0].maximum, 4);
});
for (const [status, expected] of [[401, 'authentication'], [403, 'authentication'], [404, 'missing'], [503, 'unreachable']]) test('HTTP failure has distinct state ' + status, async () => {
  reply = () => new Response('{}', { status });
  const result = await client.refresh();
  assert.equal(result.status, expected);
  assert(!('catalog' in result));
  reply = () => new Response(JSON.stringify(fixture('catalog-fourth')));
  assert.equal((await client.refresh()).status, 'ready');
  assert.equal(header(calls.at(-1), 'If-None-Match'), null);
});
test('network and incompatible contracts are explicit and clear usable data', async () => {
  reply = () => { throw new Error('private-key-not-an-error-message'); };
  const down = await client.refresh();
  assert.equal(down.status, 'unreachable'); assert(!down.message.includes('private-key'));
  const wire = fixture('catalog-fourth'); wire.control_version = 3;
  reply = () => new Response(JSON.stringify(wire));
  assert.equal((await client.refresh()).status, 'incompatible');
  reply = () => new Response('not-json');
  assert.equal((await client.refresh()).status, 'incompatible');
});
test('late schema responses from an obsolete generation are rejected', async () => {
  reset(); let release;
  reply = () => new Promise(resolve => { release = resolve; });
  const pending = client.refresh();
  await tick(); connect('http://three.test', 'new-private-key');
  release(new Response(JSON.stringify(fixture('catalog-fourth'))));
  await assert.rejects(pending, /connection changed/);
  reply = () => new Response(JSON.stringify(fixture('controls')));
  assert.equal((await client.refresh()).catalog.recipes[0].recipe, 'constructor');
  assert.equal(header(calls.at(-1), 'If-None-Match'), null);
});
test('bound mutation cannot retry or retarget after connection change', async () => {
  const snapshot = api.connectionSnapshot();
  connect('http://four.test', 'new-private-key');
  const before = calls.length;
  await assert.rejects(api.fetchForConnection(snapshot, '/load', { method: 'POST' }), /connection changed/);
  assert.equal(calls.length, before);
});
test('a connection change drops cached system info and late responses', async () => {
  let release;
  reply = () => new Promise(resolve => { release = resolve; });
  const pending = api.systemInfo();
  await tick(); connect('http://four-b.test', 'new-private-key');
  release(new Response(JSON.stringify({ recipes: { stale: {} } })));
  await pending;
  assert.equal(api.systemInfoData, null);
});
test('selector identity is independent of suffix/UI and supports compatible older hints', () => {
  const recipe = parseBackendSchema(fixture('controls')).recipes[0];
  assert.equal(backendSelector(recipe), 'constructor');
  delete recipe.backend_option; assert.equal(backendSelector(recipe), 'constructor');
  delete recipe.options[0].ui; assert.equal(backendSelector(recipe), null);
  assert.equal(own({}, 'constructor'), undefined);
});
test('backend option resolves from the schema, never from a recipe-name guess once a schema exists', () => {
  const info = { recipes: {
    constructor: { selectable_backend: true, options: [] },
    builtin: { selectable_backend: true, options: [{ name: 'engine', type_name: 'BACKEND' }] },
    plain: { selectable_backend: true, options: [] },
  } };
  state.setCatalogState({ status: 'missing', connection: api.connectionSnapshot(), message: 'none' });
  assert.equal(recipeBackendOptionName(info, 'plain'), 'plain_backend');
  assert.equal(recipeBackendOptionName(info, 'builtin'), 'engine');
  assert.equal(recipeBackendOptionName({ recipes: {} }, 'constructor'), null);
  const catalog = parseBackendSchema(fixture('controls'));
  catalog.recipes.push({ recipe: 'plain', source: 'builtin', display_name: 'Plain', options: [], support: [] });
  catalog.recipes.push({ recipe: 'none', source: 'external', display_name: 'None', backend_option: null, options: [], support: [] });
  state.setCatalogState({ status: 'ready', connection: api.connectionSnapshot(), catalog });
  assert.equal(recipeBackendOptionName(info, 'constructor'), 'constructor');
  assert.equal(recipeBackendOptionName(info, 'plain'), null);
  assert.equal(recipeBackendOptionName(info, 'builtin'), 'engine');
  assert.equal(recipeBackendOptionName({ recipes: { none: { selectable_backend: true } } }, 'none'), null);
});
test('server display names replace tables, vanish with the catalog and survive prototype identifiers', () => {
  const { backendLabel, backendCompactLabel } = load('modelPresentation.ts');
  assert.equal(backendLabel('constructor'), 'Unlisted controls');
  assert.equal(backendCompactLabel('constructor'), 'Unlisted controls');
  state.setSystemRecipeNames({ recipes: { llamacpp: { display_name: 'Llama.cpp from server' }, toString: {} } });
  assert.equal(backendLabel('llamacpp'), 'Llama.cpp from server');
  assert.equal(backendCompactLabel('llamacpp'), 'llama.cpp');
  assert.equal(backendLabel('toString'), 'toString');
  state.clearServerRecipeNames();
  state.setCatalogState({ status: 'loading', connection: api.connectionSnapshot() });
  assert.equal(backendLabel('constructor'), 'constructor');
  assert.equal(backendLabel('llamacpp'), 'llama.cpp');
  assert.equal(state.serverRecipeDisplayName('__proto__'), undefined);
});
test('external recipes join the add-model choices with their declared modes', () => {
  const catalog = parseBackendSchema(fixture('controls'));
  catalog.recipes[0].supported_modes = ['transcription', 'chat'];
  catalog.recipes.push({ recipe: 'shipped', source: 'builtin', display_name: 'Shipped', supported_modes: ['image'], options: [], support: [] });
  state.setCatalogState({ status: 'ready', connection: api.connectionSnapshot(), catalog });
  assert.deepEqual(externalRecipeChoices().map(choice => [choice.recipe, choice.label, choice.capabilities]),
    [['constructor', 'Unlisted controls', ['audio', 'chat', 'omni']]]);
  assert.deepEqual(catalogRecipeCapabilities('shipped'), ['image']);
  assert.equal(catalogRecipeCapabilities('toString'), undefined);
});
test('published catalog state follows refreshes and the legacy notice state', async () => {
  backendCatalogClient.clear();
  reply = () => new Response('{}', { status: 404 });
  assert.equal((await refreshBackendCatalog()).status, 'missing');
  assert.equal(state.catalogState().status, 'missing');
  assert.match(state.LEGACY_OPTIONS_NOTICE, /no backend schema endpoint/);
  reply = () => new Response(JSON.stringify(fixture('catalog-fourth')), { headers: { ETag: 'e' } });
  const version = state.catalogVersion();
  await refreshBackendCatalog();
  assert.equal(state.catalogRecipe('catalog-fourth').display_name, fixture('catalog-fourth').recipes[0].display_name);
  assert(state.catalogVersion() > version);
  reply = () => new Response(null, { status: 304 });
  const settled = state.catalogVersion();
  await refreshBackendCatalog();
  assert.equal(state.catalogVersion(), settled);
});
test('dry-run saves send dry_run, skip the models-changed notification and are connection-bound', async () => {
  let changed = 0;
  const unsubscribe = api.onModelsChanged(() => { changed++; });
  const answer = { recipe: 'catalog-fourth', model_name: 'm', saved: {}, defaults: {}, effective: { catalog_fourth_batch: 4 } };
  reply = () => new Response(JSON.stringify(answer));
  await api.saveModelOptions('m', { catalog_fourth_batch: 4 }, { dryRun: true });
  assert.deepEqual(body(calls.at(-1).options), { catalog_fourth_batch: 4, dry_run: true });
  assert.equal(changed, 0);
  await api.validateModelOptions('m', {});
  assert.deepEqual(body(calls.at(-1).options), { dry_run: true });
  await api.saveModelOptions('m', { catalog_fourth_batch: 4 });
  assert.deepEqual(body(calls.at(-1).options), { catalog_fourth_batch: 4 });
  assert.equal(changed, 1);
  reply = () => { connect('http://four-c.test', 'new-private-key'); return new Response(JSON.stringify(answer)); };
  await assert.rejects(api.saveModelOptions('m', {}, { dryRun: true }), /connection changed/);
  unsubscribe();
});
test('selector reset uses fresh server defaults instead of a stale saved backend', () => {
  const recipe = parseBackendSchema(fixture('controls')).recipes[0];
  const context = { recipe, effective: { constructor: '' }, connection: api.connectionSnapshot() };
  assert.equal(catalogBackend(context, { constructor: { default_backend: 'cpu' } }), 'cpu');
  context.effective.constructor = 'server-gpu'; assert.equal(catalogBackend(context, {}), 'server-gpu');
  delete recipe.backend_option; delete recipe.options[0].ui;
  assert.equal(catalogBackend(context, {}), null);
});
test('catalog load validates before any side effect and sends exact typed API names', async () => {
  const recipe = parseBackendSchema(fixture('catalog-fourth')).recipes[0];
  const context = { recipe, effective: { catalog_fourth_batch: 4 }, connection: api.connectionSnapshot() };
  const model = { id: 'm', recipe: 'catalog-fourth', downloaded: true };
  const posts = []; calls.length = 0;
  reply = (url, options) => {
    if (options.method === 'POST') { posts.push([url, body(options)]); return new Response('{}'); }
    return new Response(JSON.stringify(url.endsWith('/system-info') ? { recipes: {} } : { recipe: 'catalog-fourth', downloaded: true }));
  };
  await assert.rejects(ensureCatalogModelReady('m', model, { catalog_fourth_batch: 9 }, context), /Invalid option/);
  await assert.rejects(ensureCatalogModelReady('m', model, { unknown: 1 }, context), /Invalid option/);
  assert.equal(calls.length, 0);
  await ensureCatalogModelReady('m', model, { catalog_fourth_batch: 4, pinned: true }, context);
  assert.deepEqual(posts, [[context.connection.apiUrl + '/load', { catalog_fourth_batch: 4, pinned: true, model_name: 'm', save_options: false }]]);
});
test('a switch during preflight prevents every subsequent install/pull/load mutation', async () => {
  const recipe = parseBackendSchema(fixture('catalog-fourth')).recipes[0];
  const connection = api.connectionSnapshot(); let posts = 0;
  reply = (url, options) => {
    if (options.method === 'POST') posts++;
    connect('http://six.test', 'private-six-key');
    return new Response(JSON.stringify({ recipes: {} }));
  };
  await assert.rejects(ensureCatalogModelReady('m', { id: 'm', recipe: 'catalog-fourth' }, { catalog_fourth_batch: 4 }, { recipe, connection, effective: {} }), /connection changed/);
  assert.equal(posts, 0);
});
test('arbitrary selector installs the selected server backend and stops for setup actions', async () => {
  const recipe = parseBackendSchema(fixture('controls')).recipes[0];
  const connection = api.connectionSnapshot();
  const posts = [];
  reply = (url, options) => {
    if (options.method === 'POST') {
      posts.push([url, body(options)]);
      return new Response(JSON.stringify({ action: 'https://example.test/setup' }));
    }
    return new Response(JSON.stringify({ recipes: { constructor: { default_backend: 'cpu', backends: {
      'server-gpu': { state: 'installable', message: 'Server setup', action: '' },
    } } } }));
  };
  await assert.rejects(ensureCatalogModelReady('m', { id: 'm', recipe: 'constructor', downloaded: true }, { constructor: 'server-gpu' },
    { connection, recipe, effective: { constructor: 'server-gpu' } }), /Complete the server backend setup/);
  assert.deepEqual(posts, [[connection.apiUrl + '/install', { recipe: 'constructor', backend: 'server-gpu', stream: true, subscribe: false }]]);
});
test('an unsupported selected backend stops the load with the server message', async () => {
  const recipe = parseBackendSchema(fixture('controls')).recipes[0];
  const connection = api.connectionSnapshot();
  let posts = 0;
  reply = (url, options) => {
    if (options.method === 'POST') posts++;
    return new Response(JSON.stringify({ recipes: { constructor: { default_backend: 'cpu', backends: {
      cpu: { state: 'unsupported', message: 'Needs another machine' },
    } } } }));
  };
  await assert.rejects(ensureCatalogModelReady('m', { id: 'm', recipe: 'constructor' }, {},
    { connection, recipe, effective: {} }), /Needs another machine/);
  assert.equal(posts, 0);
});
test('switch after install dispatch prevents any subsequent load or pull', async () => {
  const recipe = parseBackendSchema(fixture('controls')).recipes[0];
  const connection = api.connectionSnapshot();
  const posts = [];
  reply = (url, options) => {
    if (options.method === 'POST') {
      posts.push(url);
      connect('http://seven.test', 'private-seven-key');
      return new Response(JSON.stringify({ action: 'https://example.test/setup' }));
    }
    return new Response(JSON.stringify({ recipes: { constructor: { default_backend: 'cpu', backends: {
      cpu: { state: 'installable', message: '', action: '' },
    } } } }));
  };
  await assert.rejects(ensureCatalogModelReady('m', { id: 'm', recipe: 'constructor' }, { constructor: 'cpu' },
    { connection, recipe, effective: { constructor: 'cpu' } }), /connection changed/);
  assert.deepEqual(posts, [connection.apiUrl + '/install']);
});
test('a missing model is pulled before the load and both stay on the bound server', async () => {
  const recipe = parseBackendSchema(fixture('catalog-fourth')).recipes[0];
  const connection = api.connectionSnapshot();
  const posts = [];
  reply = (url, options) => {
    if (options.method === 'POST') { posts.push(url); return new Response(JSON.stringify({ status: 'completed', complete: true })); }
    if (url.endsWith('/downloads')) return new Response('[]');
    return new Response(JSON.stringify(url.endsWith('/system-info') ? { recipes: {} } : { recipe: 'catalog-fourth', downloaded: false }));
  };
  await ensureCatalogModelReady('m', { id: 'm', recipe: 'catalog-fourth' }, {}, { connection, recipe, effective: {} });
  assert.deepEqual(posts, [connection.apiUrl + '/pull', connection.apiUrl + '/load']);
});
test('ordinary external loads validate with a dry run and send transient options', async () => {
  backendCatalogClient.clear();
  const posts = [];
  const connection = api.connectionSnapshot();
  reply = (url, options) => {
    if (url.endsWith('/backends/schema')) return new Response(JSON.stringify(fixture('catalog-fourth')));
    if (url.endsWith('/options')) {
      posts.push([url, body(options)]);
      return new Response(JSON.stringify({ recipe: 'catalog-fourth', model_name: 'm',
        saved: {}, defaults: { catalog_fourth_batch: 2 }, effective: { catalog_fourth_batch: 2 } }));
    }
    if (url.endsWith('/system-info')) return new Response('{"recipes":{}}');
    if (options.method === 'POST') { posts.push([url, body(options)]); return new Response('{}'); }
    return new Response('{"recipe":"catalog-fourth","downloaded":true}');
  };
  const model = { id: 'm', recipe: 'catalog-fourth', downloaded: true };
  assert.equal(await loadThroughCatalog('m', model), true);
  assert.deepEqual(posts, [
    [connection.apiUrl + '/models/m/options', { dry_run: true }],
    [connection.apiUrl + '/load', { model_name: 'm', save_options: false }],
  ]);
  posts.length = 0;
  assert.equal(await loadThroughCatalog('m', model, { catalog_fourth_batch: 4, pinned: true }, { ctx_size: 4096, save_options: false }), true);
  assert.deepEqual(posts, [
    [connection.apiUrl + '/models/m/options', { catalog_fourth_batch: 4, dry_run: true }],
    [connection.apiUrl + '/load', { catalog_fourth_batch: 4, pinned: true, model_name: 'm', save_options: false }],
  ]);
  posts.length = 0;
  await api.loadModel('m', { catalog_fourth_batch: 3 }, model);
  assert.deepEqual(posts.map(([url]) => url), [connection.apiUrl + '/models/m/options', connection.apiUrl + '/load']);
});
test('a rejected dry run stops an external load before any mutation', async () => {
  const posts = [];
  reply = (url, options) => {
    if (url.endsWith('/backends/schema')) return new Response(null, { status: 304 });
    if (options.method === 'POST') { posts.push(url); return new Response('{}', { status: 400 }); }
    return new Response('{}');
  };
  await assert.rejects(loadThroughCatalog('m', { id: 'm', recipe: 'catalog-fourth' }, { catalog_fourth_batch: 99 }), /rejected model options/);
  assert.equal(posts.length, 1);
});
test('built-in and unknown recipes stay on the regular load path', async () => {
  const wire = fixture('catalog-fourth'); wire.recipes[0].source = 'builtin'; wire.catalog_id = 'builtin-only';
  backendCatalogClient.clear();
  reply = () => new Response(JSON.stringify(wire));
  assert.equal(await loadThroughCatalog('m', { id: 'm', recipe: 'catalog-fourth' }), false);
  assert.equal(await loadThroughCatalog('m', { id: 'm', recipe: 'toString' }), false);
  assert.equal(await loadThroughCatalog('m', null), false);
});

run();
