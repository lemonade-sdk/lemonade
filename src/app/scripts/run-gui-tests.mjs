#!/usr/bin/env node
import { spawnSync } from 'node:child_process';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

// Preserve readable errors when npm runs both suites in the same terminal.
const colorErrors = !('NO_COLOR' in process.env) && (
  process.env.FORCE_COLOR === undefined ? Boolean(process.stderr.isTTY) : process.env.FORCE_COLOR !== '0'
);
const highlight = message => colorErrors ? `\x1b[1;31m${message}\x1b[0m` : message;

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
  if (result.error || result.status !== 0) {
    failures.push(name);
    console.error(highlight(`\nFAIL: GUI ${name}`));
  }
}
if (failures.length > 0) {
  console.error(highlight([
    '',
    '============================================================',
    'GUI TESTS FAILED',
    ...failures.map(name => `  FAIL: ${name}`),
    '============================================================',
  ].join('\n')));
  if (process.env.GITHUB_ACTIONS === 'true') {
    console.error(`::error title=GUI tests failed::${failures.join(', ')}`);
  }
  process.exitCode = 1;
}
