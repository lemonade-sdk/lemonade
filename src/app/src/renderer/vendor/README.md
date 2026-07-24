# Vendored assets

## model-viewer.min.js

- **Source:** [`@google/model-viewer`](https://www.npmjs.com/package/@google/model-viewer) `dist/model-viewer.min.js`
- **Version:** 4.3.0
- **License:** Apache-2.0 (Google LLC); three.js (MIT) is bundled inside
- **Why vendored:** the Debian-native `lemonade-server` package must build using only
  npm modules available in Debian (`USE_SYSTEM_NODEJS_MODULES`), and Debian does not
  ship model-viewer. A single self-contained bundle works in both the Tauri app and
  the browser web-app without adding an npm dependency.

### Integrity

The bundle is the unmodified upstream artifact. Its hash is pinned in
`model-viewer.min.js.sha256` and verified in CI (see `docs_and_style.yml`), so any
drift — an accidental edit, a corrupted download, or a tampered blob — fails the build.
This is the in-repo equivalent of a Subresource Integrity (SRI) hash, which a webpack
`import` cannot carry.

- **SHA-256 (this file):** `ba1a6859cc03167e9f7850e67c1a60a3a03b1c48546c75bfebca4dbf6ee63dad`
- **npm tarball integrity (`4.3.0`):** `sha512-NaJeVwzAZjGJMnAnOkVV8+vHQhzqZuRwwbFnzUNqKKajtLDlZyxPMx/cC8S/82n+3slBotbGe0zowj3EYXIW8A==`

The npm tarball integrity is the authoritative upstream anchor. The SHA-256 pins the
exact `dist/model-viewer.min.js` extracted from that tarball. Both are reproducible from
the [npm registry metadata](https://registry.npmjs.org/@google/model-viewer/4.3.0).

### Update process

Do not edit the bundle by hand. To move to a new version, replace it with the pristine
upstream artifact and re-pin the hash:

```bash
VERSION=<new-version>
# Fetch the exact bundle npm publishes (registry tarball is the source of truth).
curl -sL "https://registry.npmjs.org/@google/model-viewer/-/model-viewer-${VERSION}.tgz" \
  | tar xz -C /tmp
cp "/tmp/package/dist/model-viewer.min.js" src/app/src/renderer/vendor/model-viewer.min.js

# Re-pin the hash (must run from the vendor dir so the path in the file is relative).
cd src/app/src/renderer/vendor
sha256sum model-viewer.min.js > model-viewer.min.js.sha256
```

Then update **Version** and the two hashes above, and verify the 3D panel preview still
renders in both the Tauri app and the web-app.
