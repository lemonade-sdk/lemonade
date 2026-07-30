const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const modulePath = path.join(root, 'src/modelConfiguration.ts');
const values = new Map();

const localStorage = {
  getItem: key => values.has(key) ? values.get(key) : null,
  setItem: (key, value) => values.set(key, String(value)),
  removeItem: key => values.delete(key),
};
global.localStorage = localStorage;
global.window = { dispatchEvent() {} };
global.CustomEvent = class CustomEvent {
  constructor(type, init) {
    this.type = type;
    this.detail = init?.detail;
  }
};

function loadModelConfigurationModule() {
  const source = fs.readFileSync(modulePath, 'utf8')
    .replace("import { capabilityFromModelInfo, type ModelCapability } from './modelCapabilities';", 'const capabilityFromModelInfo = () => null;')
    .replace("import { storageKey } from './storage';", "const storageKey = (key: string) => `lemonade:${key}`;");
  const compiled = ts.transpileModule(source, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS,
    },
    fileName: modulePath,
    reportDiagnostics: true,
  });
  assert.equal(
    (compiled.diagnostics || []).length,
    0,
    (compiled.diagnostics || []).map(diagnostic => ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n')).join('\n'),
  );
  const moduleRecord = { exports: {} };
  Function('exports', 'require', 'module', '__filename', '__dirname', compiled.outputText)(
    moduleRecord.exports,
    require,
    moduleRecord,
    modulePath,
    path.dirname(modulePath),
  );
  return moduleRecord.exports;
}

const model = 'collision-model';
const directModel = 'direct-model';
const directValue = { source: 'direct', recipe_options: { ctx_size: 4096 } };
const defaultValue = { source: 'default', recipe_options: { ctx_size: 8192 } };
const sDefaultValue = { source: 's_default', recipe_options: { ctx_size: 16384 } };
const sDoubleDefaultValue = { source: 's__default', recipe_options: { ctx_size: 32768 } };
const sDefaultConflictKey = `model_tunings:${encodeURIComponent(model)}:${encodeURIComponent(`${model}@@S_DEFAULT`)}`;

values.set('lemonade:model_tunings', JSON.stringify({
  [`${model}@@s__default`]: sDoubleDefaultValue,
  [`${model}@@S_DEFAULT`]: sDefaultValue,
  [`${model}@@DeFaUlT`]: defaultValue,
  [directModel]: directValue,
  [`${directModel}@@DEFAULT`]: defaultValue,
  [`${directModel}@@s_default`]: sDefaultValue,
  [`${directModel}@@S__DEFAULT`]: sDoubleDefaultValue,
}));
values.set('lemonade:storage_migration_conflicts_v1', JSON.stringify({
  [sDefaultConflictKey]: { source: 'preexisting' },
}));

const settings = loadModelConfigurationModule();
settings.loadModelTunings();
const migrated = JSON.parse(values.get('lemonade:model_tunings'));
assert.deepEqual(migrated[model], defaultValue, 'the highest-priority legacy suffix must win deterministically');
assert.deepEqual(migrated[directModel], directValue, 'the direct model record must retain precedence');

const conflicts = JSON.parse(values.get('lemonade:storage_migration_conflicts_v1'));
assert.deepEqual(
  conflicts[sDefaultConflictKey],
  { source: 'preexisting' },
);
assert.deepEqual(
  conflicts[`${sDefaultConflictKey}:2`],
  sDefaultValue,
);
assert.deepEqual(
  conflicts[`model_tunings:${encodeURIComponent(model)}:${encodeURIComponent(`${model}@@s__default`)}`],
  sDoubleDefaultValue,
);
for (const suffix of ['DEFAULT', 's_default', 'S__DEFAULT']) {
  assert.deepEqual(
    conflicts[`model_tunings:${encodeURIComponent(directModel)}:${encodeURIComponent(`${directModel}@@${suffix}`)}`],
    suffix === 'DEFAULT' ? defaultValue : suffix === 's_default' ? sDefaultValue : sDoubleDefaultValue,
  );
}

for (const suffix of ['s__default', 'S_DEFAULT', 'DeFaUlT']) {
  assert.equal(values.get(`lemonade:${model}@@${suffix}`), undefined);
}
for (const suffix of ['DEFAULT', 's_default', 'S__DEFAULT']) {
  assert.equal(values.get(`lemonade:${directModel}@@${suffix}`), undefined);
}
assert.equal(values.get('lemonade:model_tunings_migrated_v3'), 'true');

const tuningSnapshot = values.get('lemonade:model_tunings');
const conflictSnapshot = values.get('lemonade:storage_migration_conflicts_v1');
settings.loadModelTunings();
assert.deepEqual(JSON.parse(values.get('lemonade:model_tunings'))[model], defaultValue);
assert.equal(values.get('lemonade:model_tunings'), tuningSnapshot);
assert.equal(values.get('lemonade:storage_migration_conflicts_v1'), conflictSnapshot);

console.log('Model tuning migration preserves suffix collisions, direct precedence, and rerun stability.');
