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
console.log('Tauri release version regression checks passed.');
