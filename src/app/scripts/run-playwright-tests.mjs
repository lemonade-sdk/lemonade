#!/usr/bin/env node

import { spawnSync } from 'node:child_process';
import process from 'node:process';

const args = process.argv.slice(2);

// If user explicitly specifies a project, we forward all args directly to a single Playwright run
const hasProject = args.some(arg => arg.startsWith('--project') || arg === '-p');
const hasHelp = args.some(arg => arg === '--help' || arg === '-h' || arg === 'help');

if (hasProject || hasHelp) {
  const result = spawnSync('npx', ['playwright', 'test', ...args], {
    stdio: 'inherit',
    shell: true,
  });
  process.exit(result.status ?? 0);
}

console.log('Running accessibility (a11y) tests...');
const a11yResult = spawnSync('npx', ['playwright', 'test', '--project=a11y', '--pass-with-no-tests', ...args], {
  stdio: 'inherit',
  shell: true,
});

if (a11yResult.status !== 0) {
  process.exit(a11yResult.status ?? 1);
}

console.log('Running functional tests...');
const functionalResult = spawnSync('npx', ['playwright', 'test', '--project=functional', '--pass-with-no-tests', ...args], {
  stdio: 'inherit',
  shell: true,
});

process.exit(functionalResult.status ?? 0);
