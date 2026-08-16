# Vendored assets

## model-viewer.min.js

- **Source:** [`@google/model-viewer`](https://www.npmjs.com/package/@google/model-viewer) `dist/model-viewer.min.js`
- **Version:** 4.3.1
- **License:** Apache-2.0 (Google LLC); three.js (MIT) is bundled inside
- **Why vendored:** the Debian-native `lemonade-server` package must build using only
  npm modules available in Debian (`USE_SYSTEM_NODEJS_MODULES`), and Debian does not
  ship model-viewer. A single self-contained bundle works in both the Tauri app and
  the browser web-app without adding an npm dependency.

### Integrity

The bundle is the unmodified upstream artifact. Its SHA-256 is pinned in
`model-viewer.min.js.sha256` and verified in CI (see `docs_and_style.yml`) as a
drift guard — the file and checksum can be changed together in the same commit,
so this is not a true SRI replacement. The npm `dist.integrity` (SHA-512) is the
authoritative trust anchor; the update process below enforces it.

- **SHA-256 (this file):** `283b0672384614b4847636c306fc93fe4b1fcadc76d668b4e47f0ca76bcf033b`
- **npm tarball integrity (`4.3.1`):** `sha512-GP+inXhAtY31E8rILVmByA6z8CZZjdlNajddppyI1/j1eIaSQiZcMRaUqTFe7+jv4mzRzwKIOiKBud0apiv+WQ==`

The npm `dist.integrity` is the upstream trust anchor. The SHA-256 pins the exact
`dist/model-viewer.min.js` extracted from that verified tarball. Both are reproducible
from the [npm registry metadata](https://registry.npmjs.org/@google/model-viewer/4.3.1).

### Update process

Do not edit the bundle by hand. To move to a new version, verify the npm tarball
integrity, extract the pristine artifact, and re-pin the SHA-256:

```bash
set -euo pipefail

VERSION=<new-version>

# (1) Fetch the registry tarball — npm publishes `dist.integrity` as sha512.
TARBALL=$(mktemp /tmp/model-viewer-XXXXXX.tgz)
trap 'rm -rf "$TARBALL" /tmp/model-viewer-extract' EXIT
curl -sL "https://registry.npmjs.org/@google/model-viewer/-/model-viewer-${VERSION}.tgz" -o "$TARBALL"

# (2) Independently verify npm's published integrity hash.
EXPECTED=$(curl -s "https://registry.npmjs.org/@google/model-viewer/${VERSION}" \
  | python3 -c "import sys,json; print(json.load(sys.stdin)['dist']['integrity'])")
ACTUAL=$(python3 - "$TARBALL" <<'PY'
import base64
import hashlib
import pathlib
import sys

digest = hashlib.sha512(pathlib.Path(sys.argv[1]).read_bytes()).digest()
print("sha512-" + base64.b64encode(digest).decode("ascii"))
PY
)
if [ "$ACTUAL" != "$EXPECTED" ]; then
  echo "INTEGRITY MISMATCH"
  echo "  expected: $EXPECTED"
  echo "  actual:   $ACTUAL"
  exit 1
fi

# (3) Extract the bundle — now we can trust this came from npm.
mkdir -p /tmp/model-viewer-extract
tar xzf "$TARBALL" -C /tmp/model-viewer-extract
cp "/tmp/model-viewer-extract/package/dist/model-viewer.min.js" src/app/src/renderer/vendor/model-viewer.min.js

# (4) Re-pin the sidecar hash.
cd src/app/src/renderer/vendor
sha256sum model-viewer.min.js > model-viewer.min.js.sha256
```

Then update **Version** and the two hashes above, and verify the 3D panel preview
still renders in both the Tauri app and the web-app.
