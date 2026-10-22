'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

// Fast, dependency-free contract guards; actual behavior is tested separately
// in GUI3's *.runtime.cjs and test/model_compat/server_contract.py.
const root = path.resolve(__dirname, '../../..');
const serverPath = path.join(root, 'src/cpp/server/server.cpp');
const clientPath = path.join(root, 'src/app/src/api.ts');
const server = fs.readFileSync(serverPath, 'utf8');
const gui3Available = fs.existsSync(clientPath);
const gui3 = gui3Available ? fs.readFileSync(clientPath, 'utf8') : '';
const route = (method, name) => {
  assert.match(server, new RegExp(`register_${method}\\("${name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}"`));
};
const active = fn => gui3Available ? fn() : { skip: true, reason: 'GUI3 not merged into this branch' };

module.exports.tests = [
  { name: 'model registration route retained', run() { route('post', 'models/register'); } },
  { name: 'model list route retained', run() { route('get', 'models'); } },
  { name: 'model health route retained', run() { route('get', 'health'); } },
  { name: 'download status route retained', run() { route('get', 'downloads'); } },
  { name: 'pull route retained', run() { route('post', 'pull'); } },
  { name: 'delete route retained', run() { route('post', 'delete'); } },
  { name: 'model files registered before generic model detail', run() {
    assert.ok(server.indexOf('web_server.Get(R"(/api/v1/models/(.+)/files)"') >= 0);
    assert.ok(server.indexOf('web_server.Get(R"(/api/v1/models/(.+))"') >
      server.indexOf('web_server.Get(R"(/api/v1/models/(.+)/files)"'));
  } },
  { name: 'per-model options GET POST DELETE routes', run() {
    for (const method of ['Get', 'Post', 'Delete']) {
      assert.match(server, new RegExp(`web_server\\.${method}\\(route,`));
    }
  } },
  { name: 'GUI3 registration uses backend register-only route', run() {
    return active(() => {
      assert.match(gui3, /async registerModelDefinition\(/);
      assert.match(gui3, /_json<Record<string, unknown>>\('\/api\/v1\/models\/register'/);
    });
  } },
  { name: 'GUI3 model list uses server filtering', run() {
    return active(() => {
      assert.match(gui3, /async models\(showAll = true\)/);
      assert.match(gui3, /\/api\/v1\/models\$\{qs\}/);
    });
  } },
  { name: 'GUI3 maps missing files to null', run() {
    return active(() => {
      assert.match(gui3, /async getModelFiles\(/);
      assert.match(gui3, /\/api\/v1\/models\/\$\{encodeURIComponent\(id\)\}\/files/);
    });
  } },
  { name: 'GUI3 routes options through server', run() {
    return active(() => {
      assert.match(gui3, /async getModelOptions\(/);
      assert.match(gui3, /async saveModelOptions\(/);
      assert.match(gui3, /async resetModelOptions\(/);
      assert.match(gui3, /\/options`/);
    });
  } },
  { name: 'GUI3 keeps server-owned registry provenance', run() {
    return active(() => {
      assert.match(gui3, /registry_source/);
      assert.match(gui3, /async registrySearch\(/);
      assert.match(gui3, /async pullVariants\(/);
    });
  } },
  { name: 'GUI3 uses authoritative server download status', run() {
    return active(() => {
      assert.match(gui3, /async downloads\(/);
      assert.match(gui3, /\/api\/v1\/downloads/);
      assert.match(gui3, /async _assertModelNotDownloading\(/);
    });
  } },
  { name: 'GUI3 does not persist load-time options unintentionally', run() {
    return active(() => {
      assert.match(gui3, /async loadModel\(/);
      assert.match(gui3, /save_options: false/);
    });
  } },
];
