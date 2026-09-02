#!/usr/bin/env python3
"""Work out which reviewers a pull request requires, and whether it has them.

A PR needs a *primary* review from the maintainer of each vertical it touches,
plus an *expert* review from the maintainer of each horizontal its diff or PR
body invokes. Expert reviews are additive, and never stand in for a primary
one.
A reviewer counts as satisfied when they authored the PR or have an approving
review on its current head commit. Any push after an approval invalidates it.
Used by .github/workflows/required_reviewers.yml; safe to run locally against
any open PR for spot-testing.

Usage:
    python .github/scripts/check_maintainer_involvement.py <pr> [--repo OWNER/REPO]

Requirements:
    - gh CLI authenticated (GH_TOKEN env var works in CI)
    - Python 3.9+ (stdlib only, no external deps)
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
        "keywords": ("curl", "tcp", "udp", "cors", "security"),
        "reviewers": ("Geramy",),
    },
    {
        "name": "ROCm",
        "keywords": ("rocm",),
        "reviewers": ("superm1",),
    },
)

# A keyword only counts as its own token. Bare \b would reject tcp_port and
# cors_config, which are the names worth catching; letters and digits alone are
# what turn readsecurity and -iTCP:13305 into false triggers.
for _horizontal in HORIZONTALS:
    _horizontal["pattern"] = re.compile(
        "(?<![A-Za-z0-9])(?:"
        + "|".join(re.escape(k) for k in _horizontal["keywords"])
        + ")(?![A-Za-z0-9])",
        re.IGNORECASE,
    )

# Reviews in these states say nothing about whether the reviewer approves,
# so they never displace an earlier verdict from the same person.
NON_VERDICT_REVIEW_STATES = ("COMMENTED", "PENDING")


# Identifies this workflow's own comment so each run edits it in place instead
# of stacking a new one onto the PR.
COMMENT_MARKER = "<!-- required-reviewers-check -->"


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


def gh_api_write(path, repo, method, payload):
    cmd = [
        "gh",
        "api",
        "--method",
        method,
        "-H",
        "X-GitHub-Api-Version: 2022-11-28",
        f"/repos/{repo}/{path}",
        "--input",
        "-",
    ]
    result = subprocess.run(
        cmd, input=json.dumps(payload), capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        raise CheckError(f"gh api {method} {path} failed: {result.stderr.strip()}")
    return json.loads(result.stdout) if result.stdout.strip() else {}


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


def review_states(reviews, head_sha):
    """Each reviewer's latest verdict, as (state, commit_id).

    COMMENTED and PENDING carry no verdict, so they never displace an earlier one.
    """
    verdicts = {}
    for review in reviews:
        state = (review.get("state") or "").upper()
        user = (review.get("user") or {}).get("login")
        if not user:
            continue
        if state in NON_VERDICT_REVIEW_STATES:
            verdicts.setdefault(user.lower(), ("COMMENTED", None))
            continue
        verdicts[user.lower()] = (state, review.get("commit_id"))
    return verdicts


def active_approvers(reviews, head_sha):
    """Logins whose most recent verdict is an approval of the current head commit.

    An approval is bound to the commit it was submitted against, so a later push
    leaves it behind and it stops counting.
    """
    return {
        user
        for user, (state, commit) in review_states(reviews, head_sha).items()
        if state == "APPROVED" and commit == head_sha
    }


def reviewer_status(login, states, head_sha):
    """Plain-language account of where one reviewer stands."""
    state, commit = states.get(login.lower(), (None, None))
    if state == "APPROVED" and commit == head_sha:
        return "approved"
    if state == "APPROVED":
        short = (commit or "")[:7]
        return f"approved an older commit ({short}); needs to approve again"
    if state == "CHANGES_REQUESTED":
        return "requested changes"
    if state == "DISMISSED":
        return "their approval was dismissed"
    if state == "COMMENTED":
        return "commented, but has not clicked Approve"
    return "has not reviewed yet"


def keyword_hits(pattern, text):
    return {m.group(0).lower() for m in pattern.finditer(text or "")}


def describe_source(in_diff, in_body):
    if in_diff and in_body:
        return "diff and description"
    return "description" if in_body else "diff"


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
                    "trigger": "changes files under "
                    + " and ".join(f"`{p}`" for p in vertical["prefixes"]),
                    "evidence": touched,
                    "reviewers": vertical["reviewers"],
                }
            )

    unclaimed = [p for p in paths if p not in claimed]
    if unclaimed:
        owned = " and ".join(f"`{pre}`" for v in VERTICALS for pre in v["prefixes"])
        required.append(
            {
                "role": "primary",
                "area": FALLBACK_VERTICAL["name"],
                "trigger": f"changes files outside {owned}",
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
                    "trigger": f"mentions {', '.join(hits)} in its "
                    + describe_source(in_diff, in_body),
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


def action_line(review, states, head_sha):
    """One instruction a reader can act on without knowing the policy.

    Whatever the required reviewers have already done shapes the ask, so the
    reader never has to reconcile the instruction against a separate status.
    """
    names = " or ".join(f"@{r}" for r in review["reviewers"])
    verdict = lambda r: states.get(r.lower(), (None, None))

    stale = [r for r in review["reviewers"] if verdict(r)[0] == "APPROVED"]
    if stale:
        who = " or ".join(f"@{r}" for r in stale)
        commit = (verdict(stale[0])[1] or "")[:7]
        return (
            f"Ask {who} to approve again. They already approved this PR at commit "
            f"{commit}, but it has since been updated to {head_sha[:7]}, and an "
            f"approval only counts on the newest commit."
        )

    blocked = [r for r in review["reviewers"] if verdict(r)[0] == "CHANGES_REQUESTED"]
    if blocked:
        who = " and ".join(f"@{r}" for r in blocked)
        return (
            f"{who} requested changes on this PR. Address the feedback, then ask "
            f"for a fresh review."
        )

    dismissed = [r for r in review["reviewers"] if verdict(r)[0] == "DISMISSED"]
    if dismissed:
        who = " or ".join(f"@{r}" for r in dismissed)
        return f"{who} had an approval dismissed on this PR. Ask them to approve again."

    talked = [r for r in review["reviewers"] if verdict(r)[0] == "COMMENTED"]
    if talked:
        who = " or ".join(f"@{r}" for r in talked)
        return (
            f"{who} has commented but not approved. Ask them to submit a review with "
            f"Approve selected."
        )

    wait = "wait for one of them" if len(review["reviewers"]) > 1 else "wait for them"
    return f"Request a review from {names}, and {wait} to approve."


def render(pr_number, author, head_sha, states, required):
    """The report body.

    GitHub renders a single newline inside a comment as a line break, so every
    paragraph is emitted as one line and left to wrap on the reader's screen.
    """
    unmet = [r for r in required if not r["satisfied"]]
    met = [r for r in required if r["satisfied"]]

    if not unmet:
        lines = [
            "# Required reviewers: all set",
            "",
            "This PR has every approval this repository requires of it.",
            "",
        ]
    else:
        count = len(unmet)
        touches = "one such part" if count == 1 else f"{count} such parts"
        lines = [
            f"# Required reviewers: {count} still needed",
            "",
            "Some parts of this repository can only be changed with approval from a"
            f" specific maintainer. This PR touches {touches}, so it cannot merge yet.",
            "",
            "## What to do",
            "",
        ]
        for i, review in enumerate(unmet, 1):
            lines.append(f"{i}. {action_line(review, states, head_sha)}")
            lines.append("")
            detail = f"   Required because this PR {review['trigger']}."
            if review["evidence"]:
                shown = review["evidence"][:5]
                rest = len(review["evidence"]) - len(shown)
                files = ", ".join(f"`{path}`" for path in shown)
                if rest:
                    files += f", and {rest} more"
                detail += f" Files: {files}"
            lines.append(detail)
            lines.append("")
        lines.append(
            "Only a review submitted as **Approve** counts, not a comment. Pushing a"
            " new commit clears approvals given before it, so ask for approvals once"
            " the branch has settled."
        )
        lines.append("")

    if met:
        lines.append("## Already covered")
        lines.append("")
        for review in met:
            by = review["satisfied_by"]
            who = (
                "you wrote this PR"
                if author.lower() in {b.lower() for b in by}
                else "approved by @" + ", @".join(by)
            )
            trigger = review["trigger"][0].upper() + review["trigger"][1:]
            lines.append(f"- {trigger}: {who}.")
        lines.append("")

    return "\n".join(lines) + "\n"


def upsert_comment(pr_number, repo, body):
    """Post the report to the PR, replacing this check's previous comment."""
    payload = {"body": f"{COMMENT_MARKER}\n{body}"}
    existing = gh_api_json(
        f"issues/{pr_number}/comments?per_page=100", repo, paginate=True
    )
    for comment in existing:
        if (comment.get("body") or "").startswith(COMMENT_MARKER):
            gh_api_write(f"issues/comments/{comment['id']}", repo, "PATCH", payload)
            return "updated"
    gh_api_write(f"issues/{pr_number}/comments", repo, "POST", payload)
    return "created"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pr", type=int, help="pull request number")
    parser.add_argument(
        "--comment",
        action="store_true",
        help="post the report to the PR, replacing this check's previous comment",
    )
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
    head_sha = (pull.get("head") or {}).get("sha") or ""
    approvers = active_approvers(reviews, head_sha)
    paths = changed_paths(files)
    required = evaluate(
        required_reviews(
            paths, diff_text(args.pr, args.repo, files), pull.get("body") or ""
        ),
        author,
        approvers,
    )

    states = review_states(reviews, head_sha)
    report = render(args.pr, author, head_sha, states, required)
    print(report)

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8") as handle:
            handle.write(report)

    unmet = [r for r in required if not r["satisfied"]]

    if args.comment:
        try:
            print(f"Comment {upsert_comment(args.pr, args.repo, report)} on the PR.")
        except CheckError as error:
            print(f"::warning::Could not post the PR comment: {error}", file=sys.stderr)

    if unmet:
        count = len(unmet)
        noun = "approval" if count == 1 else "approvals"
        where = (
            "See the Required reviewers comment on the pull request for what to do."
            if args.comment
            else "See the job summary below for what to do."
        )
        print(
            f"::error::{count} {noun} still needed before this PR can merge. {where}",
            file=sys.stderr,
        )
    return 1 if unmet else 0


if __name__ == "__main__":
    sys.exit(main())
