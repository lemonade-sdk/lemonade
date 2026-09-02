#!/usr/bin/env python3
"""Close idle pull requests for lemonade-sdk/lemonade.

A PR is idle when it has had no pushes and no comments (issue comments,
review submissions, or inline review comments) for --days days. Idle PRs
are closed with an explanatory comment. Used by
.github/workflows/close-idle-prs.yml; safe to run locally with --dry-run.

Usage:
    python .github/scripts/close_idle_prs.py [--days 30] [--dry-run] [--repo OWNER/REPO]

Requirements:
    - gh CLI authenticated (GH_TOKEN env var works in CI)
    - Python 3.9+ (stdlib only -- no external deps)
"""

import argparse
import datetime
import json
import subprocess
import sys

CLOSE_COMMENT = (
    "This repository automatically closes PRs that have been idle for 30 days. "
    "If you are committed to getting your code merged, please contact a maintainer "
    "on Discord for guidance. See the [Lemonade contribution guide]"
    "(https://github.com/lemonade-sdk/lemonade/blob/main/docs/dev/contribute.md) "
    "for more information, including a list of maintainers and their subject areas."
)

# GitHub stopped populating Commit.pushedDate, so the head commit's
# committedDate stands in for push time. It can lag an actual push (a
# contributor can push commits authored weeks earlier), which is why every
# candidate goes through verify_idle() before being closed.
PR_QUERY = """
query($owner: String!, $name: String!, $endCursor: String) {
  repository(owner: $owner, name: $name) {
    pullRequests(states: OPEN, first: 50, after: $endCursor) {
      pageInfo { hasNextPage endCursor }
      nodes {
        number
        title
        createdAt
        commits(last: 1) { nodes { commit { oid committedDate } } }
        comments(last: 1) { nodes { createdAt } }
        reviews(last: 1) { nodes { createdAt } }
        reviewThreads(last: 50) { nodes { comments(last: 1) { nodes { createdAt } } } }
        timelineItems(last: 1, itemTypes: [HEAD_REF_FORCE_PUSHED_EVENT]) {
          nodes { ... on HeadRefForcePushedEvent { createdAt } }
        }
      }
    }
  }
}
"""


def gh(*args, check=True):
    result = subprocess.run(
        ["gh", *args], capture_output=True, text=True, encoding="utf-8", check=False
    )
    if result.returncode != 0:
        if check:
            raise RuntimeError(f"gh {' '.join(args)} failed: {result.stderr.strip()}")
        return None
    return result.stdout


def gh_json(*args, check=True):
    out = gh(*args, check=check)
    return json.loads(out) if out else None


def parse_ts(value):
    if not value:
        return None
    return datetime.datetime.fromisoformat(value.replace("Z", "+00:00"))


def newest(*values):
    stamps = [parse_ts(v) for v in values]
    stamps = [s for s in stamps if s is not None]
    return max(stamps) if stamps else None


def first_node(container, key="createdAt"):
    nodes = (container or {}).get("nodes") or []
    return nodes[0].get(key) if nodes else None


def fetch_open_prs(repo):
    owner, name = repo.split("/", 1)
    prs, cursor = [], None
    while True:
        args = [
            "api",
            "graphql",
            "-f",
            f"query={PR_QUERY}",
            "-F",
            f"owner={owner}",
            "-F",
            f"name={name}",
        ]
        if cursor:
            args += ["-F", f"endCursor={cursor}"]
        page = gh_json(*args)["data"]["repository"]["pullRequests"]
        prs.extend(page["nodes"])
        if not page["pageInfo"]["hasNextPage"]:
            return prs
        cursor = page["pageInfo"]["endCursor"]


def last_activity(pr):
    """Most recent push or comment on a PR, from the bulk scan's data."""
    commit = first_node(pr.get("commits"), "commit") or {}
    threads = [
        first_node(thread.get("comments"))
        for thread in (pr.get("reviewThreads") or {}).get("nodes") or []
    ]
    return newest(
        commit.get("committedDate"),
        first_node(pr.get("timelineItems")),
        first_node(pr.get("comments")),
        first_node(pr.get("reviews")),
        pr["createdAt"],
        *threads,
    )


def verify_idle(repo, pr, cutoff):
    """Re-check a candidate against sources the bulk scan approximates.

    reviewThreads is capped at the 50 newest threads, so an older thread can
    hide a recent reply; and an old committedDate does not prove the commits
    were pushed long ago. Check-suite creation is the closest available proxy
    for push time, since CI fires on push.
    """
    commit = first_node(pr.get("commits"), "commit") or {}
    sha = commit.get("oid")

    review_comments = gh_json(
        "api",
        "--paginate",
        f"/repos/{repo}/pulls/{pr['number']}/comments?per_page=100",
        check=False,
    )
    if (
        review_comments
        and newest(*[c["created_at"] for c in review_comments]) >= cutoff
    ):
        return False

    if sha:
        suites = gh_json(
            "api", f"/repos/{repo}/commits/{sha}/check-suites?per_page=100", check=False
        )
        runs = (suites or {}).get("check_suites") or []
        if runs and newest(*[s["created_at"] for s in runs]) >= cutoff:
            return False

    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default="lemonade-sdk/lemonade")
    parser.add_argument(
        "--days", type=int, default=30, help="idle threshold in days (default: 30)"
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="report what would close, change nothing"
    )
    args = parser.parse_args()

    cutoff = datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(
        days=args.days
    )
    print(f"Closing PRs in {args.repo} with no push or comment since {cutoff:%Y-%m-%d}")

    prs = fetch_open_prs(args.repo)
    candidates = [pr for pr in prs if last_activity(pr) < cutoff]
    print(f"{len(prs)} open PRs, {len(candidates)} idle candidates")

    verb = "would close" if args.dry_run else "closed"
    acted = 0
    for pr in sorted(candidates, key=lambda p: p["number"]):
        number = pr["number"]
        if not verify_idle(args.repo, pr, cutoff):
            print(f"#{number}: recent activity found on re-check, skipping")
            continue
        idle_days = (
            datetime.datetime.now(datetime.timezone.utc) - last_activity(pr)
        ).days
        if not args.dry_run:
            gh(
                "pr",
                "close",
                str(number),
                "--repo",
                args.repo,
                "--comment",
                CLOSE_COMMENT,
            )
        print(f"#{number}: {verb} (idle {idle_days}d) -- {pr['title']}")
        acted += 1

    print(f"{verb.capitalize()} {acted} PRs")


if __name__ == "__main__":
    sys.exit(main())
