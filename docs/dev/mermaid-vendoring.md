# Vendoring Mermaid for native GUI3 packages (temporary)

GUI3 normally bundles Mermaid from npm. For native Debian/Ubuntu builds,
`USE_SYSTEM_NODEJS_MODULES=ON` uses a locally staged copy of the official
Mermaid 11 browser ESM distribution instead of the old no-op stub.

The assets are **not committed to Git**. They are generated **before** the
Debian source package is created and included in its source tarball. Launchpad
never downloads from npm, and the package build remains offline.

## Updating Mermaid

1. Update both `src/app` and `src/web-app` lockfiles, for example:
   ```sh
   (cd src/app && npm install --save-exact mermaid@11.16.1 --package-lock-only --ignore-scripts)
   (cd src/web-app && npm install --save-dev --save-exact mermaid@11.16.1 --package-lock-only --ignore-scripts)
   ```
   Use the latest reviewed/security-patched 11.x release, not necessarily 11.16.1.
   Inspect the lockfile diffs and commit `package.json` and `package-lock.json`
   from both directories.
2. From the repository root, prepare the offline assets:
   ```sh
   python3 tools/vendor_mermaid.py
   python3 tools/vendor_mermaid.py --check
   ```
   The generator verifies the npm tarball's SHA-512 against the exact
   integrity value committed in both lockfiles. The generated assets live in
   `src/web-app/vendor/mermaid/` and are gitignored.
3. Create the Debian *source* package from this prepared tree. The
   `prepare-debian-build` GitHub Action already runs the generator before
   `dpkg-buildpackage -S`; the downstream PPA build does not fetch npm data.
4. For local binary builds, generate the assets before `dpkg-buildpackage -b`.
   For reproducible offline rebuilds, run `--check` against the unpacked
   source package, not `--download` inside the package builder.

The upstream distribution's main `mermaid.esm.min.mjs` imports adjacent ESM
chunks, including dynamically loaded diagram renderers. The generator copies
these together and validates all static relative imports. Copying only the
entry file is insufficient.

## Release checks

- Verify the source `.tar.*` **includes** the vendored files and licenses.
- Ensure `USE_SYSTEM_NODEJS_MODULES=ON` builds on both Ubuntu PPAs and Debian
  test environments *without network access*.
- Run browser rendering tests against the native package for flowchart,
  sequence, class, ER, architecture, mindmap, and malformed Mermaid input.
- Audit licenses of **all transitive code inside the upstream prebundled JS**
  and update `debian/copyright` before distribution. Mermaid itself uses MIT;
  its bundled dependencies can have different licenses.
- Update the vendored copy for upstream CVEs and rebuild/reissue Lemonade:
  already-shipped JS bundles do not automatically pick up system updates.

This is a short-term release workaround and is not suitable for Debian archive
submission without further packaging work agreed with the Debian maintainer.
