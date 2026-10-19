#!/usr/bin/env python3
"""Stage the locked Mermaid browser distribution for offline native package builds.

Downloads happen only before creating the Debian *source* package. The PPA and
Debian buildds consume the staged files without network access or npm.
"""

import argparse
import base64
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import sys
import tarfile
import tempfile
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parent.parent
DEST = ROOT / "src/web-app/vendor/mermaid"
LOCKFILES = [ROOT / "src/web-app/package-lock.json", ROOT / "src/app/package-lock.json"]
LICENSE_FALLBACK = ROOT / "tools/vendor-mermaid.LICENSE"
MAIN = "dist/mermaid.esm.min.mjs"
MANIFEST = "VENDOR-MANIFEST.json"
MAX_TARBALL_SIZE = 60 * 1024 * 1024
MAX_UNPACKED_SIZE = 100 * 1024 * 1024
IMPORT_RE = re.compile(
    r"""(?:\bfrom\s*|\bimport\s*\(\s*|\bimport\s*)["']([^"']+)["']"""
)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def locked_package():
    entries = []
    for lock_path in LOCKFILES:
        lock = json.loads(lock_path.read_text(encoding="utf-8"))
        pkg = lock.get("packages", {}).get("node_modules/mermaid")
        if not pkg:
            raise ValueError(f"Missing node_modules/mermaid in {lock_path}")
        entries.append((lock_path, pkg))
    first = entries[0][1]
    for path, pkg in entries[1:]:
        for key in ("version", "resolved", "integrity"):
            if pkg.get(key) != first.get(key):
                raise ValueError(
                    f"Mermaid {key} differs between the two lockfiles ({path}). Update both first."
                )
    version = first["version"]
    parts = version.split(".")
    if len(parts) != 3 or not all(part.isdigit() for part in parts):
        raise ValueError(f"Expected a stable Mermaid version, got {version!r}")
    if int(parts[0]) != 11 or tuple(map(int, parts)) < (11, 16, 1):
        raise ValueError(
            f"Mermaid {version} has known security issues. Upgrade both lockfiles to >=11.16.1 (11.x) first."
        )
    resolved = first["resolved"]
    expected_url = f"https://registry.npmjs.org/mermaid/-/mermaid-{version}.tgz"
    if resolved != expected_url:
        raise ValueError(f"Unexpected Mermaid tarball URL: {resolved!r}")
    integrity = first["integrity"]
    if not integrity.startswith("sha512-"):
        raise ValueError(
            "Expected npm sha512 subresource integrity in package-lock.json"
        )
    expected_digest = base64.b64decode(integrity[len("sha512-") :], validate=True)
    if len(expected_digest) != 64:
        raise ValueError("Invalid npm sha512 digest length")
    return version, resolved, integrity, expected_digest


def verify_imports(root):
    main = root / MAIN
    if not main.is_file():
        raise ValueError(f"Missing vendored Mermaid entry: {MAIN}")
    # The official ESM browser distribution references prebuilt local chunks.
    # Catch partial extractions instead of shipping broken runtime imports.
    for module in root.rglob("*.mjs"):
        source = module.read_text(encoding="utf-8")
        for specifier in IMPORT_RE.findall(source):
            if specifier.startswith("."):
                target = (module.parent / specifier).resolve()
                if not target.is_relative_to(root.resolve()) or not target.is_file():
                    raise ValueError(
                        f"Broken import {specifier!r} in {module.relative_to(root)}"
                    )
            elif specifier.startswith(("http:", "https:")):
                raise ValueError(
                    f"Remote import not permitted in vendored Mermaid: {specifier!r}"
                )
            else:
                raise ValueError(
                    f"Unbundled dependency {specifier!r} in {module.relative_to(root)}"
                )


