# Vendored assets

## `model-viewer.min.js`

- **Source:** `@google/model-viewer` package, `dist/model-viewer.min.js`
- **Version:** 4.3.1
- **License:** Apache-2.0 (`@google/model-viewer`, Google LLC). The bundle
  also contains BSD-3-Clause code from Lit and MIT-licensed code from three.js,
  `@monogrid/gainmap-js`, and fflate 0.8.2 (Copyright (c) 2023 Arjun Barrett);
  see the root `LICENSE` and `contrib/debian/copyright`.
- **Why vendored:** Lemonade's Debian-native build must work with the system Node.js module set, which does not provide `model-viewer`. Keeping the self-contained browser bundle avoids a runtime CDN dependency and matches the existing desktop-app integration.
- **Build behavior:** the established model-viewer import path is kept inside the app's lazy 3D result chunk. Webpack resolves the vendor module together with the 3D UI, so normal startup does not load it and no runtime asset URL has to be guessed.
- **Update procedure:** replace this file with the matching `dist/model-viewer.min.js` from a reviewed release, update the version above, then verify GLB preview loading in both `/` and `/web-app/` deployments.
