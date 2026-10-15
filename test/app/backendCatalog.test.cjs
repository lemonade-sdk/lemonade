const vm = require('node:vm');
const { assert, fixture, load, stub, test, run, bridge, tick, fs, path, renderer } = require('./helpers/catalogHarness.cjs');
const callbacks = bridge();
const { serverConfig, ServerConfig } = load('utils/serverConfig.ts');
const { BackendCatalogClient } = load('utils/backendCatalog.ts');
const { parseBackendSchema, backendSelector, own } = load('recipes/backendSchema.ts');
let client;
const calls = [];
let reply = () => new Response(JSON.stringify(fixture('catalog-fourth')), { headers: { ETag: 'fourth-etag' } });
global.fetch = async (url, options) => { calls.push({ url, options }); return reply(url, options); };
const reset = () => { client = new BackendCatalogClient(); calls.length = 0; };
test('explicit URL initializes optional connection listener without port callback', async () => {
  await serverConfig.waitForInit();
  assert.equal(typeof callbacks.connection, 'function');
  assert.equal(serverConfig.getSnapshot().apiUrl, 'http://one.test/proxy/api/v1');
  assert.equal(serverConfig.getSnapshot().authenticationGeneration, 0);
});
test('snapshots expose opaque auth generations and preserve proxy prefixes', () => {
  const before = serverConfig.getSnapshot();
  callbacks.connection('http://one.test/proxy/', 'private-second-key');
  const after = serverConfig.getSnapshot();
  assert.equal(after.apiUrl, before.apiUrl);
  assert(after.authenticationGeneration > before.authenticationGeneration);
  assert(!JSON.stringify(after).includes('private-'));
  assert(Object.isFrozen(after));
});
test('catalog refresh deduplicates and then revalidates with ETag', async () => {
  reset();
  const pending = client.refresh(); assert.equal(pending, client.refresh());
  assert.equal((await pending).status, 'ready'); assert.equal(calls.length, 1);
  reply = () => new Response(null, { status: 304 });
  const cached = await client.refresh();
  assert.equal(cached.status, 'ready');
  assert.equal(calls[1].options.headers.get('If-None-Match'), 'fourth-etag');
  assert.equal(cached.catalog.catalog_id, 'sha256:fourth-fixture');
});
test('same catalog hash on a different URL cannot reuse metadata or ETag', async () => {
  callbacks.connection('http://two.test/reverse/path', 'private-second-key');
  reply = () => new Response(JSON.stringify(fixture('catalog-fourth')));
  const state = await client.refresh();
  const request = calls.at(-1);
  assert.equal(state.status, 'ready');
  assert.equal(request.url, 'http://two.test/reverse/path/api/v1/backends/schema');
  assert.equal(request.options.headers.get('If-None-Match'), null);
});
test('empty/nonempty auth transitions invalidate a same-URL cache', async () => {
  for (const key of ['', 'private-third-key', '']) {
    const previous = serverConfig.getSnapshot();
    callbacks.connection('http://two.test/reverse/path', key);
    await client.refresh();
    assert(serverConfig.getSnapshot().authenticationGeneration > previous.authenticationGeneration);
    assert.equal(calls.at(-1).options.headers.get('If-None-Match'), null);
  }
});
test('304 without a cached body refetches unconditionally', async () => {
  reset(); let request = 0;
  reply = () => ++request === 1 ? new Response(null, { status: 304 }) : new Response(JSON.stringify(fixture('catalog-fourth')));
  assert.equal((await client.refresh()).status, 'ready');
  assert.equal(calls.length, 2);
  assert(calls.every(call => call.options.headers.get('If-None-Match') === null));
});
test('same URL restart with new catalog hash returns new metadata', async () => {
  const changed = fixture('catalog-fourth'); changed.catalog_id = 'restarted'; changed.recipes[0].options[0].maximum = 4;
  reply = () => new Response(JSON.stringify(changed), { headers: { ETag: 'new' } });
  const state = await client.refresh();
  assert.equal(state.catalog.catalog_id, 'restarted');
  assert.equal(state.catalog.recipes[0].options[0].maximum, 4);
});
for (const [status, expected] of [[401, 'authentication'], [403, 'authentication'], [404, 'missing'], [503, 'unreachable']]) test('HTTP failure has distinct state ' + status, async () => {
  reply = () => new Response('{}', { status });
  const result = await client.refresh();
  assert.equal(result.status, expected);
  assert(!('catalog' in result));
  reply = () => new Response(JSON.stringify(fixture('catalog-fourth')));
  assert.equal((await client.refresh()).status, 'ready');
  assert.equal(calls.at(-1).options.headers.get('If-None-Match'), null);
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
  await tick(); callbacks.connection('http://three.test', 'new-private-key');
  release(new Response(JSON.stringify(fixture('catalog-fourth'))));
  await assert.rejects(pending, /connection changed/);
  reply = () => new Response(JSON.stringify(fixture('controls')));
  assert.equal((await client.refresh()).catalog.recipes[0].recipe, 'constructor');
  assert.equal(calls.at(-1).options.headers.get('If-None-Match'), null);
});
test('bound mutation cannot retry or retarget after connection change', async () => {
  const snapshot = serverConfig.getSnapshot();
  callbacks.connection('http://four.test', 'new-private-key');
  const before = calls.length;
  await assert.rejects(serverConfig.fetchForConnection(snapshot, '/load', { method: 'POST' }), /connection changed/);
  assert.equal(calls.length, before);
});
test('selector identity is independent of suffix/UI and supports compatible older hints', () => {
  const recipe = parseBackendSchema(fixture('controls')).recipes[0];
  assert.equal(backendSelector(recipe), 'constructor');
  delete recipe.backend_option; assert.equal(backendSelector(recipe), 'constructor');
  delete recipe.options[0].ui; assert.equal(backendSelector(recipe), null);
  assert.equal(own({}, 'constructor'), undefined);
});
test('recipe-name snapshots replace old connection names and handle prototype identifiers', async () => {
  const names = load('utils/recipeNames.ts');
  const React = require(path.resolve(renderer, '../../node_modules/react'));
  const { renderToStaticMarkup } = require(path.resolve(renderer, '../../node_modules/react-dom/server'));
  const View = () => React.createElement('span', null, JSON.stringify(names.useRecipeDisplayNames()));
  names.replaceRecipeDisplayNames('catalog', JSON.parse('{"constructor":{"display_name":"Remote constructor"},"old":{"display_name":"Old name"}}'), serverConfig.getSnapshot());
  assert(renderToStaticMarkup(React.createElement(View)).includes('Remote constructor'));
  callbacks.connection('http://five.test', 'same-private-key');
  const html = renderToStaticMarkup(React.createElement(View));
  assert(!html.includes('Old name')); assert(!html.includes('Remote constructor')); assert(html.includes('Lemonade Router'));
});
test('injected browser bridge notifies local and cross-tab settings while staying on origin', async () => {
  const server = fs.readFileSync(path.resolve(renderer, '../../../cpp/server/server.cpp'), 'utf8');
  const script = server.slice(server.indexOf('const settingsListeners'), server.indexOf('</script>', server.indexOf('const settingsListeners')));
  const storage = new Map(); const win = new EventTarget(); win.location = { origin: 'http://served.test' };
  const context = { window: win, navigator: { platform: 'client-only' }, localStorage: { getItem: key => storage.get(key), setItem: (key, value) => storage.set(key, value) }, Set, parseInt };
  vm.runInNewContext(script, context);
  let settingsEvents = 0; const connections = [];
  win.api.onSettingsUpdated(() => settingsEvents++);
  const unsubscribe = win.api.onConnectionSettingsUpdated((url, key) => connections.push([url, key]));
  await win.api.saveSettings({ apiUrl: 'http://ignored-client-url', apiKey: { value: 'private-browser-key' } });
  assert.equal(settingsEvents, 1); assert.deepEqual(connections[0], ['http://served.test', 'private-browser-key']);
  storage.set('lemonade-settings', JSON.stringify({ apiKey: { value: '' } }));
  const event = new Event('storage'); event.key = 'lemonade-settings'; win.dispatchEvent(event); await tick();
  assert.deepEqual(connections[1], ['http://served.test', '']);
  unsubscribe(); await win.api.saveSettings({}); assert.equal(connections.length, 2);
});
stub('utils/downloadTracker.ts', { downloadTracker: new Proxy({}, { get: () => () => {} }) });
const { catalogBackend, ensureModelReady } = load('utils/backendInstaller.ts');
test('selector reset uses fresh server defaults instead of a stale saved backend', () => {
  const recipe = parseBackendSchema(fixture('controls')).recipes[0];
  const context = { recipe, effective: { constructor: '' }, connection: serverConfig.getSnapshot() };
  assert.equal(catalogBackend(context, { constructor: { default_backend: 'cpu' } }), 'cpu');
  context.effective.constructor = 'server-gpu'; assert.equal(catalogBackend(context, {}), 'server-gpu');
  delete recipe.backend_option; delete recipe.options[0].ui;
  assert.equal(catalogBackend(context, {}), null);
});
test('catalog load validates before any side effect and sends exact typed API names', async () => {
  const recipe = parseBackendSchema(fixture('catalog-fourth')).recipes[0];
  const context = { recipe, effective: { catalog_fourth_batch: 4 }, connection: serverConfig.getSnapshot() };
  const models = { m: { recipe: 'catalog-fourth', downloaded: true } };
  const posts = [];
  reply = (url, options) => {
    if (options.method === 'POST') { posts.push([url, JSON.parse(options.body)]); return new Response('{}'); }
    return new Response(JSON.stringify(url.endsWith('/system-info') ? { recipes: {} } : { recipe: 'catalog-fourth', downloaded: true }));
  };
  await assert.rejects(ensureModelReady('m', models, { catalog: context, loadBody: { catalog_fourth_batch: 9 } }), /Invalid option/);
  assert.equal(posts.length, 0);
  await ensureModelReady('m', models, { catalog: context, loadBody: { catalog_fourth_batch: 4, pinned: true } });
  assert.deepEqual(posts, [[context.connection.apiUrl + '/load', { catalog_fourth_batch: 4, pinned: true, model_name: 'm', save_options: false }]]);
});
test('a switch during preflight prevents every subsequent install/pull/load mutation', async () => {
  const recipe = parseBackendSchema(fixture('catalog-fourth')).recipes[0];
  const connection = serverConfig.getSnapshot(); let posts = 0;
  reply = (url, options) => {
    if (options.method === 'POST') posts++;
    callbacks.connection('http://six.test', 'private-six-key');
    return new Response(JSON.stringify({ recipes: {} }));
  };
  await assert.rejects(ensureModelReady('m', { m: { recipe: 'catalog-fourth' } }, { catalog: { recipe, connection, effective: {} }, loadBody: { catalog_fourth_batch: 4 } }), /connection changed/);
  assert.equal(posts, 0);
});
test('arbitrary selector installs the selected server backend and stops for setup actions', async () => {
  const recipe = parseBackendSchema(fixture('controls')).recipes[0];
  const connection = serverConfig.getSnapshot();
  const posts = [];
  reply = (url, options) => {
    if (options.method === 'POST') {
      posts.push([url, JSON.parse(options.body)]);
      return new Response(JSON.stringify({ action: 'https://example.test/setup' }));
    }
    return new Response(JSON.stringify({ recipes: { constructor: { default_backend: 'cpu', backends: {
      'server-gpu': { state: 'installable', message: 'Server setup', action: '' },
    } } } }));
  };
  await assert.rejects(ensureModelReady('m', { m: { recipe: 'constructor', downloaded: true } }, {
    catalog: { connection, recipe, effective: { constructor: 'server-gpu' } }, loadBody: { constructor: 'server-gpu' },
  }), /Complete the server backend setup/);
  assert.deepEqual(posts, [[connection.apiUrl + '/install', { recipe: 'constructor', backend: 'server-gpu', stream: true, subscribe: false }]]);
});
test('switch after install dispatch prevents any subsequent load or pull', async () => {
  const recipe = parseBackendSchema(fixture('controls')).recipes[0];
  const connection = serverConfig.getSnapshot();
  const posts = [];
  reply = (url, options) => {
    if (options.method === 'POST') {
      posts.push(url);
      callbacks.connection('http://seven.test', 'private-seven-key');
      return new Response(JSON.stringify({ action: 'https://example.test/setup' }));
    }
    return new Response(JSON.stringify({ recipes: { constructor: { default_backend: 'cpu', backends: {
      cpu: { state: 'installable', message: '', action: '' },
    } } } }));
  };
  await assert.rejects(ensureModelReady('m', { m: { recipe: 'constructor' } }, {
    catalog: { connection, recipe, effective: { constructor: 'cpu' } }, loadBody: { constructor: 'cpu' },
  }), /connection changed/);
  assert.deepEqual(posts, [connection.apiUrl + '/install']);
});

