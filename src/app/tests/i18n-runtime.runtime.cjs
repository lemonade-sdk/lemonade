const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const corePath = path.join(root, 'src/i18n/core.tsx');
const modelDetailPath = path.join(root, 'src/components/ModelDetailPanel.tsx');
const chatViewPath = path.join(root, 'src/components/ChatView.tsx');
const copyHookPath = path.join(root, 'src/hooks/useCopyToClipboard.ts');
const inspectViewPath = path.join(root, 'src/components/InspectView.tsx');
const i18nLibPath = path.join(root, 'scripts/i18n-lib.mjs');

const i18nLibSource = fs.readFileSync(i18nLibPath, 'utf8');
assert.doesNotMatch(i18nLibSource, /import\.meta\.dirname/,
  'i18n tooling must remain compatible with Node 20 releases before import.meta.dirname existed');
assert.match(i18nLibSource, /fileURLToPath\(import\.meta\.url\)/,
  'i18n tooling must derive its ESM directory through fileURLToPath(import.meta.url)');

const modelDetailSource = fs.readFileSync(modelDetailPath, 'utf8');
assert.match(modelDetailSource, /noticeKey === 'detail\.config\.saved'/,
  'model configuration notices must compare semantic keys, not translated presentation strings');
assert.doesNotMatch(modelDetailSource, /notice\s*===\s*t\(/,
  'translated strings must never be used as model-configuration state identifiers');

const chatViewSource = fs.readFileSync(chatViewPath, 'utf8');
assert.match(chatViewSource, /contentKind\?: 'live-microphone'/,
  'live microphone history must persist a semantic message kind');
assert.match(chatViewSource, /titleKind: 'live-microphone'/,
  'live microphone conversation titles must be rendered from semantic state');
assert.match(chatViewSource, /toolResultText\(tc, t\)/,
  'persisted built-in tool results must be rendered using the current locale');

const copyHookSource = fs.readFileSync(copyHookPath, 'utf8');
const inspectViewSource = fs.readFileSync(inspectViewPath, 'utf8');
assert.match(copyHookSource, /successToast\?: boolean/,
  'copy callers must be able to suppress the generic success toast');
assert.match(inspectViewSource, /copyToClipboard\(dataStr, 'session', \{ successToast: false \}\)/,
  'session export must not emit a generic copy toast before its specific export toast');

const testLocales = [
  {
    manifest: { locale: 'en-US', source: true, direction: 'ltr' },
    resources: { common: {
      files: { one: '{{count}} file', other: '{{count}} files' },
      greeting: 'Hello {{name}}',
      amount: 'Total {{count}}',
      layout: { one: 'First layout', label: 'Layout label' },
    } },
  },
  {
    manifest: { locale: 'de-DE', source: false, direction: 'ltr', complete: true },
    resources: { common: {
      files: { one: '{{count}} Datei', other: '{{count}} Dateien' },
      greeting: 'Hallo {{name}}',
      amount: 'Summe {{count}}',
      layout: { one: 'Erstes Layout', label: 'Layout-Bezeichnung' },
    } },
  },
  {
    manifest: { locale: 'fr-FR', source: false, direction: 'ltr' },
    resources: { common: { greeting: 'Bonjour {{name}}' } },
  },
  { manifest: { locale: 'zh-Hans', source: false, direction: 'ltr' }, resources: {} },
  { manifest: { locale: 'zh-Hant', source: false, direction: 'ltr' }, resources: {} },
];

function loadCore() {
  let source = fs.readFileSync(corePath, 'utf8');
  source = source
    .replace(/import React, \{[\s\S]*?\} from 'react';/, `
const React = { createElement: () => null };
const createContext = () => ({ Provider: () => null });
const useCallback = fn => fn;
const useContext = () => null;
const useEffect = () => {};
const useMemo = fn => fn();
const useState = initial => [typeof initial === 'function' ? initial() : initial, () => {}];
type ReactNode = unknown;`)
    .replace("import { storageKey } from '../storage';", "const storageKey = (key: string) => `lemonade:${key}`;")
    .replace(/import \{\s*GENERATED_LOCALES,\s*GENERATED_SOURCE_LOCALE,\s*\} from '\.\/generated\/locales\.generated';/, `const GENERATED_LOCALES = ${JSON.stringify(testLocales)};\nconst GENERATED_SOURCE_LOCALE = 'en-US';`)
    .replace(/import type \{[\s\S]*?\} from '\.\/types';/, `
type GeneratedLocaleDefinition = any;
type LocaleDirection = 'ltr' | 'rtl';
type LocaleManifest = any;
type TranslationParams = Record<string, string | number | boolean | null | undefined>;`);

  const compiled = ts.transpileModule(source, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS,
      jsx: ts.JsxEmit.React,
      esModuleInterop: true,
    },
    fileName: corePath,
    reportDiagnostics: true,
  });
  assert.equal(
    (compiled.diagnostics || []).length,
    0,
    (compiled.diagnostics || []).map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'),
  );
  const module = { exports: {} };
  Function('exports', 'require', 'module', '__filename', '__dirname', compiled.outputText)(
    module.exports, require, module, corePath, path.dirname(corePath),
  );
  return module.exports;
}

