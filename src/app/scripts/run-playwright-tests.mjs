#!/usr/bin/env node

import { spawnSync } from 'node:child_process';
import process from 'node:process';

const args = process.argv.slice(2);

// If user explicitly specifies a project, we forward all args directly to a single Playwright run
const hasProject = args.some(arg => arg.startsWith('--project') || arg === '-p');
const hasHelp = args.some(arg => arg === '--help' || arg === '-h' || arg === 'help');

const npxCommand = process.platform === 'win32' ? 'npx.cmd' : 'npx';

if (hasProject || hasHelp) {
  const result = spawnSync(npxCommand, ['playwright', 'test', ...args], {
    stdio: 'inherit',
    shell: false,
  });
  if (result.error) {
    console.error('Failed to start Playwright tests:', result.error.message || result.error);
    process.exit(1);
  }
  process.exit(result.status ?? 1);
}

console.log('Running accessibility (a11y) tests...');
const a11yResult = spawnSync(npxCommand, ['playwright', 'test', '--project=a11y', ...args], {
  stdio: 'inherit',
  shell: false,
});

if (a11yResult.error || a11yResult.status !== 0) {
  if (a11yResult.error) {
    console.error('Failed to start accessibility tests:', a11yResult.error.message || a11yResult.error);
  }
  process.exit(a11yResult.status ?? 1);
}

console.log('Running functional tests...');
const functionalResult = spawnSync(npxCommand, ['playwright', 'test', '--project=functional', ...args], {
  stdio: 'inherit',
  shell: false,
});

if (functionalResult.error || functionalResult.status !== 0) {
  if (functionalResult.error) {
    console.error('Failed to start functional tests:', functionalResult.error.message || functionalResult.error);
  }
  process.exit(functionalResult.status ?? 1);
}

process.exit(0);
