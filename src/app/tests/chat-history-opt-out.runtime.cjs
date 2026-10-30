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

const legacy = { title: 'Legacy chat', messages: [{ role: 'user', content: 'hi' }] };
const legacyId = settings.storedConversationId(legacy);
assert.equal(settings.storedConversationId({ id: 'a' }), 'a', 'stored ids are used as is');
assert.equal(settings.storedConversationId('not a conversation'), undefined);
assert.match(legacyId, /^legacy-/);
assert.equal(
  settings.storedConversationId(JSON.parse(JSON.stringify(legacy))),
  legacyId,
  'a legacy entry gets the same id on every load without writing storage',
);
assert.notEqual(
  settings.storedConversationId({ ...legacy, title: 'Other legacy chat' }),
  legacyId,
  'different legacy entries get different ids',
);
const withLegacy = [{ id: 'a', messages: [] }, legacy, 'not a conversation'];
assert.deepEqual(
  settings.removeDeletedConversations(withLegacy, new Set([legacyId])),
  [{ id: 'a', messages: [] }, 'not a conversation'],
  'a legacy chat can be deleted by its derived id',
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
