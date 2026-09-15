#!/usr/bin/env python3

"""Deterministically create and push a Lemonade release tag.

Guards against the classic hand-tagging mistake (picking the wrong patch
number) by computing the version from the release branch itself using
``tools/version.py``, then asking for confirmation before it creates a signed
tag and pushes it.

Steps:

1. Fetch the remote's ``release-v*`` branches and tags.
2. Select the newest ``release-vYYYY.WW`` branch (or an explicit one for a
   hotfix on an older branch).
3. Compute the version from that branch tip via ``tools/version.py``.
4. Confirm ``Create and push vYYYY.WW.N?`` with the operator.
5. Create a signed tag on that exact commit and push it, aborting if the branch
   advanced in the meantime or the tag already exists.

Usage::

    # Release the newest active branch
    python tools/release.py

    # Hotfix on an explicit older branch
    python tools/release.py release-v2026.34

    # Preview without creating or pushing anything
    python tools/release.py --dry-run
"""

import argparse
import subprocess
import sys
from pathlib import Path

try:
    from tools.version import RELEASE_BRANCH_PATTERN, get_version, git
except ModuleNotFoundError:
    from version import RELEASE_BRANCH_PATTERN, get_version, git


def fetch(repo, remote):
    git("fetch", "--prune", "--tags", remote, cwd=repo)


def remote_release_branches(repo, remote):
    output = git("branch", "--remotes", "--list", f"{remote}/release-v*", cwd=repo)
    branches = []
    for line in output.splitlines():
        name = line.strip()
        if not name or "->" in name:
            continue
        short = name[len(remote) + 1 :]
        match = RELEASE_BRANCH_PATTERN.fullmatch(short)
        if match:
            branches.append((int(match.group(1)), int(match.group(2)), short))
    return branches


def select_branch(repo, remote, explicit):
    branches = remote_release_branches(repo, remote)
    if not branches:
        raise RuntimeError(f"no {remote}/release-vYYYY.WW branches found")
    if explicit:
        for _, _, short in branches:
            if short == explicit:
                return short
        available = ", ".join(short for _, _, short in sorted(branches))
        raise RuntimeError(f"{remote}/{explicit} not found; available: {available}")
    return max(branches)[2]


def tag_exists(repo, remote, tag):
    if git("tag", "--list", tag, cwd=repo):
        return True
    return bool(git("ls-remote", "--tags", remote, tag, cwd=repo))


def confirm(prompt):
    reply = input(f"{prompt} [y/N] ").strip().lower()
    return reply in ("y", "yes")


def create_release(repo, remote, explicit, message, assume_yes, dry_run, sign=True):
    fetch(repo, remote)

    branch = select_branch(repo, remote, explicit)
    ref = f"{remote}/{branch}"
    sha = git("rev-parse", ref, cwd=repo)
    print(f"Releasing {ref} at commit {sha[:12]}")

    version = get_version(repo, branch=branch, head=ref)
    year, week = branch[len("release-v") :].split(".")
    if not version.startswith(f"{year}.{week}."):
        raise RuntimeError(f"computed version {version} does not match branch {branch}")

    tag = f"v{version}"
    if tag_exists(repo, remote, tag):
        raise RuntimeError(f"tag {tag} already exists")

    if dry_run:
        print(f"[dry-run] would create and push signed tag {tag} on {sha[:12]}")
        return

    if not assume_yes and not confirm(f"Create and push {tag}?"):
        print("aborted")
        return

    sign_flag = "--sign" if sign else "--annotate"
    git("tag", sign_flag, "--message", message.format(tag=tag), tag, sha, cwd=repo)
    try:
        current = git("rev-parse", ref, cwd=repo)
        if current != sha:
            raise RuntimeError(
                f"{ref} advanced from {sha[:12]} to {current[:12]}; aborting"
            )
        git("push", remote, tag, cwd=repo)
    except (RuntimeError, subprocess.CalledProcessError):
        git("tag", "--delete", tag, cwd=repo)
        raise

    print(f"pushed {tag} -> {remote}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "branch",
        nargs="?",
        help="explicit release-vYYYY.WW branch (default: newest)",
    )
    parser.add_argument("--remote", default="origin")
    parser.add_argument(
        "--message",
        default="Lemonade {tag}",
        help="signed tag message ('{tag}' is substituted)",
    )
    parser.add_argument("--yes", action="store_true", help="skip confirmation")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--no-sign",
        dest="sign",
        action="store_false",
        help="create an annotated (unsigned) tag instead of a signed one",
    )
    args = parser.parse_args()

    repo = Path(__file__).resolve().parent.parent
    try:
        create_release(
            repo,
            args.remote,
            args.branch,
            args.message,
            args.yes,
            args.dry_run,
            args.sign,
        )
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
