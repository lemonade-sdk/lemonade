const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const webpack = require('webpack');

async function bundle(entry, filename) {
  const outputPath = fs.mkdtempSync(path.join(os.tmpdir(), 'lemonade-device-selection-'));
  const config = {
    mode: 'development',
    target: 'node',
    entry: path.resolve(__dirname, entry),
    output: { path: outputPath, filename, library: { type: 'commonjs2' } },
    resolve: { extensions: ['.ts', '.tsx', '.js'] },
    module: { rules: [{ test: /\.tsx?$/, use: 'ts-loader', exclude: /node_modules/ }] },
    optimization: { minimize: false },
  };
  await new Promise((resolve, reject) => {
    webpack(config, (error, stats) => {
      if (error) return reject(error);
      if (stats?.hasErrors()) return reject(new Error(stats.toString({ all: false, errors: true })));
      resolve();
    });
  });
  return { outputPath, modulePath: path.join(outputPath, filename) };
}

function systemInfoFor(name) {
  return { 'OS Version': name, devices: { nvidia_gpu: [{ index: 0, name, uuid: `GPU-${name}`, available: true }] } };
}

(async () => {
  const apiBundle = await bundle('../src/api.ts', 'api.cjs');
  try {
    // System-info belongs to one server: moving the app to another drops it.
    const values = new Map();
    global.localStorage = {
      getItem: key => (values.has(key) ? values.get(key) : null),
      setItem: (key, value) => values.set(key, String(value)),
      removeItem: key => values.delete(key),
    };
    const requested = [];
    global.fetch = async (url) => {
      requested.push(String(url));
      const host = new URL(String(url)).host;
      const body = systemInfoFor(host);
      return { ok: true, status: 200, headers: new Map([['content-type', 'application/json']]), json: async () => body, text: async () => JSON.stringify(body) };
    };
    const { api } = (() => { const m = require(apiBundle.modulePath); return { api: m.default || m.api }; })();

    api.baseUrl = 'http://server-a.test:13305';
    const changes = [];
    api.onServerChange(() => changes.push(api.baseUrl));
    const a = await api.systemInfo();
    assert.equal(a['OS Version'], 'server-a.test:13305');
    assert.equal(api.systemInfoData['OS Version'], 'server-a.test:13305');

    // Same server again: nothing is dropped, nobody is told.
    api.baseUrl = 'http://server-a.test:13305';
    assert.equal(api.systemInfoData['OS Version'], 'server-a.test:13305');
    assert.deepEqual(changes, []);

    // Another server: the cached copy is gone at once, listeners hear about it,
    // and the next read is the new server's.
    api.baseUrl = 'http://server-b.test:13305';
    assert.equal(api.systemInfoData, null);
    assert.deepEqual(changes, ['http://server-b.test:13305']);
    const b = await api.systemInfo();
    assert.equal(b['OS Version'], 'server-b.test:13305');
    assert.equal(api.systemInfoData['OS Version'], 'server-b.test:13305');
    assert.ok(requested.some(url => url.startsWith('http://server-b.test:13305/')));

    // A request still in flight for the old server must not be served, or stored,
    // for the new one.
    let release;
    const gate = new Promise(resolve => { release = resolve; });
    const slowFetch = global.fetch;
    global.fetch = async (url) => { await gate; return slowFetch(url); };
    const pending = api.systemInfo({ maxAgeMs: 0 });
    api.baseUrl = 'http://server-c.test:13305';
    release();
    await pending.catch(() => undefined);
    assert.equal(api.systemInfoData, null);
    global.fetch = slowFetch;
    const c = await api.systemInfo();
    assert.equal(c['OS Version'], 'server-c.test:13305');
  } finally {
    fs.rmSync(apiBundle.outputPath, { recursive: true, force: true });
  }
  console.log('device selection runtime checks passed');
})().catch(error => { console.error(error); process.exit(1); });
