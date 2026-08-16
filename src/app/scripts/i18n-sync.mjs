import { writeRegistry } from './i18n-lib.mjs';

try {
  const { definitions, source } = writeRegistry();
  console.log(`[i18n] Registered ${definitions.length} locale(s); source=${source.manifest.locale}.`);
} catch (error) {
  console.error(`[i18n] ${error.message}`);
  process.exitCode = 1;
}
