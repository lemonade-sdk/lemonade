/*
 * The configuration form decomposes lemond's resolved `*_args` string into typed
 * sampler fields and shows the rest verbatim, then recomposes both halves into
 * the string it sends on load. These checks pin that round trip against the two
 * sources of truth it has to agree with: the shipped per-architecture defaults,
 * and the C++ tokeniser lemond parses the result with.
 */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const repoRoot = path.resolve(root, '../..');

function loadTypeScriptModule(filename) {
  const source = fs.readFileSync(filename, 'utf8');
  const compiled = ts.transpileModule(source, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS,
      esModuleInterop: true,
    },
    fileName: filename,
    reportDiagnostics: true,
  });
  const diagnostics = compiled.diagnostics || [];
  assert.equal(
    diagnostics.length,
    0,
    diagnostics.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'),
  );
  const module = { exports: {} };
  Function('exports', 'require', 'module', '__filename', '__dirname', compiled.outputText)(
    module.exports,
    require,
    module,
    filename,
    path.dirname(filename),
  );
  return module.exports;
}

const {
  SAMPLER_ARG_FIELDS,
  parseCustomArgs,
  buildArgsMap,
  argsMapToString,
  splitSamplerArgs,
  composeSamplerArgs,
} = loadTypeScriptModule(path.join(root, 'src/samplerArgs.ts'));

const canonical = str => argsMapToString(buildArgsMap(parseCustomArgs(str)));
const roundTrip = str => {
  const split = splitSamplerArgs(str);
  return composeSamplerArgs(split.fields, split.rest);
};

// ── The tokeniser must agree with the C++ one ────────────────────────────────

// keep_quotes wraps the whole accumulated token, so a value quoted from its
// first character comes back with its quotes intact.
assert.deepEqual(
  parseCustomArgs('--chat-template-kwargs \'{"preserve_thinking":true}\''),
  ['--chat-template-kwargs', '\'{"preserve_thinking":true}\''],
);

// A complete negative number is a value; a flag-looking token is a flag.
assert.deepEqual(buildArgsMap(parseCustomArgs('--presence-penalty -1.5')).get('--presence-penalty'), [['-1.5']]);
assert.deepEqual(buildArgsMap(parseCustomArgs('--temp -1e-3')).get('--temp'), [['-1e-3']]);
assert.ok(buildArgsMap(parseCustomArgs('--no-mmap')).has('--no-mmap'), 'a valueless flag is still a flag');
assert.deepEqual(buildArgsMap(parseCustomArgs('--no-mmap')).get('--no-mmap'), [[]]);

// ── Every shipped per-architecture default survives the round trip ───────────

const archDefaults = JSON.parse(
  fs.readFileSync(path.join(repoRoot, 'src/cpp/resources/architecture_defaults.json'), 'utf8'),
);
const architectures = Object.keys(archDefaults).filter(key => !key.startsWith('_'));
assert.ok(architectures.length > 0, 'architecture_defaults.json must define architectures');

for (const architecture of architectures) {
  const args = archDefaults[architecture].llamacpp_args;
  if (!args) continue;
  assert.equal(
    canonical(roundTrip(args)),
    canonical(args),
    `${architecture}: decomposing and recomposing must not change the arguments`,
  );
  // The whole string is sampler flags, so nothing should be left over.
  assert.equal(splitSamplerArgs(args).rest, '', `${architecture}: every flag must map to a typed field`);
}

// Every flag the shipped defaults use has a typed field, so no architecture
// falls back to the freeform box for a value the form claims to cover.
const specced = new Set(SAMPLER_ARG_FIELDS.map(field => field.flag));
for (const architecture of architectures) {
  for (const flag of String(archDefaults[architecture].llamacpp_args || '').match(/--[a-z0-9-]+/g) || []) {
    assert.ok(specced.has(flag), `${flag} (${architecture}) needs an entry in SAMPLER_ARG_FIELDS`);
  }
}

