#!/usr/bin/env python3
"""Verify that the required maintainers were involved in a pull request.

"Involved" means the maintainer either authored the PR or has an active
approving review on it. Used by .github/workflows/required_reviewers.yml;
safe to run locally against any open PR for spot-testing.

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

CLI_PREFIXES = ("src/cpp/cli/",)
APP_PREFIXES = ("src/app/",)

CLI_MAINTAINERS = ("bitgamma",)
APP_MAINTAINERS = ("kpoineal",)
CORE_MAINTAINERS = ("jeremyfowers", "ramkrishna2910")
NETWORK_MAINTAINERS = ("Geramy",)
ROCM_MAINTAINERS = ("superm1",)

NETWORK_KEYWORDS = ("http", "curl", "tcp", "udp", "cors")
ROCM_KEYWORDS = ("rocm",)

# Additive rules: each matching group adds a required reviewer on top of
# whatever the changed paths already require.
KEYWORD_RULES = (
    {
        "pattern": re.compile(
            "|".join(re.escape(k) for k in NETWORK_KEYWORDS), re.IGNORECASE
        ),
        "maintainers": NETWORK_MAINTAINERS,
    },
    {
        "pattern": re.compile(
            "|".join(re.escape(k) for k in ROCM_KEYWORDS), re.IGNORECASE
        ),
        "maintainers": ROCM_MAINTAINERS,
    },
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


def active_approvers(reviews):
    """Logins whose most recent verdict on the PR is an approval."""
    verdicts = {}
    for review in reviews:
        state = (review.get("state") or "").upper()
        if state in NON_VERDICT_REVIEW_STATES:
            continue
        user = (review.get("user") or {}).get("login")
        if user:
            verdicts[user.lower()] = state
    return {user for user, state in verdicts.items() if state == "APPROVED"}


def matches_prefix(path, prefixes):
    return any(path.startswith(prefix) for prefix in prefixes)


def build_requirements(paths, changed_diff):
    """Which maintainer groups this PR needs, and why."""
    requirements = []

    cli_paths = [p for p in paths if matches_prefix(p, CLI_PREFIXES)]
    if cli_paths:
        requirements.append(
            {
                "reason": f"changes under {', '.join(CLI_PREFIXES)}",
                "evidence": cli_paths,
                "maintainers": CLI_MAINTAINERS,
            }
        )

    app_paths = [p for p in paths if matches_prefix(p, APP_PREFIXES)]
    if app_paths:
        requirements.append(
            {
                "reason": f"changes under {', '.join(APP_PREFIXES)}",
                "evidence": app_paths,
                "maintainers": APP_MAINTAINERS,
            }
        )

    scoped = CLI_PREFIXES + APP_PREFIXES
    other_paths = [p for p in paths if not matches_prefix(p, scoped)]
    if other_paths:
        requirements.append(
            {
                "reason": f"changes outside {', '.join(scoped)}",
                "evidence": other_paths,
                "maintainers": CORE_MAINTAINERS,
            }
        )

    haystack = changed_diff + "\n" + "\n".join(paths)
    for rule in KEYWORD_RULES:
        keywords = sorted(
            {m.group(0).lower() for m in rule["pattern"].finditer(haystack)}
        )
        if keywords:
            requirements.append(
                {
                    "reason": f"diff mentions {', '.join(keywords)}",
                    "evidence": [],
                    "maintainers": rule["maintainers"],
                }
            )

    return requirements


def evaluate(requirements, author, approvers):
    """Attach the involvement verdict to each requirement."""
    involved = {author.lower()} | approvers
    for requirement in requirements:
        satisfied = [m for m in requirement["maintainers"] if m.lower() in involved]
        requirement["satisfied_by"] = satisfied
        requirement["satisfied"] = bool(satisfied)
    return requirements


def render(pr_number, author, approvers, requirements):
    lines = [f"# Maintainer involvement — PR #{pr_number}", ""]
    lines.append(f"- **Author:** @{author}")
    approver_list = (
        ", ".join(f"@{a}" for a in sorted(approvers)) if approvers else "_none_"
    )
    lines.append(f"- **Approving reviewers:** {approver_list}")
    lines.append("")

    if not requirements:
        lines.append("No maintainer involvement is required for this change.")
        return "\n".join(lines) + "\n"

    lines.append("| | Trigger | Required (any one) | Involved |")
    lines.append("|---|---|---|---|")
    for requirement in requirements:
        icon = "✅" if requirement["satisfied"] else "❌"
        required = ", ".join(f"@{m}" for m in requirement["maintainers"])
        got = (
            ", ".join(f"@{m}" for m in requirement["satisfied_by"])
            if requirement["satisfied"]
            else "—"
        )
        lines.append(f"| {icon} | {requirement['reason']} | {required} | {got} |")
    lines.append("")

    for requirement in requirements:
        if requirement["satisfied"] or not requirement["evidence"]:
            continue
        shown = requirement["evidence"][:10]
        lines.append(
            f"<details><summary>Files triggering: {requirement['reason']}</summary>"
        )
        lines.append("")
        lines += [f"- `{path}`" for path in shown]
        if len(requirement["evidence"]) > len(shown):
            lines.append(f"- …and {len(requirement['evidence']) - len(shown)} more")
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
    approvers = active_approvers(reviews)
    paths = changed_paths(files)
    requirements = evaluate(
        build_requirements(paths, diff_text(args.pr, args.repo, files)),
        author,
        approvers,
    )

    report = render(args.pr, author, approvers, requirements)
    print(report)

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8") as handle:
            handle.write(report)

    unmet = [r for r in requirements if not r["satisfied"]]
    for requirement in unmet:
        required = ", ".join(f"@{m}" for m in requirement["maintainers"])
        print(
            f"::error::{requirement['reason']} — needs an approving review from "
            f"one of: {required}",
            file=sys.stderr,
        )
    return 1 if unmet else 0


if __name__ == "__main__":
    sys.exit(main())
