#!/usr/bin/env node
import { spawnSync } from 'node:child_process';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const suites = [
  ['runtime regressions', 'run-runtime-tests.mjs', []],
  ['Playwright', 'run-playwright-tests.mjs', process.argv.slice(2)],
];
const failures = [];
for (const [name, script, args] of suites) {
  console.log(`\nRunning GUI ${name}...`);
  const result = spawnSync(process.execPath, [fileURLToPath(new URL(script, import.meta.url)), ...args], {
    stdio: 'inherit',
    env: { ...process.env, LEMONADE_REAL_SERVER: '0' },
  });
  if (result.error) console.error(result.error);
  if (result.error || result.status !== 0) failures.push(name);
}
if (failures.length > 0) {
  console.error(`\nGUI tests failed: ${failures.join(', ')}`);
  process.exitCode = 1;
}
