const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const filename = path.resolve(__dirname, '../src/features/chatHistory/historySettings.ts');
const source = fs.readFileSync(filename, 'utf8')
  .replace("import { storageKey } from '../../storage';", "const storageKey = (key: string) => `lemonade:${key}`;");
const { outputText } = ts.transpileModule(source, {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
});
const settings = {};
Function('exports', outputText)(settings);

const values = new Map();
global.localStorage = {
  getItem: key => values.get(key) ?? null,
  setItem: (key, value) => values.set(key, String(value)),
};
global.window = { dispatchEvent() {} };
global.CustomEvent = class CustomEvent {
  constructor(type, init) { this.type = type; this.detail = init?.detail; }
};

assert.equal(settings.loadChatHistoryPreference(), true, 'new clients save history by default');
settings.saveChatHistoryPreference(false);
assert.equal(settings.loadChatHistoryPreference(), false, 'explicit opt-out must survive reload');
settings.saveChatHistoryPreference(true);
assert.equal(settings.loadChatHistoryPreference(), true, 'users can re-enable history');
values.set('lemonade:persist_conversations', 'invalid');
assert.equal(settings.loadChatHistoryPreference(), false, 'unexpected stored values must not opt in');
global.localStorage.getItem = () => { throw new Error('storage blocked'); };
assert.equal(settings.loadChatHistoryPreference(), false, 'unavailable storage does not promise persistence');

console.log('Chat history defaults on without overriding explicit preferences.');
