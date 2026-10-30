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
  { id: 'saved-by-another-tab', messages: [] },
  { id: 'line\nbreak', messages: [] },
];

assert.deepEqual(
  settings.removeDeletedConversations(stored, new Set()),
  stored,
  'opting out keeps every conversation saved earlier',
);
assert.deepEqual(
  settings.removeDeletedConversations(stored, new Set(['a'])).map(entry => entry.id),
  ['b', 'saved-by-another-tab', 'line\nbreak'],
  'deleting a chat removes only that chat, never one saved by another tab',
);
assert.deepEqual(
  settings.removeDeletedConversations(stored, new Set(['line\nbreak'])).map(entry => entry.id),
  ['a', 'b', 'saved-by-another-tab'],
  'ids are matched whole, even when they contain a newline',
);

assert.deepEqual(
  [...settings.deletedConversationIds(new Set(['a', 'b', 'c']), new Set(['a', 'c', 'new']))],
  ['b'],
  'only ids that disappeared count as deleted; new ids do not',
);

let nextId = 0;
const generate = () => `generated-${++nextId}`;
assert.equal(settings.assignMissingConversationIds(stored, generate), null, 'no write when every entry has an id');
const migrated = settings.assignMissingConversationIds(
  [{ id: 'a', messages: [] }, { title: 'Legacy chat', messages: [] }, 'not a conversation'],
  generate,
);
assert.deepEqual(
  migrated,
  [{ id: 'a', messages: [] }, { title: 'Legacy chat', messages: [], id: 'generated-1' }, 'not a conversation'],
  'legacy entries get a stable id so a later delete can match them',
);
assert.deepEqual(
  settings.removeDeletedConversations(migrated, new Set(['generated-1'])).map(entry => entry.id ?? entry),
  ['a', 'not a conversation'],
  'a migrated legacy chat can be deleted',
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
