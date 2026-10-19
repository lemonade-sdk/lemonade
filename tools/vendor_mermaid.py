#!/usr/bin/env python3
"""Verify or explicitly refresh the committed Mermaid browser ESM vendor tree.

Native Debian/Ubuntu builds and source-package preparation use --check only.
Only --update downloads; --tarball refreshes from an existing npm archive.
"""

import argparse
import base64
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tarfile
import tempfile
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parent.parent
DEST = ROOT / "src/web-app/vendor/mermaid"
LOCKFILES = (ROOT / "src/app/package-lock.json", ROOT / "src/web-app/package-lock.json")
MAIN = "dist/mermaid.esm.mjs"
MAIN_MAP = MAIN + ".map"
CHUNKS = "dist/chunks/mermaid.esm/"
MANIFEST = "VENDOR-MANIFEST.json"
METADATA = {"package.json", "LICENSE", "README.md"}
MAX_TARBALL_SIZE = 60 * 1024 * 1024
MAX_UNPACKED_SIZE = 100 * 1024 * 1024


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def locked_package():
    entries = []
    for lock_path in LOCKFILES:
        lock = json.loads(lock_path.read_text(encoding="utf-8"))
        packages = lock.get("packages", {})
        pkg = packages.get("node_modules/mermaid")
        declared = packages.get("", {}).get("dependencies", {}).get("mermaid")
        if declared is None:
            declared = packages.get("", {}).get("devDependencies", {}).get("mermaid")
        if not pkg or not declared:
            raise ValueError(f"Mermaid missing from {lock_path}")
        entries.append((lock_path, pkg, declared))

    first = entries[0][1]
    version = first["version"]
    parts = version.split(".")
    if len(parts) != 3 or not all(part.isdecimal() for part in parts):
        raise ValueError(f"Expected a stable Mermaid version, got {version!r}")
    if int(parts[0]) != 11 or tuple(map(int, parts)) < (11, 16, 1):
        raise ValueError(
            f"Mermaid {version} is not supported: pin a security-fixed 11.x (>= 11.16.1)"
        )

    for lock_path, pkg, declared in entries:
        if declared != version:
            raise ValueError(
                f"Pin mermaid to exactly {version} in {lock_path}, not {declared!r}"
            )
        for field in ("version", "resolved", "integrity"):
            if pkg.get(field) != first.get(field):
                raise ValueError(
                    f"Mermaid {field} differs between lockfiles ({lock_path})"
                )
        pkg_json_path = lock_path.with_name("package.json")
        pkg_json = json.loads(pkg_json_path.read_text(encoding="utf-8"))
        requested = pkg_json.get("dependencies", {}).get("mermaid")
        if requested is None:
            requested = pkg_json.get("devDependencies", {}).get("mermaid")
        if requested != version:
            raise ValueError(f"Pin mermaid to exactly {version} in {pkg_json_path}")

    url = f"https://registry.npmjs.org/mermaid/-/mermaid-{version}.tgz"
    if first["resolved"] != url:
        raise ValueError(f"Unexpected Mermaid source URL: {first['resolved']!r}")
    integrity = first["integrity"]
    if not integrity.startswith("sha512-"):
        raise ValueError("Expected exactly one SHA-512 npm integrity value")
    try:
        digest = base64.b64decode(integrity[7:], validate=True)
    except ValueError as exc:
        raise ValueError("Invalid npm integrity encoding") from exc
    if len(digest) != 64:
        raise ValueError("Invalid npm SHA-512 length")
    return version, url, integrity, digest


def selected_path(path):
    return (
        path in (MAIN, MAIN_MAP)
        or path in METADATA
        or (path.startswith(CHUNKS) and path.endswith((".mjs", ".mjs.map")))
    )


def safe_path(path):
    p = PurePosixPath(path)
    if (
        not path
        or "\\" in path
        or p.is_absolute()
        or p.as_posix() != path
        or any(x in (".", "..") for x in path.split("/"))
    ):
        raise ValueError(f"Unsafe vendored path: {path!r}")
    return p


