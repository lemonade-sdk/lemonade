#!/usr/bin/env python3
"""Vendor the Lemonade Nexus client SDK from one upstream commit.

src/LemonadeNexusSDK holds exactly the profile paths of the pinned Nexus commit
with the patches in src/LemonadeNexusSDK/patches applied in order.
UPSTREAM.json records the pin, the profile and the patch series.

Usage:
    python tools/update_nexus_sdk.py <commit-or-tag>   # re-vendor and re-pin
    python tools/update_nexus_sdk.py --check           # fail if the tree drifted

--source takes a local Nexus clone instead of the upstream URL.
"""

import argparse
import filecmp
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SDK_DIR = REPO_ROOT / "src" / "LemonadeNexusSDK"
MANIFEST = SDK_DIR / "UPSTREAM.json"
PATCH_DIR = SDK_DIR / "patches"
METADATA = {"UPSTREAM.json", "patches"}


class VendorError(RuntimeError):
    pass


def git(*args, cwd, capture=True):
    result = subprocess.run(
        ["git", *args], cwd=cwd, capture_output=capture, text=True, check=False
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise VendorError(f"git {' '.join(args)} failed: {detail}")
    return result.stdout.strip() if capture else ""


def build_tree(manifest, ref, source, work):
    """Return a directory holding upstream@ref's profile with the patches applied."""
    clone = work / "upstream"
    clone.mkdir()
    git("init", "-q", cwd=clone)
    try:
        git("fetch", "-q", "--depth", "1", source, ref, cwd=clone)
        commit = git("rev-parse", "FETCH_HEAD", cwd=clone)
    except VendorError:
        # An abbreviated SHA is not a fetchable ref; fetch history and resolve it.
        git(
            "fetch",
            "-q",
            "--tags",
            source,
            "+refs/heads/*:refs/remotes/nexus/*",
            cwd=clone,
        )
        commit = git("rev-parse", "--verify", f"{ref}^{{commit}}", cwd=clone)

    archive = work / "profile.tar"
    with open(archive, "wb") as out:
        result = subprocess.run(
            ["git", "archive", "--format=tar", commit, *manifest["paths"]],
            cwd=clone,
            stdout=out,
            stderr=subprocess.PIPE,
            check=False,
        )
    if result.returncode != 0:
        raise VendorError(
            f"profile paths missing at {commit}: {result.stderr.decode().strip()}"
        )

    tree = work / "tree"
    tree.mkdir()
    with tarfile.open(archive) as tar:
        try:
            tar.extractall(tree, filter="data")
        except TypeError:  # Python before 3.12 has no extraction filters.
            tar.extractall(tree)
    for path in manifest.get("exclude", []):
        target = tree / path
        if not target.exists():
            raise VendorError(f"excluded path {path} does not exist at {commit}")
        shutil.rmtree(target) if target.is_dir() else target.unlink()

    identity = [
        "-c",
        "user.name=update_nexus_sdk",
        "-c",
        "user.email=nexus-sdk@invalid",
    ]
    git("init", "-q", cwd=tree)
    git(*identity, "add", "-A", cwd=tree)
    git(*identity, "commit", "-q", "--no-verify", "-m", f"upstream {commit}", cwd=tree)
    patches = sorted(PATCH_DIR.glob("*.patch")) if PATCH_DIR.is_dir() else []
    for patch in patches:
        try:
            git(*identity, "am", "--quiet", str(patch), cwd=tree)
        except VendorError as error:
            raise VendorError(f"{patch.name} does not apply to {commit}: {error}")
    shutil.rmtree(tree / ".git")

    version_result = subprocess.run(
        ["git", "show", f"{commit}:VERSION"],
        cwd=clone,
        capture_output=True,
        text=True,
        check=False,
    )
    version = version_result.stdout.strip() if version_result.returncode == 0 else ""
    return tree, commit, version, [p.name for p in patches]


def vendored_files(root):
    files = set()
    for path in root.rglob("*"):
        relative = path.relative_to(root)
        if relative.parts[0] in METADATA or not path.is_file():
            continue
        files.add(relative.as_posix())
    return files


def compare(expected, actual):
    """List paths that differ between the rebuilt tree and the committed one."""
    want, have = vendored_files(expected), vendored_files(actual)
    problems = [f"missing: {p}" for p in sorted(want - have)]
    problems += [f"not upstream: {p}" for p in sorted(have - want)]
    for path in sorted(want & have):
        if not filecmp.cmp(expected / path, actual / path, shallow=False):
            problems.append(f"changed: {path}")
    return problems


def install(tree):
    for entry in SDK_DIR.iterdir():
        if entry.name in METADATA:
            continue
        shutil.rmtree(entry) if entry.is_dir() else entry.unlink()
    for path in tree.rglob("*"):
        if path.is_file():
            target = SDK_DIR / path.relative_to(tree)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ref", nargs="?", help="Nexus commit, tag or branch to vendor")
    parser.add_argument("--source", help="Nexus repository URL or local clone")
    parser.add_argument(
        "--check", action="store_true", help="rebuild the pin and compare"
    )
    args = parser.parse_args()
    if args.check == bool(args.ref):
        parser.error("pass a ref to vendor, or --check")

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    source = args.source or manifest["repository"]
    ref = manifest["commit"] if args.check else args.ref

    with tempfile.TemporaryDirectory() as temp:
        try:
            tree, commit, version, patches = build_tree(
                manifest, ref, source, Path(temp)
            )
        except VendorError as error:
            print(f"error: {error}", file=sys.stderr)
            return 1

        if args.check:
            problems = compare(tree, SDK_DIR)
            if problems:
                print(
                    f"src/LemonadeNexusSDK differs from Nexus {commit} plus "
                    f"{len(patches)} patch(es):"
                )
                for problem in problems:
                    print(f"  {problem}")
                print(
                    "Change the SDK upstream or add a patch, then run "
                    "tools/update_nexus_sdk.py."
                )
                return 1
            print(
                f"src/LemonadeNexusSDK matches Nexus {commit} plus "
                f"{len(patches)} patch(es)."
            )
            return 0

        previous = manifest.get("commit", "")
        install(tree)
        manifest.update({"commit": commit, "version": version, "patches": patches})
        MANIFEST.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        changed = git(
            "status",
            "--short",
            "--",
            os.path.relpath(SDK_DIR, REPO_ROOT),
            cwd=REPO_ROOT,
        )
        print(f"Nexus {previous[:12] or '(none)'} -> {commit} (version {version})")
        print(
            f"{len(vendored_files(SDK_DIR))} files, {len(patches)} patch(es) applied:"
        )
        for name in patches:
            print(f"  {name}")
        print(f"{len(changed.splitlines())} path(s) changed in src/LemonadeNexusSDK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
