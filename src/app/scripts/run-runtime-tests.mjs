#!/usr/bin/env node
import { spawnSync } from 'node:child_process';
import { readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

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
  if (result.error || result.status !== 0) failures.push(file);
}

console.log(`\nGUI runtime regressions: ${files.length - failures.length} passed, ${failures.length} failed.`);
if (failures.length > 0) {
  console.error(`Failed: ${failures.join(', ')}`);
  process.exitCode = 1;
}
