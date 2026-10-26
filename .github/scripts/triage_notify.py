#!/usr/bin/env python3
"""Post attention-needed comments on PRs that have gone quiet.

Runs daily. For each open PR matching a notification rule, posts one
GitHub comment tagging the right maintainer — but only if no triage-notify
comment has been posted in the last 7 days, to avoid spam.

Rules (in priority order):
  1. rfc-blocked  — rfc:required label + 14d+ no update → ping @jeremyfowers
  2. wg-stale     — wg::* label + 21d+ no update        → ping WG lead
  3. first-timer  — FIRST_TIME_CONTRIBUTOR + 5d+ open   → ping @jeremyfowers
  4. stale-unassigned — 45d+ no update + unassigned     → ping @jeremyfowers

Usage:
    python .github/scripts/triage_notify.py [--repo OWNER/REPO] [--dry-run]

Requirements:
    - gh CLI authenticated (GH_TOKEN in CI)
    - Python 3.9+ (stdlib only)
"""

import argparse
import json
import subprocess
import sys
from datetime import datetime, timedelta, timezone

ADMIN_MAINTAINERS = {
    "jeremyfowers",
    "kenvandine",
    "ramkrishna2910",
    "superm1",
    "bitgamma",
    "kpoineal",
    "Geramy",
}

WG_LEADS = {
    "wg::auto-tune": "bitgamma",
    "wg::cross-vendor-support": "kenvandine",
    "wg::cloud-hybrid": "ramkrishna2910",
    "wg::remote-use": "Geramy",
    "wg::gui-app": "kpoineal",
    "wg::enterprise-grade": "jeremyfowers",
}

SENTINEL = "<!-- triage-notify:"
DEDUP_DAYS = 7
MAX_PRS_PER_RUN = 20


def run(cmd):
    return subprocess.run(
        cmd,
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    ).stdout


def gh_api_pages(path):
    pages = json.loads(run(["gh", "api", "--paginate", "--slurp", path]))
    return [item for page in pages for item in page]


def fetch_open_prs(repo):
    now = datetime.now(timezone.utc)
    raw = gh_api_pages(f"/repos/{repo}/issues?state=open&per_page=100")
    prs = []
    for it in raw:
        if it.get("pull_request") is None:
            continue
        if it.get("draft"):
            continue
        login = (it.get("user") or {}).get("login", "")
        if login in ADMIN_MAINTAINERS:
            continue
        updated = datetime.fromisoformat(it["updated_at"].replace("Z", "+00:00"))
        created = datetime.fromisoformat(it["created_at"].replace("Z", "+00:00"))
        labels = [lbl["name"] for lbl in it.get("labels", [])]
        prs.append(
            {
                "number": it["number"],
                "title": it["title"],
                "url": it["html_url"],
                "author": login,
                "author_association": it.get("author_association", "NONE"),
                "labels": labels,
                "assignees": [a["login"] for a in it.get("assignees", []) if a],
                "days_stale": (now - updated).days,
                "days_old": (now - created).days,
                "comments": it.get("comments", 0),
            }
        )
    return prs


def already_notified(repo, pr_number):
    """Return True if a triage-notify comment was posted in the last DEDUP_DAYS."""
    cutoff = datetime.now(timezone.utc) - timedelta(days=DEDUP_DAYS)
    try:
        comments = gh_api_pages(
            f"/repos/{repo}/issues/{pr_number}/comments?per_page=100"
        )
    except subprocess.CalledProcessError:
        return False
    for c in comments:
        body = c.get("body", "")
        if SENTINEL not in body:
            continue
        created = datetime.fromisoformat(c["created_at"].replace("Z", "+00:00"))
        if created >= cutoff:
            return True
    return False


def classify_pr(pr):
    """Return (rule_name, ping_login) or None if no rule fires."""
    labels = set(pr["labels"])

    # Rule 1: RFC blocked and going stale
    if "rfc:required" in labels and pr["days_stale"] >= 14:
        return ("rfc-blocked", "jeremyfowers")

    # Rule 2: WG-labeled but quiet
    for wg_label, lead in WG_LEADS.items():
        if wg_label in labels and pr["days_stale"] >= 21:
            return ("wg-stale", lead)

    # Rule 3: First-timer with no engagement yet
    if (
        pr["author_association"] == "FIRST_TIME_CONTRIBUTOR"
        and pr["days_old"] >= 5
        and pr["comments"] == 0
    ):
        return ("first-timer", "jeremyfowers")

    # Rule 4: Long-stale and unassigned
    if pr["days_stale"] >= 45 and not pr["assignees"]:
        return ("stale-unassigned", "jeremyfowers")

    return None


def build_comment(pr, rule, ping_login):
    reasons = {
        "rfc-blocked": (
            f"This PR has been waiting {pr['days_stale']}d with an `rfc:required` label. "
            "It looks like it needs an RFC decision before it can move forward."
        ),
        "wg-stale": (
            f"This PR has been quiet for {pr['days_stale']}d and carries a workgroup label "
            "that falls under your charter."
        ),
        "first-timer": (
            f"This is @{pr['author']}'s first contribution and it's been open {pr['days_old']}d "
            "with no review yet."
        ),
        "stale-unassigned": (
            f"This PR has had no activity for {pr['days_stale']}d and has no assignee."
        ),
    }
    reason = reasons.get(rule, f"This PR needs attention ({rule}).")
    today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    return (
        f"Hey @{ping_login} — {reason} "
        f"Would be sweet to get a look at this when you have a moment!\n\n"
        f"{SENTINEL} {rule} {today} -->"
    )


def post_comment(repo, pr_number, body, dry_run):
    if dry_run:
        print(f"  [dry-run] would post:\n{body}\n")
        return
    cmd = [
        "gh",
        "api",
        f"/repos/{repo}/issues/{pr_number}/comments",
        "-f",
        f"body={body}",
    ]
    run(cmd)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--repo", default="lemonade-sdk/lemonade", help="OWNER/REPO")
    p.add_argument("--dry-run", action="store_true", help="Print; do not post")
    p.add_argument(
        "--max", type=int, default=MAX_PRS_PER_RUN, help="Max PRs to notify per run"
    )
    args = p.parse_args()

    print(f"Fetching open PRs from {args.repo}…", file=sys.stderr)
    prs = fetch_open_prs(args.repo)
    print(f"  {len(prs)} candidate PRs", file=sys.stderr)

    notified = 0
    for pr in prs:
        if notified >= args.max:
            print(f"  reached --max {args.max}, stopping", file=sys.stderr)
            break

        result = classify_pr(pr)
        if result is None:
            continue

        rule, ping_login = result
        print(
            f"  #{pr['number']} matches rule={rule} ping=@{ping_login}", file=sys.stderr
        )

        if already_notified(args.repo, pr["number"]):
            print(f"    skipped (notified within {DEDUP_DAYS}d)", file=sys.stderr)
            continue

        body = build_comment(pr, rule, ping_login)
        post_comment(args.repo, pr["number"], body, args.dry_run)
        print(f"    {'[dry-run] ' if args.dry_run else ''}posted", file=sys.stderr)
        notified += 1

    print(
        f"Done. {'Would notify' if args.dry_run else 'Notified'} {notified} PRs.",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
