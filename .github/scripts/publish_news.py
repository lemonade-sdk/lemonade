#!/usr/bin/env python3
"""Publish news posts that merged to main since the last release.

Run with the website branch checked out. For each PR merged since the last
release, in order:

- If the PR changed only news files, copy those files onto website and commit.
- If the PR changed anything else too, skip it. Its news changes go out with
  the next release.

One exception: a news file is not copied if a skipped PR also changed it,
because copying the file would publish that PR's change too. It waits for the
release instead.

Usage:
    python publish_news.py <main-ref>
"""

import subprocess
import sys

NEWS_DIR = "docs/news/"
NEWS_FILES = {"docs/assets/news-data.js"}


def git(*args):
    return subprocess.run(
        ["git", *args], check=True, capture_output=True, text=True
    ).stdout.strip()


def blob(rev, path):
    result = subprocess.run(
        ["git", "rev-parse", "--verify", "-q", f"{rev}:{path}"],
        capture_output=True,
        text=True,
    )
    return result.stdout.strip() or None


def is_news(path):
    return path.startswith(NEWS_DIR) or path in NEWS_FILES


def already_live(path, commit, main_ref):
    live = blob("HEAD", path)
    later = git(
        "rev-list", "--first-parent", f"{commit}^1..{main_ref}", "--", path
    ).splitlines()
    return any(live == blob(rev, path) for rev in later)


def publish(commit, main_ref):
    subject = git("log", "-1", "--format=%s", commit)
    changed = git(
        "diff", "--name-only", "--no-renames", f"{commit}^1", commit
    ).splitlines()
    if not changed or not any(is_news(path) for path in changed):
        return
    if not all(is_news(path) for path in changed):
        print(f"::notice::Waiting for release (touches non-news files): {subject}")
        return

    published = []
    for path in changed:
        if already_live(path, commit, main_ref):
            continue
        if blob("HEAD", path) != blob(f"{commit}^1", path):
            print(
                f"::notice::Waiting for release ({path} has unreleased changes): {subject}"
            )
            continue
        if blob(commit, path):
            git("checkout", commit, "--", path)
        else:
            git("rm", "-q", "--", path)
        published.append(path)

    if published:
        git("commit", "-q", "-m", f"Publish news: {subject}")
        print(f"Published {', '.join(published)} from: {subject}")


def main():
    main_ref = sys.argv[1]
    base = git("merge-base", "HEAD", main_ref)
    commits = git(
        "rev-list", "--reverse", "--first-parent", f"{base}..{main_ref}"
    ).splitlines()
    for commit in commits:
        publish(commit, main_ref)


if __name__ == "__main__":
    main()
