const assert = require('node:assert/strict');
const fs = require('node:fs');
const ts = require('typescript');

require.extensions['.ts'] = (module, filename) => {
  module._compile(ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS, esModuleInterop: true },
    fileName: filename,
  }).outputText, filename);
};

const { visibleTreeRows, treeBreadcrumbs, nexusKeysEqual, readNexusTreeBranch } = require('../src/features/nexus/tree.ts');
const group = (id, parent_id = '') => ({ id, parent_id, name: id, kind: 'group' });
const root = group('organization');
const team = group('team', root.id);
const person = group('person', team.id);
const device = { id: 'laptop', parent_id: person.id, name: 'Laptop', kind: 'node' };
const branches = {
  [root.id]: { parent: root, children: [team] },
  [team.id]: { parent: team, children: [person] },
  [person.id]: { parent: person, children: [device] },
};
assert.deepEqual(visibleTreeRows(root, branches, new Set()).map(row => row.entry.id), [root.id]);
const expanded = new Set([root.id, team.id, person.id]);
assert.deepEqual(visibleTreeRows(root, branches, expanded).map(row => [row.entry.id, row.depth]), [
  [root.id, 1], [team.id, 2], [person.id, 3], [device.id, 4],
]);
assert.deepEqual(visibleTreeRows(root, branches, new Set([root.id])).map(row => row.entry.id), [root.id, team.id], 'collapsing a group hides every descendant');
const entries = new Map([root, team, person, device].map(entry => [entry.id, entry]));
assert.deepEqual(treeBreadcrumbs(device, entries).map(entry => entry.id), [root.id, team.id, person.id, device.id]);
const badBranches = { ...branches, [device.id]: { parent: device, children: [group('bad', device.id)] } };
assert.equal(visibleTreeRows(root, badBranches, new Set([...expanded, device.id])).length, 4, 'device nodes remain leaves');
const cycleRoot = group('cycle', 'cycle');
assert.equal(visibleTreeRows(cycleRoot, { cycle: { parent: cycleRoot, children: [cycleRoot] } }, new Set(['cycle'])).length, 1, 'cycles never hang rendering');
assert.equal(treeBreadcrumbs(cycleRoot, new Map([['cycle', cycleRoot]])).length, 1);
assert.equal(visibleTreeRows(root, { [root.id]: { parent: root, children: [group('outside', 'someone-else')] } }, new Set([root.id])).length, 1, 'misparented children are not rendered');

const deep = {};
const open = new Set();
for (let i = 0; i < 1000; i += 1) {
  const parent = group(String(i), i ? String(i - 1) : '');
  deep[parent.id] = { parent, children: i < 999 ? [group(String(i + 1), parent.id)] : [] };
  open.add(parent.id);
}
assert.equal(visibleTreeRows(deep['0'].parent, deep, open).at(-1).depth, 1000, 'rendering does not rely on JavaScript recursion depth');
assert.equal(nexusKeysEqual('ed25519:ab-_==', 'ab+/'), true);
assert.equal(nexusKeysEqual('', ''), false);
assert.equal(nexusKeysEqual('first', 'second'), false);
assert.throws(() => readNexusTreeBranch({ status: 'running' }), /invalid group data/, 'a different API response must fail before it reaches React state');
assert.throws(() => readNexusTreeBranch(null), /invalid group data/);
console.log('Nexus hierarchy: recursive groups, leaf nodes, collapse, breadcrumbs, cycle protection and identity matching passed.');