def esm_specifiers(source):
    """Find literal ESM imports, ignoring JS strings, comments and templates.

    A plain `from ...` regex also matches text in Mermaid's generated template
    strings (e.g. `from '${source.modelEntityType}'`). Tokenize only enough JS
    to identify imports and re-exports; no external parser is needed on buildds.
    """
    tokens = []
    length = len(source)

    def quoted(pos):
        quote = source[pos]
        start = pos + 1
        pos = start
        while pos < length:
            if source[pos] == "\\":
                pos += 2
            elif source[pos] == quote:
                tokens.append(("string", source[start:pos]))
                return pos + 1
            else:
                pos += 1
        raise ValueError("Unterminated JavaScript string in vendored ESM")

    def template(pos):
        pos += 1
        while pos < length:
            if source[pos] == "\\":
                pos += 2
            elif source[pos] == "`":
                return pos + 1
            elif source.startswith("${", pos):
                pos = code(pos + 2, interpolation=True)
            else:
                pos += 1
        raise ValueError("Unterminated JavaScript template in vendored ESM")

    def regex_literal(pos):
        pos += 1
        in_class = False
        while pos < length:
            char = source[pos]
            if char == "\\":
                pos += 2
                continue
            if char == "[":
                in_class = True
            elif char == "]":
                in_class = False
            elif char == "/" and not in_class:
                pos += 1
                while pos < length and (source[pos].isalnum() or source[pos] in "_$"):
                    pos += 1
                return pos
            pos += 1
        raise ValueError("Unterminated JavaScript regexp in vendored ESM")

    def code(pos, interpolation=False):
        braces = 0
        while pos < length:
            char = source[pos]
            if interpolation and char == "}" and braces == 0:
                return pos + 1
            if char.isspace():
                pos += 1
            elif source.startswith("//", pos):
                end = source.find("\n", pos + 2)
                pos = length if end < 0 else end + 1
            elif source.startswith("/*", pos):
                end = source.find("*/", pos + 2)
                if end < 0:
                    raise ValueError("Unterminated JavaScript comment in vendored ESM")
                pos = end + 2
            elif char in ("'", '"'):
                pos = quoted(pos)
            elif char == "`":
                pos = template(pos)
            elif (
                char == "/"
                and tokens
                and (
                    tokens[-1][1] in ("(", "[", "{", "=", ":", ",", ";", "!", "?", ">")
                    or (
                        tokens[-1][0] == "word"
                        and tokens[-1][1]
                        in ("return", "throw", "case", "yield", "await")
                    )
                )
            ):
                pos = regex_literal(pos)
            elif char.isascii() and (char.isalpha() or char in "_$"):
                end = pos + 1
                while (
                    end < length
                    and source[end].isascii()
                    and (source[end].isalnum() or source[end] in "_$")
                ):
                    end += 1
                tokens.append(("word", source[pos:end]))
                pos = end
            else:
                if interpolation:
                    if char == "{":
                        braces += 1
                    elif char == "}":
                        braces -= 1
                tokens.append(("punct", char))
                pos += 1
        if interpolation:
            raise ValueError(
                "Unterminated JavaScript template expression in vendored ESM"
            )
        return pos

    code(0)

    for index, (kind, value) in enumerate(tokens):
        if kind != "word" or value not in ("import", "export"):
            continue
        if index and tokens[index - 1][1] == ".":  # import.meta, object.import(...)
            continue
        next_index = index + 1
        if next_index >= len(tokens):
            continue
        next_kind, next_value = tokens[next_index]
        if value == "import" and next_kind == "string":  # import "./chunk.mjs"
            yield next_value
        elif value == "import" and next_value == "(":  # import("./chunk.mjs")
            if next_index + 1 < len(tokens) and tokens[next_index + 1][0] == "string":
                yield tokens[next_index + 1][1]
        elif next_value in ("{", "*") or (value == "import" and next_kind == "word"):
            # import {a} from "...", import a from "...", export * from "..."
            for j in range(next_index + 1, len(tokens) - 1):
                if tokens[j] == ("word", "from") and tokens[j + 1][0] == "string":
                    yield tokens[j + 1][1]
                    break
                if tokens[j][1] == ";":
                    break


def verify_imports(root):
    root = root.resolve()
    modules = sorted(root.rglob("*.mjs"))
    if not (root / MAIN).is_file() or len(modules) < 2:
        raise ValueError("Mermaid ESM entry or diagram chunks are missing")
    for module in modules:
        content = module.read_text(encoding="utf-8")
        for specifier in esm_specifiers(content):
            if not specifier.startswith(".") or "\\" in specifier:
                raise ValueError(
                    f"Non-relative ESM import in {module.relative_to(root)}: {specifier!r}"
                )
            dependency = (module.parent / specifier).resolve()
            if not dependency.is_relative_to(root) or not dependency.is_file():
                raise ValueError(
                    f"Broken ESM import in {module.relative_to(root)}: {specifier!r}"
                )


def verify_tree(root, version, url, integrity):
    if not root.is_dir() or root.is_symlink():
        raise ValueError(f"Missing or invalid vendored directory: {root}")
    manifest_path = root / MANIFEST
    if not manifest_path.is_file() or manifest_path.is_symlink():
        raise ValueError(
            f"Missing {manifest_path}; refresh Mermaid and commit its files"
        )
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (
        manifest.get("version"),
        manifest.get("resolved"),
        manifest.get("integrity"),
        manifest.get("entry"),
    ) != (version, url, integrity, MAIN):
        raise ValueError(
            "Vendored Mermaid provenance differs from pinned npm lockfiles"
        )
    expected = manifest.get("files")
    if not isinstance(expected, dict) or not expected:
        raise ValueError("Missing vendor file inventory")
    if not ({MAIN, MAIN_MAP, "LICENSE", "package.json"} <= expected.keys()):
        raise ValueError(
            "Vendored Mermaid entry, license or metadata missing from manifest"
        )
    if any(not selected_path(name) for name in expected):
        raise ValueError(
            "Manifest contains files outside the permitted vendor distribution"
        )
    actual = set()
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"Symlink not permitted in vendor tree: {path}")
        if path.is_file():
            actual.add(path.relative_to(root).as_posix())
    if actual != set(expected) | {MANIFEST}:
        raise ValueError(
            f"Vendor file set differs from manifest: missing={sorted(set(expected) - actual)}, unexpected={sorted(actual - set(expected) - {MANIFEST})}"
        )
    for name, digest in expected.items():
        safe_path(name)
        if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
            raise ValueError(f"Invalid SHA-256 for {name}")
        if sha256((root / name).read_bytes()) != digest:
            raise ValueError(f"Vendored Mermaid file changed: {name}")
    package = json.loads((root / "package.json").read_text(encoding="utf-8"))
    if (
        package.get("name") != "mermaid"
        or package.get("version") != version
        or package.get("license") != "MIT"
    ):
        raise ValueError(
            "Vendored package.json does not identify the expected Mermaid release"
        )
    verify_imports(root)
    return set(expected) | {MANIFEST}


