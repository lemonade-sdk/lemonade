import fs from 'node:fs';
import {
  generateRegistry,
  generatedFile,
  loadLocaleDefinitions,
} from './i18n-lib.mjs';
import { checkUiI18nContracts } from './i18n-ui-check.mjs';

try {
  const { definitions, source, sourceNamespaces } = loadLocaleDefinitions();
  checkUiI18nContracts(source);
  const expected = generateRegistry(definitions, source);
  const actual = fs.existsSync(generatedFile) ? fs.readFileSync(generatedFile, 'utf8') : '';
  if (actual !== expected) {
    throw new Error('Generated locale registry is stale. Run npm run i18n:sync.');
  }

  // Guard the locale matching assumptions that make Simplified Chinese safe:
  // BCP-47 canonicalization must preserve the script subtag and Intl must expose it.
  const simplifiedChinese = new Intl.Locale('zh-Hans').maximize();
  if (simplifiedChinese.language !== 'zh' || simplifiedChinese.script !== 'Hans') {
    throw new Error('Runtime Intl implementation cannot distinguish zh-Hans safely.');
  }

  console.log(`[i18n] OK: ${definitions.length} locale(s), ${sourceNamespaces.length} namespace(s), source=${source.manifest.locale}, zh-Hans supported.`);
} catch (error) {
  console.error(`[i18n] ${error.message}`);
  process.exitCode = 1;
}