// Every field is typed well enough to render a stepper, and its id matches the
// flag it stands for so the two cannot drift apart.
for (const field of SAMPLER_ARG_FIELDS) {
  assert.ok(field.label, `${field.flag} needs a label`);
  assert.equal(field.id, field.flag.replace(/^-+/, ''), `${field.flag} id must follow its flag`);
  if (field.kind === 'text') continue;
  assert.ok(field.backendDefault, `${field.flag} needs the value llama.cpp would have used`);
  assert.equal(typeof field.min, 'number', `${field.flag} needs a minimum`);
  assert.equal(typeof field.max, 'number', `${field.flag} needs a maximum`);
  assert.ok(field.step > 0, `${field.flag} needs a positive step`);
  assert.ok(field.min < field.max, `${field.flag} range must be non-empty`);
}

// ── Unknown flags stay whole, and ownership is never shared ──────────────────

const mixed = splitSamplerArgs('--temp 0.6 --no-mmap --gpu-layers 35 --top-p 0.9');
assert.equal(mixed.fields['--temp'], '0.6');
assert.equal(mixed.fields['--top-p'], '0.9');
assert.equal(mixed.rest, '--no-mmap --gpu-layers 35', 'the remainder keeps the order it was typed in');
assert.equal(canonical(composeSamplerArgs(mixed.fields, mixed.rest)),
  canonical('--temp 0.6 --no-mmap --gpu-layers 35 --top-p 0.9'));

// A sampler flag the fields cannot represent stays in the remainder, and is
// reported so the form can stop claiming to own it.
const repeated = splitSamplerArgs('--temp 0.6 --temp 0.7');
assert.equal(repeated.fields['--temp'], undefined);
assert.ok(repeated.claimedByRest.has('--temp'));
assert.equal(repeated.rest, '--temp 0.6 --temp 0.7');
const multiValue = splitSamplerArgs('--chat-template-kwargs a b');
assert.equal(multiValue.fields['--chat-template-kwargs'], undefined);
assert.ok(multiValue.claimedByRest.has('--chat-template-kwargs'));

// Order is preserved through a full round trip, so the freeform field does not
// rearrange itself between keystrokes.
const ordered = '--zeta 1 --alpha 2 --no-mmap';
assert.equal(splitSamplerArgs(ordered).rest, ordered);
assert.equal(splitSamplerArgs(composeSamplerArgs({ '--temp': '0.6' }, ordered)).rest, ordered);

// Only one half may emit a given flag.
for (const source of ['--temp 0.6 --temp 0.7', '--temp 0.6 --gpu-layers 35']) {
  const split = splitSamplerArgs(source);
  for (const flag of Object.keys(split.fields)) {
    assert.ok(!split.claimedByRest.has(flag), `${flag} must not be owned by both halves`);
    assert.ok(!buildArgsMap(parseCustomArgs(split.rest)).has(flag),
      `${flag} is a field, so it must not also appear in the remainder`);
  }
}

// ── An empty field is an omitted flag, not an empty one ──────────────────────

assert.equal(composeSamplerArgs({ '--temp': '', '--top-p': '0.9' }, ''), '--top-p 0.9');
assert.equal(composeSamplerArgs({}, ''), '');
assert.equal(splitSamplerArgs('').rest, '');
assert.deepEqual(splitSamplerArgs('').fields, {});

// A number input only ever hands over a complete value or an empty string, and
// every complete value has to survive the trip back out of the composed string.
for (const value of ['0', '0.0', '0.6', '1.25', '-1.5', '-0.5', '200']) {
  const composed = composeSamplerArgs({ '--presence-penalty': value }, '');
  assert.equal(splitSamplerArgs(composed).fields['--presence-penalty'], value,
    `a field holding "${value}" must round-trip unchanged`);
}

// A lone "-" would tokenise as a flag and lose the value, which is why the
// negative-range field has to be a number input: it reports a half-typed sign
// as an empty value and never puts one in the draft.
assert.equal(splitSamplerArgs(composeSamplerArgs({ '--presence-penalty': '-' }, '')).fields['--presence-penalty'],
  undefined);

console.log('Sampler argument decomposition checks passed.');
