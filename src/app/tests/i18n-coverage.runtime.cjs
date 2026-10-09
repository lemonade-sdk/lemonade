const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const srcRoot = path.join(root, 'src');
const i18nSource = fs.readFileSync(path.join(srcRoot, 'i18n.tsx'), 'utf8');

const SQ = "'((?:[^'\\\\]|\\\\.)*)'";
const DQ = '"((?:[^"\\\\]|\\\\.)*)"';

function unescape(literal) {
  return literal.replace(/\\(['"\\])/g, '$1');
}

function dictionaryBody(source) {
  const start = source.indexOf('const ZH_CN');
  assert.notEqual(start, -1, 'i18n.tsx must declare ZH_CN');
  const open = source.indexOf('{', start);
  let depth = 0;
  for (let i = open; i < source.length; i += 1) {
    if (source[i] === '{') depth += 1;
    else if (source[i] === '}') {
      depth -= 1;
      if (depth === 0) return source.slice(open + 1, i);
    }
  }
  throw new Error('ZH_CN object literal is not balanced');
}

// A repeated key in an object literal is legal JavaScript: the last one silently
// wins, so a shadowed translation would never surface at runtime or at build time.
const entryPattern = new RegExp(`(?:^|\\n)\\s*(?:${SQ}|${DQ})\\s*:\\s*(?:${SQ}|${DQ})\\s*,`, 'g');
const keys = [];
for (const match of dictionaryBody(i18nSource).matchAll(entryPattern)) {
  keys.push(unescape(match[1] !== undefined ? match[1] : match[2]));
}

assert.ok(keys.length > 0, 'ZH_CN must not be empty');

const seen = new Set();
const duplicates = [];
for (const key of keys) {
  if (seen.has(key)) duplicates.push(key);
  seen.add(key);
}
assert.deepEqual(duplicates, [], `ZH_CN has duplicate keys: ${duplicates.join(', ')}`);

function walk(dir, out = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      if (entry.name !== 'node_modules') walk(full, out);
    } else if (/\.tsx?$/.test(entry.name) && entry.name !== 'i18n.tsx') {
      out.push(full);
    }
  }
  return out;
}

// Only literal keys can be checked statically. Keys reached through a variable
// -- t(section.label) and friends -- are invisible here by construction.
const callPattern = new RegExp(`\\b(?:t|translate|translateText)\\(\\s*(?:${SQ}|${DQ})`, 'g');
const untranslated = [];
for (const file of walk(srcRoot)) {
  const source = fs.readFileSync(file, 'utf8');
  for (const match of source.matchAll(callPattern)) {
    const key = unescape(match[1] !== undefined ? match[1] : match[2]);
    if (!seen.has(key)) {
      untranslated.push(`${path.relative(root, file).replace(/\\/g, '/')}: ${JSON.stringify(key)}`);
    }
  }
}

assert.deepEqual(
  untranslated,
  [],
  `t() called with keys that have no zh-CN translation:\n  ${untranslated.join('\n  ')}`,
);

// Two shapes of JSX text escape a naive one-line `>text<` scan, and both shipped
// English in the original pass: a text node on its own line between an opening
// and a closing tag, and prose trailing a self-closing tag such as <Icon />.
const JSX_FILES = [];
(function collectTsx(dir) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      if (entry.name !== 'node_modules') collectTsx(full);
    } else if (entry.name.endsWith('.tsx') && entry.name !== 'i18n.tsx') {
      JSX_FILES.push(full);
    }
  }
})(srcRoot);

const PROSE = /^[A-Z][A-Za-z0-9][A-Za-z0-9 ,.'’\-&/()]{3,}[.a-zA-Z)]$/;
const bareProse = [];
for (const file of JSX_FILES) {
  const lines = fs.readFileSync(file, 'utf8').split('\n');
  const rel = path.relative(srcRoot, file).replace(/\\/g, '/');
  lines.forEach((line, i) => {
    const text = line.trim();
    // Standalone text node: own line, previous line opens an element, next closes one.
    if (PROSE.test(text) && !/[<>{}]/.test(text)) {
      const prev = [...lines.slice(0, i)].reverse().find((l) => l.trim());
      const next = lines.slice(i + 1).find((l) => l.trim());
      if (prev && /(>|\/>)$/.test(prev.trim()) && next && next.trim().startsWith('</')) {
        bareProse.push(`${rel}:${i + 1}: ${text}`);
      }
    }
    // Prose trailing a self-closing tag on the same line.
    const trailing = line.match(/\/>\s+([A-Z][A-Za-z0-9 ,.'’\-&/()]{5,})$/);
    if (trailing) bareProse.push(`${rel}:${i + 1}: ${trailing[1].trim()}`);
  });
}

assert.deepEqual(
  bareProse,
  [],
  `JSX text rendered without t():\n  ${bareProse.join('\n  ')}`,
);

// Label tables live at module scope and are translated at render via t(x.label).
// A literal scan cannot see them, so their values are checked explicitly: a new
// row here ships English to zh-CN users without tripping anything else.
const DYNAMIC_LABEL_TABLES = {
  'components/ModelNavRail.tsx': { const: 'TASK_ITEMS', field: 'label' },
  'components/ModelDetailPanel.tsx': { const: 'TABS', field: 'label' },
  'components/GlobalModelSettingsPanel.tsx': { const: 'READ_MODES', field: 'title' },
};

// Brands and model families read the same in every locale.
const UNTRANSLATED_BY_DESIGN = new Set(['Hugging Face', 'ModelScope', 'Lemonade']);

for (const [rel, spec] of Object.entries(DYNAMIC_LABEL_TABLES)) {
  const source = fs.readFileSync(path.join(srcRoot, rel), 'utf8');
  const start = source.indexOf(`const ${spec.const}`);
  assert.notEqual(start, -1, `${rel} must declare ${spec.const}`);
  const block = source.slice(start, source.indexOf('\n];', start));
  const values = [...block.matchAll(new RegExp(`\\b${spec.field}:\\s*(?:${SQ}|${DQ})`, 'g'))]
    .map((m) => unescape(m[1] !== undefined ? m[1] : m[2]))
    .filter((v) => !UNTRANSLATED_BY_DESIGN.has(v));
  assert.ok(values.length > 0, `${rel}: found no ${spec.field} values in ${spec.const}`);
  const gaps = values.filter((v) => !seen.has(v));
  assert.deepEqual(gaps, [], `${rel} ${spec.const}.${spec.field} values missing from ZH_CN: ${gaps.join(', ')}`);
}

// The locale must stay per-client. Sending it to lemond would break the
// many-clients-one-server topology.
assert.match(i18nSource, /localStorage\.getItem\(LOCALE_STORAGE_KEY\)/);
assert.doesNotMatch(i18nSource, /api\.|fetch\(/);

// The critical CSS is inlined before the main stylesheet loads, so its font
// stack must carry the CJK families too or zh-CN text swaps font on first paint.
const tokensCss = fs.readFileSync(path.join(srcRoot, 'styles/tokens.css'), 'utf8');
const criticalCss = fs.readFileSync(path.join(srcRoot, 'styles/critical.generated.css'), 'utf8');
for (const family of ['"Microsoft YaHei"', '"Noto Sans CJK SC"']) {
  assert.ok(tokensCss.includes(family), `tokens.css --font-sans must list ${family}`);
  assert.ok(criticalCss.includes(family), `critical.generated.css --font-sans must list ${family}`);
}

console.log(`i18n coverage OK: ${keys.length} zh-CN entries, ${seen.size} unique, 0 untranslated t() literals`);
