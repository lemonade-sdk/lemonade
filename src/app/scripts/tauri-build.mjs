#!/usr/bin/env node
// Compute Tauri bundle metadata from the same dynamic version as lemond/CMake.
// Windows MSI requires a major version below 256; WiX uses year % 100 too.
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const appRoot = fileURLToPath(new URL('../', import.meta.url));
const repoRoot = fileURLToPath(new URL('../../../', import.meta.url));

function lemonadeVersion() {
  if (process.env.LEMONADE_VERSION?.trim()) return process.env.LEMONADE_VERSION.trim();
  const pythonCandidates = [
    [process.env.PYTHON, []],
    ['python3', []],
    ['python', []],
    ['py', ['-3']],
  ];
  for (const [python, prefix] of pythonCandidates) {
    if (!python) continue;
    const run = spawnSync(python, [...prefix, 'tools/version.py'], {
      cwd: repoRoot, encoding: 'utf8', windowsHide: true,
    });
    if (!run.error && run.status === 0 && run.stdout.trim()) return run.stdout.trim();
  }
  throw new Error('Cannot determine Lemonade version (set LEMONADE_VERSION or install Python).');
}

function bundleVersion(raw) {
  const match = /^(\d{4})\.(\d{1,2})\.(\d+)(?:~.+)?$/.exec(raw);
  if (!match) throw new Error(`Unexpected Lemonade version: ${raw}`);
  const [year, week, patch] = match.slice(1).map(Number);
  return `${year % 100}.${week}.${patch}`;
}

try {
  const version = bundleVersion(lemonadeVersion());
  const dryRun = process.argv.includes('--dry-run');
  const cliArgs = [
    'build', ...process.argv.slice(2).filter(arg => arg !== '--dry-run'),
    '--config', JSON.stringify({ version }),
  ];
  if (dryRun) {
    console.log(JSON.stringify({ version, args: cliArgs }));
  } else {
    console.log(`Lemonade Tauri bundle version: ${version}`);
    // Launch the JavaScript CLI entry point directly. Spawning tauri.cmd
    // without a shell fails on Windows, while a shell can corrupt JSON args.
    const cli = fileURLToPath(new URL('../node_modules/@tauri-apps/cli/tauri.js', import.meta.url));
    const child = spawnSync(process.execPath, [cli, ...cliArgs], {
      cwd: appRoot, stdio: 'inherit',
      env: { ...process.env, LEMONADE_TAURI_VERSION: version },
    });
    if (child.error) throw child.error;
    process.exitCode = child.status ?? 1;
  }
} catch (error) {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
}
