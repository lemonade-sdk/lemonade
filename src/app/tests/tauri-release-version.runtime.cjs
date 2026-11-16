const assert = require('node:assert/strict');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');

const root = path.resolve(__dirname, '..');
const manifest = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8'));
for (const name of ['build:tauri', 'build:nobundle', 'build:mac']) {
  assert.match(manifest.scripts[name], /scripts\/tauri-build\.mjs/);
}
function inspect(version, extra = []) {
  return spawnSync(process.execPath, [path.join(root, 'scripts/tauri-build.mjs'), ...extra, '--dry-run'], {
    cwd: root,
    env: { ...process.env, LEMONADE_VERSION: version },
    encoding: 'utf8',
  });
}
const release = inspect('2026.44.2', ['--bundles', 'app']);
assert.equal(release.status, 0, release.stderr);
const parsed = JSON.parse(release.stdout);
assert.equal(parsed.version, '26.44.2');
assert.deepEqual(parsed.args, ['build', '--bundles', 'app', '--config', '{"version":"26.44.2"}']);
const dev = inspect('2026.44.0~5.a1234');
assert.equal(dev.status, 0, dev.stderr);
assert.equal(JSON.parse(dev.stdout).version, '26.44.0');
const bad = inspect('not-a-release');
assert.notEqual(bad.status, 0, 'invalid version must fail instead of producing a mislabeled bundle');
const buildScript = fs.readFileSync(path.join(root, 'src-tauri/build.rs'), 'utf8');
const wrapper = fs.readFileSync(path.join(root, 'scripts/tauri-build.mjs'), 'utf8');
assert.match(buildScript, /cargo:rerun-if-env-changed=LEMONADE_TAURI_VERSION/);
assert.match(buildScript, /set_var\("CARGO_PKG_VERSION", version\)/);
assert.match(wrapper, /LEMONADE_TAURI_VERSION: version/);
assert.match(wrapper, /\['py', \['-3'\]\]/);

if (process.platform !== 'win32') {
  const os = require('node:os');
  const bin = fs.mkdtempSync(path.join(os.tmpdir(), 'lemonade-py-launcher-'));
  try {
    for (const exe of ['python3', 'python']) {
      fs.writeFileSync(path.join(bin, exe), '#!/bin/sh\nexit 1\n', { mode: 0o755 });
    }
    fs.writeFileSync(path.join(bin, 'py'),
      '#!/bin/sh\n[ "$1" = "-3" ] || exit 1\nprintf "2026.44.3\\n"\n',
      { mode: 0o755 });
    const pyOnly = spawnSync(process.execPath, [path.join(root, 'scripts/tauri-build.mjs'), '--dry-run'], {
      cwd: root, encoding: 'utf8',
      env: { ...process.env, PATH: bin, PYTHON: '', LEMONADE_VERSION: '' },
    });
    assert.equal(pyOnly.status, 0, pyOnly.stderr);
    assert.equal(JSON.parse(pyOnly.stdout).version, '26.44.3');
  } finally {
    fs.rmSync(bin, { recursive: true, force: true });
  }
}

console.log('Tauri release version regression checks passed.');