def check_vendor(version, integrity):
    manifest_path = DEST / MANIFEST
    if not manifest_path.is_file():
        raise ValueError(
            f"Missing {manifest_path}. Run python3 tools/vendor_mermaid.py before packaging."
        )
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("version") != version or manifest.get("integrity") != integrity:
        raise ValueError(
            "Vendored Mermaid does not match the two current lockfiles. Regenerate it."
        )
    for name, checksum in manifest["files"].items():
        target = DEST / name
        if not target.is_file() or sha256(target.read_bytes()) != checksum:
            raise ValueError(f"Missing or modified vendored Mermaid file: {name}")
    verify_imports(DEST)
    print(f'Verified vendored Mermaid {version}: {len(manifest["files"])} files')


def prepare_tarball(archive_data, version, integrity, expected_digest):
    if hashlib.sha512(archive_data).digest() != expected_digest:
        raise ValueError("npm tarball SHA-512 does not match package-lock.json")

    DEST.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".mermaid-", dir=DEST.parent) as tmp:
        target_dir = Path(tmp) / "mermaid"
        target_dir.mkdir()
        file_hashes = {}
        unpacked = 0
        license_found = False
        with tarfile.open(fileobj=io.BytesIO(archive_data), mode="r:gz") as tf:
            for item in tf:
                if not item.isfile():
                    continue
                name = item.name
                if not name.startswith("package/"):
                    continue
                path = name[len("package/") :]
                if ".." in Path(path).parts:
                    raise ValueError(f"Unsafe npm archive entry: {name}")
                is_chunk = path.startswith(
                    "dist/chunks/mermaid.esm.min/"
                ) and not path.endswith(".map")
                is_metadata = path in (
                    "package.json",
                    "README.md",
                    "LICENSE",
                    "LICENSE.md",
                )
                if path != MAIN and not is_chunk and not is_metadata:
                    continue
                if (
                    item.size > MAX_UNPACKED_SIZE
                    or unpacked + item.size > MAX_UNPACKED_SIZE
                ):
                    raise ValueError(
                        "Mermaid vendored distribution exceeds extraction size limit"
                    )
                stream = tf.extractfile(item)
                if stream is None:
                    raise ValueError(f"Cannot read archive member {name}")
                content = stream.read()
                unpacked += len(content)
                out = target_dir / path
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(content)
                file_hashes[path] = sha256(content)
                if path in ("LICENSE", "LICENSE.md"):
                    license_found = True
        if not license_found:
            license_text = LICENSE_FALLBACK.read_bytes()
            (target_dir / "LICENSE").write_bytes(license_text)
            file_hashes["LICENSE"] = sha256(license_text)
        package_meta = json.loads((target_dir / "package.json").read_text())
        if (
            package_meta.get("name") != "mermaid"
            or package_meta.get("version") != version
        ):
            raise ValueError("Downloaded tarball is not the locked Mermaid package")
        verify_imports(target_dir)
        manifest = {
            "version": version,
            "integrity": integrity,
            "files": dict(sorted(file_hashes.items())),
        }
        (target_dir / MANIFEST).write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
        if DEST.exists():
            shutil.rmtree(DEST)
        shutil.move(str(target_dir), str(DEST))
    print(f"Staged Mermaid {version} ({len(file_hashes)} files) in {DEST}")
    check_vendor(version, integrity)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify existing offline vendor files without downloading",
    )
    parser.add_argument(
        "--tarball",
        type=Path,
        help="use an already downloaded npm .tgz instead of network access",
    )
    args = parser.parse_args()
    version, url, integrity, expected_digest = locked_package()
    if args.check:
        check_vendor(version, integrity)
        return
    if args.tarball:
        data = args.tarball.read_bytes()
    else:
        print(
            f"Downloading pinned Mermaid {version} from npm (before source packaging)"
        )
        with urlopen(url, timeout=120) as response:
            data = response.read(MAX_TARBALL_SIZE + 1)
    if len(data) > MAX_TARBALL_SIZE:
        raise ValueError("npm tarball exceeds download size limit")
    prepare_tarball(data, version, integrity, expected_digest)


if __name__ == "__main__":
    try:
        main()
    except (OSError, KeyError, ValueError, tarfile.TarError) as exc:
        sys.exit(f"ERROR: {exc}")