test('ordinary external load and chat preflight also use the catalog and transient options', async () => {
  const { backendCatalogClient } = load('utils/backendCatalog.ts');
  backendCatalogClient.clear();
  const posts = [];
  const connection = serverConfig.getSnapshot();
  reply = (url, options) => {
    if (url.endsWith('/backends/schema')) return new Response(JSON.stringify(fixture('catalog-fourth')));
    if (url.endsWith('/options')) {
      const body = JSON.parse(options.body); posts.push([url, body]);
      return new Response(JSON.stringify({ recipe: 'catalog-fourth', model_name: 'm',
        saved: {}, defaults: { catalog_fourth_batch: 2 }, effective: { catalog_fourth_batch: 2 } }));
    }
    if (url.endsWith('/system-info')) return new Response('{"recipes":{}}');
    if (options.method === 'POST') { posts.push([url, JSON.parse(options.body)]); return new Response('{}'); }
    return new Response('{"recipe":"catalog-fourth","downloaded":true}');
  };
  await ensureModelReady('m', { m: { recipe: 'catalog-fourth', downloaded: true } });
  assert.deepEqual(posts, [
    [connection.apiUrl + '/models/m/options', { dry_run: true }],
    [connection.apiUrl + '/load', { model_name: 'm', save_options: false }],
  ]);
});

test('external chat preserves an already-loaded transient override', async () => {
  load('utils/backendCatalog.ts').backendCatalogClient.clear();
  const posts = [];
  reply = (url, options) => {
    if (url.endsWith('/backends/schema')) return new Response(JSON.stringify(fixture('catalog-fourth')));
    if (options.method === 'POST') posts.push(url);
    return new Response(JSON.stringify({ all_models_loaded: [{ model_name: 'm' }] }));
  };
  await ensureModelReady('m', { m: { recipe: 'catalog-fourth', downloaded: true } });
  assert.deepEqual(posts, []);
});

run();
