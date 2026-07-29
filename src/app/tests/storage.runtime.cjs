const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const modulePath = path.resolve(__dirname, '../src/storage.ts');
const values = new Map();
const localStorage = {
  get length() { return values.size; },
  key: index => [...values.keys()][index] || null,
  getItem: key => values.has(key) ? values.get(key) : null,
  setItem: (key, value) => values.set(key, String(value)),
  removeItem: key => values.delete(key),
};
global.localStorage = new Proxy(localStorage, {
  ownKeys: () => [...values.keys()],
  getOwnPropertyDescriptor: (_, key) => values.has(key)
    ? { enumerable: true, configurable: true }
    : undefined,
});

const source = fs.readFileSync(modulePath, 'utf8');
const compiled = ts.transpileModule(source, {
  compilerOptions: {
    target: ts.ScriptTarget.ES2020,
    module: ts.ModuleKind.CommonJS,
  },
  fileName: modulePath,
  reportDiagnostics: true,
});
assert.equal((compiled.diagnostics || []).length, 0);
const moduleRecord = { exports: {} };
Function('exports', 'require', 'module', '__filename', '__dirname', compiled.outputText)(
  moduleRecord.exports,
  require,
  moduleRecord,
  modulePath,
  path.dirname(modulePath),
);

values.set('lemonade:user:a:conversations', JSON.stringify({
  version: 3,
  conversations: [{ id: 'shared', title: 'A', messages: [], updatedAt: 1 }],
}));
values.set('lemonade:user:b:conversations', JSON.stringify({
  version: 3,
  conversations: [
    { id: 'shared', title: 'B', messages: [], updatedAt: 2 },
    { id: 'b-only', title: 'B only', messages: [], updatedAt: 3 },
  ],
}));
values.set('lemonade:user:a:active_conversation', 'shared');
values.set('lemonade:user:b:active_conversation', 'shared');
values.set('lemonade:user:a:global_model_settings', JSON.stringify({ resourceBudgetGb: 8, loadingPolicy: 'single-active' }));
values.set('lemonade:user:b:global_model_settings', JSON.stringify({ autoEvictOnPressure: true, evictionPolicy: 'largest' }));
values.set('lemonade:user:a:mcp_server_ids', JSON.stringify(['a']));
values.set('lemonade:user:b:mcp_server_ids', JSON.stringify(['b']));

assert.equal(moduleRecord.exports.storageKey('conversations'), 'lemonade:conversations');

const conversations = JSON.parse(values.get('lemonade:conversations')).conversations;
assert.equal(conversations.length, 3);
assert.deepEqual(conversations.map(item => item.title).sort(), ['A', 'B', 'B only']);
assert.equal(values.get('lemonade:active_conversation'), 'shared');

const settings = JSON.parse(values.get('lemonade:global_model_settings'));
assert.equal(settings.resourceBudgetGb, 8);
assert.equal(settings.loadingPolicy, 'single-active');
assert.equal(settings.autoEvictOnPressure, true);
assert.equal(settings.evictionPolicy, 'largest');
assert.deepEqual(JSON.parse(values.get('lemonade:mcp_server_ids')).sort(), ['a', 'b']);

const conflicts = JSON.parse(values.get('lemonade:storage_migration_conflicts_v1'));
assert.ok(Object.keys(conflicts).some(key => key.startsWith('user:b:active_conversation')));
assert.equal(localStorage.getItem('lemonade:user:a:conversations'), null);
assert.equal(localStorage.getItem('lemonade:user:b:conversations'), null);
assert.equal(localStorage.getItem('lemonade:lemonade_storage_migrated_v1'), null);
assert.equal(values.get('lemonade_storage_migrated_v2'), 'true');

console.log('Client storage migration preserves scoped conversations and merges compatible settings.');
