import fs from 'node:fs';
import path from 'node:path';
import {
  canonicalizeLocale,
  directionForLocale,
  loadLocaleDefinitions,
  localesRoot,
  writeRegistry,
} from './i18n-lib.mjs';

const requested = process.argv[2];
if (!requested) {
  console.error('Usage: npm run i18n:add -- <bcp47-locale>');
  process.exit(1);
}

try {
  const canonical = canonicalizeLocale(requested);
  const targetDir = path.join(localesRoot, canonical);
  if (fs.existsSync(targetDir)) throw new Error(`Locale ${canonical} already exists.`);

  const { source, sourceNamespaces } = loadLocaleDefinitions();
  fs.mkdirSync(targetDir, { recursive: false });
  fs.writeFileSync(path.join(targetDir, 'manifest.json'), `${JSON.stringify({
    locale: canonical,
    source: false,
    direction: directionForLocale(canonical),
  }, null, 2)}\n`);

  for (const namespace of sourceNamespaces) {
    fs.writeFileSync(path.join(targetDir, `${namespace}.json`), '{}\n');
  }

  writeRegistry();
  console.log(`[i18n] Added ${canonical}. Empty translations fall back to ${source.manifest.locale}.`);
} catch (error) {
  console.error(`[i18n] ${error.message}`);
  process.exitCode = 1;
}
