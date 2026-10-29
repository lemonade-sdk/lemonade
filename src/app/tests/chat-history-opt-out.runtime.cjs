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

const stored = [
  { id: 'a', messages: [] },
  { id: 'b', messages: [] },
  { title: 'legacy entry without id', messages: [] },
];

assert.deepEqual(
  settings.retainSavedConversations(stored, new Set(['a', 'b', 'new'])),
  stored,
  'opting out keeps every conversation saved earlier',
);
assert.deepEqual(
  settings.retainSavedConversations(stored, new Set(['a'])).map(entry => entry.id),
  ['a', undefined],
  'deleting a saved conversation while opted out removes it from storage',
);
assert.deepEqual(
  settings.retainSavedConversations(stored, new Set()).map(entry => entry.id),
  [undefined],
  'clearing every conversation keeps only entries that cannot be matched',
);

const saved = { updatedAt: 1, messages: [{}, {}] };
assert.equal(settings.conversationVersion(saved), settings.conversationVersion({ ...saved }));
assert.notEqual(
  settings.conversationVersion(saved),
  settings.conversationVersion({ updatedAt: 1, messages: [{}, {}, {}] }),
  'a new message marks a saved conversation as changed',
);
assert.notEqual(
  settings.conversationVersion(saved),
  settings.conversationVersion({ updatedAt: 2, messages: [{}, {}] }),
  'an edit marks a saved conversation as changed',
);

console.log('Opting out of chat history keeps previously saved conversations.');
