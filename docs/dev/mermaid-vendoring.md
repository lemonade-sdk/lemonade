# Vendoring Mermaid for native GUI3 packages (temporary)

Native Debian/Ubuntu builds (`USE_SYSTEM_NODEJS_MODULES=ON`) use the official
Mermaid 11 **unminified browser ESM build**, not a no-op stub and not the npm
package's `mermaid.core.mjs` entry (which expects external npm dependencies).
The committed vendor tree contains `dist/mermaid.esm.mjs`, its static and
lazy-loaded `dist/chunks/mermaid.esm/` modules, their source maps, upstream
`LICENSE`, `package.json`, `README.md`, and `VENDOR-MANIFEST.json`.

**These files must be checked into Git.** The GitHub release preparation step
checks their hashes and Git tracking *without accessing npm or the network*.
Neither Launchpad nor Debian buildds download or regenerate vendor data.

## Refreshing the checked-in copy

From the PR branch, update the **same exact security-fixed Mermaid 11 release**
in both application manifests and locks (for example, `11.16.1`):

```sh
(cd src/app && npm install --save-exact --package-lock-only --ignore-scripts mermaid@11.16.1)
(cd src/web-app && npm install --save-dev --save-exact --package-lock-only --ignore-scripts mermaid@11.16.1)
```

Review the `package.json` and `package-lock.json` diffs in **both** directories.
A caret range or conflicting resolved tarballs is not accepted: vendoring must
pin a single reviewed release, at least `11.16.1` for the known advisories.

Only on a trusted developer machine with network access, refresh the vendor
files using the exact tarball URL and SHA-512 integrity recorded in the locks:

```sh
python3 tools/vendor_mermaid.py --update
# Alternatively, for a local copy of the exact npm archive:
# python3 tools/vendor_mermaid.py --tarball /path/to/mermaid-11.16.1.tgz

git add src/app/package.json src/app/package-lock.json \
  src/web-app/package.json src/web-app/package-lock.json
# Include all additions, deletions, and renamed upstream chunks/maps:
git add -A src/web-app/vendor/mermaid
python3 tools/vendor_mermaid.py --check --require-tracked
python3 -m unittest discover -s test -p 'vendor_mermaid_test.py'
```

The `--check` command is **offline and the default**. The extra
`--require-tracked` check uses the Git index and thus runs in CI and developer
checkouts, not inside Debian source tarballs. The exact upstream tarball is
verified before extraction, extracted paths are constrained, and a manifest
checks each committed file, the complete file set, and relative ESM imports.
Do not edit upstream-generated `.mjs` or `.map` files manually.

## Packaging and release verification

- `dpkg-buildpackage -S` must include all tracked Mermaid ESM files and
  license/manifest in the resulting source `.tar.*`; inspect that archive.
- `contrib/debian/rules` verifies the preexisting files using the offline
  `--check` path. Check an unpacked source package without network access,
  with `USE_SYSTEM_NODEJS_MODULES=ON`.
- The Docker compiler-cache prime uses a `.dockerignore`-filtered tree. The
  narrow Mermaid `dist/` exception must remain so its offline verification
  sees the same upstream chunks and maps as the full source checkout.
- The upstream README is kept verbatim and excluded from Lemonade's Markdown
  link checker: it references upstream assets intentionally not vendored here.
- Git EOL normalization and automated whitespace/EOF rewriting are disabled for
  the upstream vendor tree; a local pre-commit integrity hook catches drift.
- Browser-test the **native** packaged GUI with flowchart, sequence, class,
  ER, architecture, mindmap, and invalid Mermaid input. A successful static
  file check is not a substitute for a real Webpack build and render test.
- Before distribution, complete the third-party license and attribution audit
  for code embedded in the prebuilt ESM chunks (D3, DOMPurify, KaTeX,
  Cytoscape, etc.). The upstream Mermaid MIT license alone does **not** cover
  every bundled dependency. The `debian/copyright` stanza records the known
  license families, but is not an exhaustive per-component audit. Retained
  source maps help identify components and original sources.
- Monitor upstream security fixes, refresh the complete vendor tree, and
  rebuild/reissue packages; bundled JS is not updated by system libraries.

This is a temporary native/Ubuntu PPA solution. Unminified **bundled** ESM and
source maps still do not constitute a complete, independently rebuildable
upstream source tree. An actual Debian archive submission needs separate
source/DFSG packaging work and maintainer agreement.