def require_tracked(paths):
    command = [
        "git",
        "-C",
        str(ROOT),
        "ls-files",
        "-z",
        "--",
        "src/web-app/vendor/mermaid",
    ]
    tracked = set(
        subprocess.check_output(command).decode("utf-8").strip("\0").split("\0")
    )
    expected = {(DEST.relative_to(ROOT) / name).as_posix() for name in paths}
    if tracked != expected:
        raise ValueError(
            f"Vendor tree is not fully committed: missing={sorted(expected - tracked)}, extra={sorted(tracked - expected)}"
        )


def check_vendor(version, url, integrity, tracked=False):
    paths = verify_tree(DEST, version, url, integrity)
    if tracked:
        require_tracked(paths)
    print(f"Verified committed Mermaid {version}: {len(paths) - 1} files (offline)")


def prepare_tarball(data, version, url, integrity, expected_digest):
    if len(data) > MAX_TARBALL_SIZE or hashlib.sha512(data).digest() != expected_digest:
        raise ValueError(
            "npm archive exceeds size limit or SHA-512 does not match the lockfiles"
        )
    DEST.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".mermaid-", dir=DEST.parent) as tmp:
        temp = Path(tmp)
        stage = temp / "mermaid"
        stage.mkdir()
        inventory = {}
        size = 0
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
            for member in archive:
                if not member.name.startswith("package/"):
                    continue
                rel = member.name[len("package/") :]
                if not rel and member.isdir():
                    continue
                safe_path(rel)
                if not selected_path(rel):
                    continue
                if not member.isfile() or rel in inventory:
                    raise ValueError(
                        f"Invalid or duplicated npm archive member: {member.name}"
                    )
                size += member.size
                if size > MAX_UNPACKED_SIZE or len(inventory) >= 2000:
                    raise ValueError(
                        "Mermaid vendor exceeds unpacked size or file-count limit"
                    )
                stream = archive.extractfile(member)
                if stream is None:
                    raise ValueError(f"Cannot read npm archive member: {member.name}")
                body = stream.read(member.size + 1)
                if len(body) != member.size:
                    raise ValueError(
                        f"Unexpected npm archive member size: {member.name}"
                    )
                out = stage / rel
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(body)
                inventory[rel] = sha256(body)
        manifest = {
            "version": version,
            "resolved": url,
            "integrity": integrity,
            "entry": MAIN,
            "files": dict(sorted(inventory.items())),
        }
        (stage / MANIFEST).write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
        verify_tree(stage, version, url, integrity)
        backup = temp / "previous"
        if DEST.exists():
            DEST.rename(backup)
        try:
            stage.rename(DEST)
        except OSError:
            if backup.exists():
                backup.rename(DEST)
            raise
    print(f"Refreshed Mermaid {version} in {DEST}; commit every file before packaging")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument(
        "--update",
        action="store_true",
        help="explicitly download and refresh from locked npm tarball",
    )
    source.add_argument(
        "--tarball",
        type=Path,
        help="refresh using an existing pinned npm tarball (offline)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify vendored files without network (default)",
    )
    parser.add_argument(
        "--require-tracked",
        action="store_true",
        help="also verify every vendor file is staged/tracked by Git",
    )
    args = parser.parse_args()
    if args.check and (args.update or args.tarball):
        parser.error("--check cannot be combined with --update/--tarball")
    if args.require_tracked and (args.update or args.tarball):
        parser.error("--require-tracked is only valid during offline checks")
    version, url, integrity, digest = locked_package()
    if not (args.update or args.tarball):
        check_vendor(version, url, integrity, args.require_tracked)
        return
    if args.tarball:
        data = args.tarball.read_bytes()
    else:
        print(f"Downloading pinned Mermaid {version} for explicit vendor refresh")
        with urlopen(url, timeout=120) as response:
            data = response.read(MAX_TARBALL_SIZE + 1)
    prepare_tarball(data, version, url, integrity, digest)


if __name__ == "__main__":
    try:
        main()
    except (
        OSError,
        KeyError,
        ValueError,
        subprocess.CalledProcessError,
        tarfile.TarError,
    ) as exc:
        sys.exit(f"ERROR: {exc}")
