# Vendored assets

## model-viewer.min.js

- **Source:** [`@google/model-viewer`](https://www.npmjs.com/package/@google/model-viewer) `dist/model-viewer.min.js`
- **Version:** 4.3.0
- **License:** Apache-2.0 (Google LLC); three.js (MIT) is bundled inside
- **Why vendored:** the Debian-native `lemonade-server` package must build using only
  npm modules available in Debian (`USE_SYSTEM_NODEJS_MODULES`), and Debian does not
  ship model-viewer. A single self-contained bundle works in both the Tauri app and
  the browser web-app without adding an npm dependency.
- **Update process:** download the new release's minified bundle and replace this file:

  ```bash
  curl -L https://cdn.jsdelivr.net/npm/@google/model-viewer@<version>/dist/model-viewer.min.js \
       -o src/app/src/renderer/vendor/model-viewer.min.js
  ```

  Then update the version in this file and verify the 3D panel preview still renders.
