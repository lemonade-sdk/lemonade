#!/usr/bin/env python3

import datetime
import os
import re
import subprocess
import sys
from pathlib import Path

RELEASE_BRANCH_PATTERN = re.compile(r"^release-v(\d{4})\.(\d{1,2})$")
RELEASE_TAG_PATTERN = re.compile(r"^v(\d{4})\.(\d{1,2})\.(\d+)$")


def git(*args, cwd):
    result = subprocess.run(
        ["git", *args],
        cwd=cwd,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return result.stdout.strip()


def exact_release_tag(repo):
    tags = git("tag", "--points-at", "HEAD", cwd=repo).splitlines()
    matches = [tag for tag in tags if RELEASE_TAG_PATTERN.fullmatch(tag)]
    if not matches:
        return None
    return max(matches, key=lambda tag: int(tag.rsplit(".", 1)[1]))[1:]


def current_branch(repo):
    branch = git("branch", "--show-current", cwd=repo)
    if branch:
        return branch

    github_ref_type = os.environ.get("GITHUB_REF_TYPE")
    github_ref_name = os.environ.get("GITHUB_REF_NAME", "")
    if github_ref_type == "branch" and github_ref_name:
        return github_ref_name
    if github_ref_name.startswith("release-v"):
        return github_ref_name
    return ""


def commits_after(repo, revision):
    return int(git("rev-list", "--count", f"{revision}..HEAD", cwd=repo))


def release_commit_number(repo, year, week):
    tag_prefix = f"v{year}.{week}."
    tags = git("tag", "--merged", "HEAD", "--list", f"{tag_prefix}*", cwd=repo)
    numbered_tags = []
    for tag in tags.splitlines():
        match = RELEASE_TAG_PATTERN.fullmatch(tag)
        if match and int(match.group(1)) == year and int(match.group(2)) == week:
            numbered_tags.append((int(match.group(3)), tag))

    if numbered_tags:
        number, tag = max(numbered_tags)
        return number + commits_after(repo, tag)

    for main_ref in ("origin/main", "main"):
        try:
            merge_base = git("merge-base", "HEAD", main_ref, cwd=repo)
            return commits_after(repo, merge_base)
        except subprocess.CalledProcessError:
            continue

    raise RuntimeError("could not find main to locate the release branch point")


def upcoming_release_week(now):
    days_until_wednesday = (2 - now.weekday()) % 7
    cutoff = (now + datetime.timedelta(days=days_until_wednesday)).replace(
        hour=19, minute=0, second=0, microsecond=0
    )
    if now >= cutoff:
        cutoff += datetime.timedelta(days=7)
    release_date = cutoff + datetime.timedelta(days=7)
    iso_date = release_date.isocalendar()
    return iso_date.year, iso_date.week


def generated_version(repo, now):
    tag_version = exact_release_tag(repo)
    if tag_version:
        return tag_version

    branch = current_branch(repo)
    release_match = RELEASE_BRANCH_PATTERN.fullmatch(branch)
    if release_match:
        year = int(release_match.group(1))
        week = int(release_match.group(2))
        number = release_commit_number(repo, year, week)
        return f"{year}.{week}.{number}"

    year, week = upcoming_release_week(now)
    commit_count = git("rev-list", "--count", "HEAD", cwd=repo)
    commit_hash = git("rev-parse", "--short=8", "HEAD", cwd=repo)
    return f"{year}.{week}.0~{commit_count}.{commit_hash}"


def get_version(repo, now=None):
    override = repo / ".version"
    if override.is_file():
        version = override.read_text(encoding="utf-8").strip()
        if not version:
            raise RuntimeError(f"{override} is empty")
        return version

    if now is None:
        now = datetime.datetime.now(datetime.timezone.utc)
    return generated_version(repo, now.astimezone(datetime.timezone.utc))


def main():
    repo = Path(__file__).resolve().parent.parent
    try:
        print(get_version(repo))
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError) as error:
        print(f"error: could not determine Lemonade version: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
