# Vendored assets

## `model-viewer.min.js`

- **Source:** `@google/model-viewer` package, `dist/model-viewer.min.js`
- **Version:** 4.3.0
- **License:** BSD-3-Clause (Google LLC); bundled dependencies also retain their own notices, including MIT-licensed three.js code
- **Why vendored:** Lemonade's Debian-native build must work with the system Node.js module set, which does not provide `model-viewer`. Keeping the self-contained browser bundle avoids a runtime CDN dependency and matches the existing GUI2 integration.
- **Build behavior:** the proven GUI2 import path is kept inside GUI3's lazy 3D result chunk. Webpack resolves the vendor module together with the 3D UI, so normal startup does not load it and no runtime asset URL has to be guessed.
- **Update procedure:** replace this file with the matching `dist/model-viewer.min.js` from a reviewed release, update the version above, then verify GLB preview loading in both `/` and `/web-app/` deployments.
