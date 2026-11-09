const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

require.extensions['.ts'] = function transpileTs(module, filename) {
  const output = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS, esModuleInterop: true },
    fileName: filename,
  }).outputText;
  module._compile(output, filename);
};

const storage = new Map();
global.localStorage = {
  getItem(key) { return storage.has(key) ? storage.get(key) : null; },
  setItem(key, value) { storage.set(key, String(value)); },
  removeItem(key) { storage.delete(key); },
};
global.window = { addEventListener() {}, removeEventListener() {}, location: { hostname: 'localhost', pathname: '/' } };
URL.createObjectURL = () => 'blob:clip';

const { api } = require(path.resolve(__dirname, '../src/api.ts'));

function json(body) {
  return new Response(JSON.stringify(body), { status: 200, headers: { 'Content-Type': 'application/json' } });
}

(async () => {
  // Polls and the download go to the server that created the job, with its
  // key, even after the connection settings move to another server.
  api.baseUrl = 'http://server-a:13305';
  api.apiKey = 'key-a';
  const requests = [];
  let polls = 0;
  global.fetch = async (url, init = {}) => {
    requests.push({ url: String(url), auth: init.headers?.Authorization });
    if (String(url).endsWith('/api/v1/videos')) return json({ id: 'job-1', status: 'queued' });
    if (String(url).endsWith('/content')) return new Response(new Blob(['clip']), { status: 200 });
    polls += 1;
    if (polls === 1) {
      api.baseUrl = 'http://server-b:13305';
      api.apiKey = 'key-b';
    }
    return json({ id: 'job-1', status: polls < 2 ? 'running' : 'completed', output_format: 'webm' });
  };

  const result = await api.videoGeneration('Wan', 'a cat', {}, {});
  assert.equal(result.blob.size, 4);
  assert.ok(requests.length >= 4);
  for (const request of requests) {
    assert.ok(request.url.startsWith('http://server-a:13305/'), `${request.url} went to the wrong server`);
    assert.equal(request.auth, 'Bearer key-a');
  }

  // A resumed job without a remembered key goes back to its own server, and
  // the key for the server now in the settings is not sent there.
  api.baseUrl = 'http://server-b:13305';
  api.apiKey = 'key-b';
  requests.length = 0;
  global.fetch = async (url, init = {}) => {
    requests.push({ url: String(url), auth: init.headers?.Authorization });
    if (String(url).endsWith('/content')) return new Response(new Blob(['clip']), { status: 200 });
    return json({ id: 'job-2', status: 'completed', output_format: 'webm' });
  };
  const resumed = await api.resumeVideoGeneration(
    { id: 'job-2', baseUrl: 'http://server-a:13305', model: 'Wan' },
    {},
  );
  assert.equal(resumed.blob.size, 4);
  assert.ok(requests.length >= 2);
  for (const request of requests) {
    assert.ok(request.url.startsWith('http://server-a:13305/'), `${request.url} went to the wrong server`);
    assert.equal(request.auth, undefined);
  }

  console.log('Video job server affinity checks passed.');
})().catch(err => {
  console.error(err);
  process.exit(1);
});
