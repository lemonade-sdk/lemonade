#!/usr/bin/env node
import { spawnSync } from 'node:child_process';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const [mode, ...args] = process.argv.slice(2);
if (mode !== 'smoke') {
  console.error('Usage: run-server-tests.mjs smoke [Playwright options]');
  process.exit(1);
}
const pattern = '@real-server';
const output = 'test-results-real-server';
const result = spawnSync(process.execPath, [
  fileURLToPath(new URL('run-playwright-tests.mjs', import.meta.url)),
  '--project=functional', '--grep', pattern, `--output=${output}`, ...args,
], {
  stdio: 'inherit',
  env: { ...process.env, LEMONADE_REAL_SERVER: '1' },
});
if (result.error) console.error(result.error);
process.exitCode = result.error ? 1 : (result.status ?? 1);
