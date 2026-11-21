#!/usr/bin/env node
import { spawnSync } from 'node:child_process';
import { readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import process from 'node:process';

const colorErrors = !('NO_COLOR' in process.env) && (
  process.env.FORCE_COLOR === undefined ? Boolean(process.stderr.isTTY) : process.env.FORCE_COLOR !== '0'
);
const highlight = message => colorErrors ? `\x1b[1;31m${message}\x1b[0m` : message;

const testDirectory = new URL('../tests/', import.meta.url);
const files = readdirSync(testDirectory).filter(file => file.endsWith('.runtime.cjs')).sort();
if (files.length === 0) {
  console.error('No GUI runtime regression tests found.');
  process.exit(1);
}

const failures = [];
for (const file of files) {
  console.log(`\nRunning ${file}`);
  const result = spawnSync(process.execPath, [fileURLToPath(new URL(file, testDirectory))], {
    stdio: 'inherit',
  });
  if (result.error) console.error(result.error);
  if (result.error || result.status !== 0) {
    failures.push(file);
    console.error(highlight(`FAIL: ${file}`));
  }
}

console.log(`\nGUI runtime regressions: ${files.length - failures.length} passed, ${failures.length} failed.`);
if (failures.length > 0) {
  console.error(highlight([
    '',
    '============================================================',
    `GUI RUNTIME TESTS FAILED: ${failures.length}/${files.length}`,
    ...failures.map(file => `  FAIL: ${file}`),
    '============================================================',
  ].join('\n')));
  if (process.env.GITHUB_ACTIONS === 'true') {
    for (const file of failures) {
      console.error(`::error file=src/app/tests/${file},title=Runtime regression failed::${file}`);
    }
  }
  process.exitCode = 1;
}