(async () => {
  const core = loadCore();

  assert.equal(core.resolveSupportedLocale(['de-AT', 'en-US']), 'de-DE', 'first compatible requested locale must win');
  assert.equal(core.resolveSupportedLocale(['es-MX', 'en-US']), 'en-US', 'unsupported first choice must fall through to the next requested locale');
  assert.equal(core.resolveSupportedLocale(['es-MX']), 'en-US', 'unsupported locales must fall back to source');
  assert.equal(core.resolveSupportedLocale(['zh-CN']), 'zh-Hans', 'Simplified Chinese must map by script');
  assert.equal(core.resolveSupportedLocale(['zh-TW']), 'zh-Hant', 'Traditional Chinese must not map to Simplified Chinese');

  assert.equal(core.translateForLocale('de-DE', 'common', 'files', { count: 1 }), '1 Datei');
  assert.equal(core.translateForLocale('de-DE', 'common', 'files', { count: 2 }), '2 Dateien');
  assert.equal(core.translateForLocale('de-DE', 'common', 'amount', { count: 1234 }), 'Summe 1.234');
  assert.equal(core.translateForLocale('fr-FR', 'common', 'files', { count: 2 }), '2 files', 'partial locale must use source fallback');
  assert.equal(core.translateOptionalForLocale('en-US', 'common', 'layout', { count: 1 }), null,
    'an ordinary object containing a child named one must not be treated as a plural leaf');
  assert.equal(core.translateForLocale('en-US', 'common', 'layout.one'), 'First layout');

  const { loadLocaleDefinitions } = await import(pathToFileURL(i18nLibPath).href);
  const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'lemonade-i18n-'));
  const writeLocale = (locale, manifest, common) => {
    const dir = path.join(tempRoot, locale);
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, 'manifest.json'), JSON.stringify({ locale, direction: 'ltr', ...manifest }, null, 2));
    fs.writeFileSync(path.join(dir, 'common.json'), JSON.stringify(common, null, 2));
  };
  const sourceCommon = {
    greeting: 'Hello {{name}}',
    files: { one: '{{count}} file', other: '{{count}} files' },
    layout: { one: 'First layout', label: 'Layout label' },
    optionalOverride: null,
  };
  writeLocale('en-US', { source: true }, sourceCommon);
  writeLocale('fr-FR', { source: false, complete: false }, { greeting: 'Bonjour {{name}}' });
  assert.doesNotThrow(() => loadLocaleDefinitions(tempRoot), 'partial locales may omit source keys');

  writeLocale('fr-FR', { source: false, complete: false }, {
    greeting: 'Bonjour {{name}}',
    optionalOverride: 'Libellé localisé',
  });
  assert.doesNotThrow(() => loadLocaleDefinitions(tempRoot),
    'source null keys must accept concrete localized string overrides');

  writeLocale('fr-FR', { source: false, complete: false }, {
    greeting: 'Bonjour {{name}}',
    optionalOverride: 'Mauvais {{name}}',
  });
  assert.throws(() => loadLocaleDefinitions(tempRoot), /must not introduce interpolation placeholders/,
    'optional dynamic-content overrides must not introduce unusable interpolation placeholders');

  writeLocale('fr-FR', { source: false, complete: false }, {
    greeting: 'Bonjour {{name}}',
    files: '{{count}} fichiers',
  });
  assert.throws(() => loadLocaleDefinitions(tempRoot), /must define plural forms/,
    'a provided partial-locale key must keep the source plural shape');

  writeLocale('fr-FR', { source: false, complete: false }, { greeting: 'Bonjour {{person}}' });
  assert.throws(() => loadLocaleDefinitions(tempRoot), /interpolation mismatch/,
    'partial locales must validate placeholders for every provided key');

  fs.rmSync(tempRoot, { recursive: true, force: true });
  console.log('i18n runtime and partial-locale contract checks passed.');
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});

function pathToFileURL(file) {
  const { pathToFileURL } = require('node:url');
  return pathToFileURL(file);
}
