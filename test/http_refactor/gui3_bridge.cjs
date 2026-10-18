#!/usr/bin/env node
'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

if (process.env.LEMONADE_HTTP_REFACTOR_ISOLATED !== '1') {
  throw new Error('Use server_contract.py --gui3; direct runs are forbidden.');
}
const url = new URL(process.env.LEMONADE_HTTP_REFACTOR_URL || '');
if (url.protocol !== 'http:' || url.hostname !== '127.0.0.1' || !/^\d+$/.test(url.port)) {
  throw new Error('GUI3 contract requires a loopback server URL with an explicit port.');
}
const appDir = path.resolve(__dirname, '../../src/app');
const ts = require(require.resolve('typescript', { paths: [appDir] }));
const apiFile = path.join(appDir, 'src/api.ts');
const originalExtension = require.extensions['.ts'];
if (!globalThis.crypto) globalThis.crypto = crypto.webcrypto;
require.extensions['.ts'] = (module, filename) => {
  const source = fs.readFileSync(filename, 'utf8');
  const transpiled = ts.transpileModule(source, {
    fileName: filename,
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS,
      esModuleInterop: true,
    },
    reportDiagnostics: true,
  });
  const errors = (transpiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error);
  assert.deepEqual(errors.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')), []);
  module._compile(transpiled.outputText, filename);
};

(async () => {
  const { api } = require(apiFile);
  api.baseUrl = url.origin;
  api.setSessionApiKey('');
  api.setSessionAdminApiKey('');
  const modelName = 'user.HttpBridge' + crypto.randomBytes(6).toString('hex');
  let registered = false;
  let checks = 0;
  const verify = (condition, message) => {
    assert.ok(condition, message);
    checks += 1;
  };
  try {
    const health = await api.health();
    verify(health.status === 'ok', 'GUI3 health status changed');
    verify(Array.isArray(health.all_models_loaded), 'GUI3 cannot normalize loaded models');

    const models = await api.models(true);
    verify(Array.isArray(models.data) && models.data.length > 0, 'GUI3 model list is invalid');
    const before = await api.getRuntimeConfig();
    verify(before.offline === true, 'GUI3 connected to an unexpected/non-isolated server');
    verify(typeof before.models_dir === 'string', 'GUI3 config lost models_dir');
    verify(before.disable_model_filtering === true,
      'GUI3 bridge must run with test-only backend filtering disabled');
    verify(before.no_fetch_executables === true, 'GUI3 bridge must never download executables');
    const originalLevel = await api.getLogLevel();
    verify(originalLevel === before.log_level, 'GUI3 log-level getter changed');
    const sameLevel = await api.setLogLevel(originalLevel);
    verify(sameLevel.status === 'success' && sameLevel.level === originalLevel,
      'GUI3 settings POST is incompatible with /internal/set');
    verify((await api.getLogLevel()) === originalLevel, 'GUI3 settings POST unexpectedly changed the level');

    verify(Array.isArray(await api.downloads()), 'GUI3 cannot read downloads');
    verify(Array.isArray(await api.listJobs()), 'GUI3 cannot read jobs');
    verify(typeof (await api.stats()) === 'object', 'GUI3 cannot read stats');
    verify(typeof (await api.systemStats()) === 'object', 'GUI3 cannot read system stats');
    verify(typeof (await api.systemInfo()) === 'object', 'GUI3 cannot read system info');

    const register = await api.registerModelDefinition(modelName, {
      recipe: 'llamacpp', checkpoint: 'lemonade-compat/no-network:Q4_K_M', labels: ['chat'],
    });
    registered = true;
    verify(register.canonical_model_name === modelName, 'GUI3 registration response changed');
    const id = register.model_name;
    verify(typeof id === 'string' && id.length > 0, 'GUI3 public model name invalid');

    const after = await api.models(true);
    verify(after.data.some(item => item.id === id), 'GUI3 cache did not refresh after register');
    const detail = await api.modelDetail(id);
    verify(detail.checkpoint === 'lemonade-compat/no-network:Q4_K_M', 'GUI3 model detail is incompatible');
    const files = await api.getModelFiles(id);
    verify(files !== null && Array.isArray(files.files), 'GUI3 files endpoint is incompatible');

    const options = await api.getModelOptions(id);
    verify(options && typeof options.saved === 'object', 'GUI3 model options shape changed');
    const changed = await api.saveModelOptions(id, { ctx_size: 8192 });
    verify(changed.saved.ctx_size === 8192, 'GUI3 cannot save model context option');
    verify((await api.getModelOptions(id)).saved.ctx_size === 8192,
      'GUI3 cannot retrieve saved model option');
    const reset = await api.resetModelOptions(id);
    verify(!Object.hasOwn(reset.saved, 'ctx_size'), 'GUI3 cannot reset saved model option');

    await api.deleteModel(modelName);
    registered = false;
    verify(!(await api.models(true)).data.some(item => item.id === id),
      'GUI3 model still visible after deletion');
    console.log(`GUI3 real API checks passed: ${checks}`);
  } finally {
    if (registered) {
      try { await api.deleteModel(modelName); } catch (error) { console.error('cleanup:', error); }
    }
    if (originalExtension) require.extensions['.ts'] = originalExtension;
    else delete require.extensions['.ts'];
  }
})().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exitCode = 1;
});
