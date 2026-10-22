#!/usr/bin/env node
'use strict';

// Called exclusively by server_contract.py after spawning an isolated lemond.
// Executes GUI3's real, transpiled TypeScript API client against that daemon.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { webcrypto } = require('node:crypto');

if (process.env.LEMONADE_MODEL_COMPAT_ISOLATED !== '1') {
  throw new Error('Refusing to mutate a non-isolated server. Run via server_contract.py --gui3.');
}
const url = new URL(process.env.LEMONADE_MODEL_COMPAT_URL || '');
if (url.protocol !== 'http:' || !['127.0.0.1', 'localhost', '[::1]'].includes(url.hostname)) {
  throw new Error('GUI3 bridge requires a local HTTP server started by the harness.');
}

const appDir = path.resolve(__dirname, '../..', 'src/app');
const ts = require(require.resolve('typescript', { paths: [appDir] }));
const apiPath = path.join(appDir, 'src/api.ts');
const originalLoader = require.extensions['.ts'];
if (!globalThis.crypto) globalThis.crypto = webcrypto;
require.extensions['.ts'] = (module, filename) => {
  const code = fs.readFileSync(filename, 'utf8');
  const compiled = ts.transpileModule(code, {
    fileName: filename,
    compilerOptions: { target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS, esModuleInterop: true },
    reportDiagnostics: true,
  });
  const errors = (compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error);
  assert.deepEqual(errors, [], errors.map(e => ts.flattenDiagnosticMessageText(e.messageText, '\n')).join('\n'));
  module._compile(compiled.outputText, filename);
};

(async () => {
  const { api } = require(apiPath);
  api.baseUrl = url.origin;
  api.setSessionApiKey(process.env.LEMONADE_API_KEY || '');
  api.setSessionAdminApiKey(process.env.LEMONADE_ADMIN_API_KEY || '');
  const name = 'user.Gui3Bridge-' + require('node:crypto').randomBytes(5).toString('hex');
  const checkpoint = 'lemonade-compat/no-network:Q4_K_M';
  let registered = false;
  let checks = 0;
  const check = (ok, message) => { assert.ok(ok, message); checks += 1; };
  try {
    const health = await api.health();
    check(Array.isArray(health.all_models_loaded), 'GUI3 failed to normalize /health');
    const list = await api.models(true);
    check(Array.isArray(list.data) && list.data.length > 0, 'GUI3 failed to parse /models');

    const result = await api.registerModelDefinition(name, {
      recipe: 'llamacpp', checkpoint, labels: ['chat'],
    });
    registered = true;
    check(result.canonical_model_name === name, 'GUI3 registration canonical name changed');
    const publicName = result.model_name;
    check(typeof publicName === 'string' && publicName.length > 0, 'Missing public name');
    const fresh = await api.models(true);
    check(fresh.data.some(item => item.id === publicName), 'GUI3 model list lost registered model');

    const detail = await api.modelDetail(publicName);
    check(detail.checkpoint === checkpoint, 'GUI3 detail uses wrong registry checkpoint');
    check(detail.downloaded === false, 'Register-only unexpectedly marked downloaded');
    const files = await api.getModelFiles(publicName);
    check(files && Array.isArray(files.files), 'GUI3 files tab cannot read model files');

    const original = await api.getModelOptions(publicName);
    check(original && typeof original.saved === 'object', 'GUI3 options GET shape invalid');
    const changed = await api.saveModelOptions(publicName, { ctx_size: 8192 });
    check(changed.saved.ctx_size === 8192, 'GUI3 options POST lost ctx_size');
    check((await api.getModelOptions(publicName)).saved.ctx_size === 8192,
      'GUI3 options not persisted on server');
    const reset = await api.resetModelOptions(publicName);
    check(!Object.hasOwn(reset.saved, 'ctx_size'), 'GUI3 options DELETE did not reset');

    await api.deleteModel(name);
    registered = false;
    check(!(await api.models(true)).data.some(item => item.id === publicName),
      'GUI3 deletion failed to remove registered model');
    console.log(`GUI3 API vs real lemond: ${checks} contract checks passed`);
  } finally {
    if (registered) {
      try { await api.deleteModel(name); } catch (error) { console.error('cleanup:', error); }
    }
    if (originalLoader) require.extensions['.ts'] = originalLoader;
    else delete require.extensions['.ts'];
  }
})().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exitCode = 1;
});
