#!/usr/bin/env python3
"""Work out which reviewers a pull request requires, and whether it has them.

A PR needs a *primary* review from the maintainer of each vertical it touches,
plus an *expert* review from the maintainer of each horizontal its diff or PR
body invokes. Expert reviews are additive — they never stand in for a primary
one.
A reviewer counts as satisfied when they authored the PR or have an approving
review on its current head commit — any push after an approval invalidates it.
Used by .github/workflows/required_reviewers.yml; safe to run locally against
any open PR for spot-testing.

Usage:
    python .github/scripts/check_maintainer_involvement.py <pr> [--repo OWNER/REPO]

Requirements:
    - gh CLI authenticated (GH_TOKEN env var works in CI)
    - Python 3.9+ (stdlib only — no external deps)
"""

import argparse
import json
import os
import re
import subprocess
import sys

# Verticals: a function of the repo that lives in known folders. Every PR needs
# a primary review from the maintainer of each vertical it touches.
VERTICALS = (
    {"name": "CLI", "prefixes": ("src/cpp/cli/",), "reviewers": ("bitgamma",)},
    {"name": "GUI", "prefixes": ("src/app/",), "reviewers": ("kpoineal",)},
)

# Anything not claimed by a vertical above falls to the project maintainers.
FALLBACK_VERTICAL = {
    "name": "Everything else",
    "reviewers": ("jeremyfowers", "ramkrishna2910"),
}

# Horizontals: a function that cuts across folders, recognized by the words the
# PR uses rather than the paths it touches. Adds an expert review on top of
# whatever primary review the paths already require.
HORIZONTALS = (
    {
        "name": "Networking & security",
        "keywords": ("http", "curl", "tcp", "udp", "cors", "security"),
        "reviewers": ("Geramy",),
    },
    {
        "name": "ROCm",
        "keywords": ("rocm",),
        "reviewers": ("superm1",),
    },
)

for _horizontal in HORIZONTALS:
    _horizontal["pattern"] = re.compile(
        "|".join(re.escape(k) for k in _horizontal["keywords"]), re.IGNORECASE
    )

# Reviews in these states say nothing about whether the reviewer approves,
# so they never displace an earlier verdict from the same person.
NON_VERDICT_REVIEW_STATES = ("COMMENTED", "PENDING")


class CheckError(Exception):
    """Fatal error fetching or interpreting PR data."""


def gh_api(path, repo, accept=None, paginate=False):
    cmd = ["gh", "api", "-H", "X-GitHub-Api-Version: 2022-11-28"]
    if accept:
        cmd += ["-H", f"Accept: {accept}"]
    if paginate:
        cmd += ["--paginate"]
    cmd += [f"/repos/{repo}/{path}"]
    result = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise CheckError(f"gh api {path} failed: {result.stderr.strip()}")
    return result.stdout


def gh_api_json(path, repo, paginate=False):
    raw = gh_api(path, repo, paginate=paginate)
    if paginate:
        # --paginate concatenates one JSON array per page; merge them.
        decoder = json.JSONDecoder()
        merged, index = [], 0
        while index < len(raw):
            if raw[index].isspace():
                index += 1
                continue
            page, offset = decoder.raw_decode(raw, index)
            merged.extend(page)
            index = offset
        return merged
    return json.loads(raw)


def changed_paths(files):
    """Every path the PR touches, counting both sides of a rename."""
    paths = set()
    for entry in files:
        paths.add(entry["filename"])
        if entry.get("previous_filename"):
            paths.add(entry["previous_filename"])
    return sorted(paths)


def diff_text(pr_number, repo, files):
    """The added and removed lines of the PR, without diff context."""
    try:
        raw = gh_api(
            f"pulls/{pr_number}", repo, accept="application/vnd.github.v3.diff"
        )
    except CheckError:
        # GitHub refuses the diff media type on very large PRs; the per-file
        # patches carry the same content minus files it considers too big.
        raw = "\n".join(entry.get("patch") or "" for entry in files)

    changed = []
    for line in raw.splitlines():
        if line.startswith(("+++", "---")):
            continue
        if line.startswith(("+", "-")):
            changed.append(line[1:])
    return "\n".join(changed)


def active_approvers(reviews, head_sha):
    """Logins whose most recent verdict is an approval of the current head commit.

    An approval is bound to the commit it was submitted against, so a later push
    leaves it behind and it stops counting.
    """
    verdicts = {}
    for review in reviews:
        state = (review.get("state") or "").upper()
        if state in NON_VERDICT_REVIEW_STATES:
            continue
        user = (review.get("user") or {}).get("login")
        if user:
            verdicts[user.lower()] = (state, review.get("commit_id"))
    return {
        user
        for user, (state, commit) in verdicts.items()
        if state == "APPROVED" and commit == head_sha
    }


def keyword_hits(pattern, text):
    return {m.group(0).lower() for m in pattern.finditer(text or "")}


def describe_source(in_diff, in_body):
    if in_diff and in_body:
        return "diff and PR body mention"
    return "PR body mentions" if in_body else "diff mentions"


def matches_prefix(path, prefixes):
    return any(path.startswith(prefix) for prefix in prefixes)


