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
const methods = ['canPersistApiKey', 'isApiKeyPersisted', 'loadConnectionSettings', 'saveConnectionSettings', 'clearConnectionSettings', 'setSessionApiKey', 'clearStoredApiKey'].map(name => {
  const member = apiClass.members.find(m => m.name?.getText(parsed) === name);
  assert.ok(member, `${name} missing`);
  return member.getText(parsed);
});
const program = `class TestAPI {
  _sessionApiKey = '';
  _sessionAdminApiKey = '';
  _apiKeyPersisted = false;
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

function makeClient(webApp, startingKey = '', storedKey = '', blockedStorage = false, hasWindow = true) {
  const settings = { apiUrl: 'http://localhost:13305', apiKey: { value: startingKey } };
  const saved = [];
  const host = webApp === null ? null : {
    isWebApp: webApp,
    getSettings: async () => ({ ...settings }),
    saveSettings: async value => { saved.push(value); Object.assign(settings, value); },
  };
  const storage = new Map(storedKey ? [['api_key', storedKey]] : []);
  const localStorage = hasWindow ? {
    getItem(key) {
      if (blockedStorage) throw new Error('Storage blocked');
      return storage.has(key) ? storage.get(key) : null;
    },
    setItem(key, value) {
      if (blockedStorage) throw new Error('Storage blocked');
      storage.set(key, String(value));
    },
    removeItem(key) {
      if (blockedStorage) throw new Error('Storage blocked');
      storage.delete(key);
    },
  } : undefined;
  const context = {
    module: { exports: {} },
    window: hasWindow ? {} : undefined,
    localStorage,
    getHostSettingsApi: () => host,
    waitForHostSettingsApi: async () => host,
    safeGetLocalStorage: key => {
      try { return localStorage?.getItem(key) ?? null; } catch { return null; }
    },
    safeRemoveLocalStorage: key => {
      try { localStorage?.removeItem(key); } catch { /* unavailable */ }
    },
    safeSetLocalStorage: (key, value) => {
      try {
        if (!localStorage) return false;
        localStorage.setItem(key, value);
        return true;
      } catch { return false; }
    },
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
  // Existing browser key survives startup and the web shim cannot store a copy.
  const browser = makeClient(true, 'stale-shim-secret', 'previously-remembered-key');
  assert.equal(browser.client.canPersistApiKey, true);
  await browser.client.loadConnectionSettings();
  assert.equal(browser.client._sessionApiKey, 'previously-remembered-key');
  assert.equal(browser.client.isApiKeyPersisted, true);
  assert.equal(browser.settings.apiKey.value, '', 'scrub old key from browser shim');
  assert.equal(browser.saved.length, 1);
  browser.client.setSessionApiKey('current-session-key');
  assert.equal(browser.storage.get('api_key'), 'previously-remembered-key',
    'setting the session key must not destroy the remembered key');
  const outcome = await browser.client.saveConnectionSettings('http://localhost:13305', 'new-secret', true);
  assert.equal(outcome.apiKeyPersisted, true);
  assert.equal(browser.storage.get('api_key'), 'new-secret');
  assert.equal(browser.settings.apiKey.value, '', 'browser shim must never get the secret');
  assert.equal(browser.client.isApiKeyPersisted, true);

  const restored = makeClient(true, '', browser.storage.get('api_key'));
  await restored.client.loadConnectionSettings();
  assert.equal(restored.client._sessionApiKey, 'new-secret', 'reload must restore the key');
  assert.equal(restored.client.isApiKeyPersisted, true);
  const transient = await restored.client.saveConnectionSettings('http://localhost:13305', 'once-only', false);
  assert.equal(transient.apiKeyPersisted, false);
  assert.equal(restored.storage.has('api_key'), false, 'unchecking remember must remove the key');
  assert.equal(restored.client._sessionApiKey, 'once-only', 'session continues using the key');
  assert.equal(restored.client.isApiKeyPersisted, false, 'UI must not re-check remember on remount');

  const cleared = makeClient(true, '', 'remembered');
  await cleared.client.loadConnectionSettings();
  await cleared.client.clearConnectionSettings();
  assert.equal(cleared.client._sessionApiKey, '');
  assert.equal(cleared.client.isApiKeyPersisted, false);
  assert.equal(cleared.storage.has('api_key'), false, 'clearing local data removes the key');

  const desktop = makeClient(false, 'native-key', 'obsolete-browser-copy');
  assert.equal(desktop.client.canPersistApiKey, true);
  await desktop.client.loadConnectionSettings();
  assert.equal(desktop.client._sessionApiKey, 'native-key');
  assert.equal(desktop.client.isApiKeyPersisted, true);
  assert.equal(desktop.storage.has('api_key'), false, 'native client must not keep browser key');
  const nativeOutcome = await desktop.client.saveConnectionSettings('http://localhost:13305', 'remember', true);
  assert.equal(nativeOutcome.apiKeyPersisted, true);
  assert.equal(desktop.settings.apiKey.value, 'remember');
  const nativeTransient = await desktop.client.saveConnectionSettings('http://localhost:13305', 'temporary', false);
  assert.equal(nativeTransient.apiKeyPersisted, false);
  assert.equal(desktop.settings.apiKey.value, '');

  const noHost = makeClient(null, '', 'standalone-browser-secret');
  assert.equal(noHost.client.canPersistApiKey, true, 'browser without a shim can persist locally');
  await noHost.client.loadConnectionSettings();
  assert.equal(noHost.client._sessionApiKey, 'standalone-browser-secret');
  const noHostOutcome = await noHost.client.saveConnectionSettings('http://localhost:13305', 'remember', true);
  assert.equal(noHostOutcome.apiKeyPersisted, true);
  assert.equal(noHost.storage.get('api_key'), 'remember');

  const blocked = makeClient(true, '', '', true);
  assert.equal(blocked.client.canPersistApiKey, false, 'storage-blocked browser cannot remember');
  const blockedOutcome = await blocked.client.saveConnectionSettings('http://localhost:13305', 'secret', true);
  assert.equal(blockedOutcome.apiKeyPersisted, false);

  const headless = makeClient(null, '', '', false, false);
  assert.equal(headless.client.canPersistApiKey, false);
  const headlessOutcome = await headless.client.saveConnectionSettings('http://localhost:13305', 'temporary', true);
  assert.equal(headlessOutcome.apiKeyPersisted, false);
  console.log('Browser API-key persistence regression checks passed.');
})().catch(err => { console.error(err); process.exitCode = 1; });
