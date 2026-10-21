const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require('typescript');

const filename = path.resolve(__dirname, '../src/api.ts');
const source = fs.readFileSync(filename, 'utf8');
const parsed = ts.createSourceFile(filename, source, ts.ScriptTarget.Latest, true);
const apiClass = parsed.statements.find(s => ts.isClassDeclaration(s) && s.name?.text === 'LemonadeAPI');
assert.ok(apiClass);
const methods = ['canPersistApiKey', 'loadConnectionSettings', 'saveConnectionSettings'].map(name => {
  const member = apiClass.members.find(m => m.name?.getText(parsed) === name);
  assert.ok(member, `${name} missing`);
  return member.getText(parsed);
});
const program = `class TestAPI {
  _sessionApiKey = '';
  _hostBaseUrl = null;
  _connectionSettingsPromise = null;
  ${methods.join('\n')}
}
module.exports = TestAPI;`;
const compiled = ts.transpileModule(program, {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  reportDiagnostics: true,
});
assert.deepEqual((compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error), []);

function makeClient(webApp, startingKey = '') {
  const settings = { apiUrl: 'http://localhost:13305', apiKey: { value: startingKey } };
  const saved = [];
  const host = webApp === null ? null : {
    isWebApp: webApp,
    getSettings: async () => ({ ...settings }),
    saveSettings: async value => { saved.push(value); Object.assign(settings, value); },
  };
  const storage = new Map();
  const context = {
    module: { exports: {} },
    getHostSettingsApi: () => host,
    waitForHostSettingsApi: async () => host,
    safeRemoveLocalStorage: key => storage.delete(key),
    safeSetLocalStorage: (key, value) => storage.set(key, value),
    LS_BASE_URL: 'base_url', LS_API_KEY: 'api_key',
    normalizeBaseUrl: v => v,
    isObject: v => v !== null && typeof v === 'object' && !Array.isArray(v),
    typedSettingString: (obj, key) => obj[key]?.value || '',
    mergeConnectionSettings: (current, baseUrl, key) => ({
      ...current, baseURL: { value: baseUrl }, apiKey: { value: key },
    }),
    setTimeout, Promise,
  };
  vm.runInNewContext(compiled.outputText, context);
  return { client: new context.module.exports(), settings, saved, storage };
}

(async () => {
  const browser = makeClient(true, 'previously-stored-secret');
  assert.equal(browser.client.canPersistApiKey, false);
  await browser.client.loadConnectionSettings();
  assert.equal(browser.client._sessionApiKey, '');
  assert.equal(browser.settings.apiKey.value, '', 'migration must clear a stale browser key');
  assert.equal(browser.saved.length, 1);
  const outcome = await browser.client.saveConnectionSettings('http://localhost:13305', 'new-secret', true);
  assert.equal(outcome.apiKeyPersisted, false);
  assert.equal(browser.settings.apiKey.value, '', 'browser setting must never store the new secret');
  assert.equal(browser.client._sessionApiKey, 'new-secret', 'the current connection keeps a session key');

  const desktop = makeClient(false, 'native-key');
  assert.equal(desktop.client.canPersistApiKey, true);
  await desktop.client.loadConnectionSettings();
  assert.equal(desktop.client._sessionApiKey, 'native-key');
  const nativeOutcome = await desktop.client.saveConnectionSettings('http://localhost:13305', 'remember', true);
  assert.equal(nativeOutcome.apiKeyPersisted, true);
  assert.equal(desktop.settings.apiKey.value, 'remember');

  const noHost = makeClient(null);
  assert.equal(noHost.client.canPersistApiKey, false);
  const noHostOutcome = await noHost.client.saveConnectionSettings('http://localhost:13305', 'temporary', true);
  assert.equal(noHostOutcome.apiKeyPersisted, false);
  console.log('Browser API-key persistence regression checks passed.');
})().catch(err => { console.error(err); process.exitCode = 1; });