def required_reviews(paths, changed_diff, body=""):
    """The primary and expert reviews this PR requires, and what triggered them."""
    required = []

    claimed = set()
    for vertical in VERTICALS:
        touched = [p for p in paths if matches_prefix(p, vertical["prefixes"])]
        claimed.update(touched)
        if touched:
            required.append(
                {
                    "role": "primary",
                    "area": vertical["name"],
                    "trigger": f"changes under {', '.join(vertical['prefixes'])}",
                    "evidence": touched,
                    "reviewers": vertical["reviewers"],
                }
            )

    unclaimed = [p for p in paths if p not in claimed]
    if unclaimed:
        owned = ", ".join(pre for v in VERTICALS for pre in v["prefixes"])
        required.append(
            {
                "role": "primary",
                "area": FALLBACK_VERTICAL["name"],
                "trigger": f"changes outside {owned}",
                "evidence": unclaimed,
                "reviewers": FALLBACK_VERTICAL["reviewers"],
            }
        )

    haystack = changed_diff + "\n" + "\n".join(paths)
    for horizontal in HORIZONTALS:
        in_diff = keyword_hits(horizontal["pattern"], haystack)
        in_body = keyword_hits(horizontal["pattern"], body)
        hits = sorted(in_diff | in_body)
        if hits:
            required.append(
                {
                    "role": "expert",
                    "area": horizontal["name"],
                    "trigger": f"{describe_source(in_diff, in_body)} {', '.join(hits)}",
                    "evidence": [],
                    "reviewers": horizontal["reviewers"],
                }
            )

    return required


def evaluate(required, author, approvers):
    """Attach the involvement verdict to each required review."""
    involved = {author.lower()} | approvers
    for review in required:
        satisfied = [r for r in review["reviewers"] if r.lower() in involved]
        review["satisfied_by"] = satisfied
        review["satisfied"] = bool(satisfied)
    return required


def render(pr_number, author, approvers, required):
    lines = [f"# Required reviewers — PR #{pr_number}", ""]
    lines.append(f"- **Author:** @{author}")
    approver_list = (
        ", ".join(f"@{a}" for a in sorted(approvers)) if approvers else "_none_"
    )
    lines.append(f"- **Approving reviewers (current head):** {approver_list}")
    lines.append("")

    if not required:
        lines.append("This change requires no reviewers.")
        return "\n".join(lines) + "\n"

    lines.append("| | Area | Review | Trigger | Required (any one) | Satisfied by |")
    lines.append("|---|---|---|---|---|---|")
    for review in required:
        icon = "✅" if review["satisfied"] else "❌"
        reviewers = ", ".join(f"@{r}" for r in review["reviewers"])
        got = (
            ", ".join(f"@{r}" for r in review["satisfied_by"])
            if review["satisfied"]
            else "—"
        )
        lines.append(
            f"| {icon} | {review['area']} | {review['role']} | "
            f"{review['trigger']} | {reviewers} | {got} |"
        )
    lines.append("")

    for review in required:
        if review["satisfied"] or not review["evidence"]:
            continue
        shown = review["evidence"][:10]
        lines.append(
            f"<details><summary>Files in the {review['area']} vertical</summary>"
        )
        lines.append("")
        lines += [f"- `{path}`" for path in shown]
        if len(review["evidence"]) > len(shown):
            lines.append(f"- …and {len(review['evidence']) - len(shown)} more")
        lines.append("")
        lines.append("</details>")
        lines.append("")

    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pr", type=int, help="pull request number")
    parser.add_argument(
        "--repo",
        default=os.environ.get("GITHUB_REPOSITORY"),
        help="OWNER/REPO (defaults to $GITHUB_REPOSITORY)",
    )
    args = parser.parse_args()

    if not args.repo:
        parser.error("--repo is required when $GITHUB_REPOSITORY is unset")

    try:
        pull = gh_api_json(f"pulls/{args.pr}", args.repo)
        files = gh_api_json(
            f"pulls/{args.pr}/files?per_page=100", args.repo, paginate=True
        )
        reviews = gh_api_json(
            f"pulls/{args.pr}/reviews?per_page=100", args.repo, paginate=True
        )
    except CheckError as error:
        print(f"::error::{error}", file=sys.stderr)
        return 2

    author = (pull.get("user") or {}).get("login") or ""
    approvers = active_approvers(reviews, (pull.get("head") or {}).get("sha"))
    paths = changed_paths(files)
    required = evaluate(
        required_reviews(
            paths, diff_text(args.pr, args.repo, files), pull.get("body") or ""
        ),
        author,
        approvers,
    )

    report = render(args.pr, author, approvers, required)
    print(report)

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8") as handle:
            handle.write(report)

    unmet = [r for r in required if not r["satisfied"]]
    for review in unmet:
        reviewers = ", ".join(f"@{r}" for r in review["reviewers"])
        article = "an" if review["role"] == "expert" else "a"
        print(
            f"::error::{review['area']} needs {article} {review['role']} review "
            f"({review['trigger']}) from one of: {reviewers}",
            file=sys.stderr,
        )
    return 1 if unmet else 0


if __name__ == "__main__":
    sys.exit(main())
