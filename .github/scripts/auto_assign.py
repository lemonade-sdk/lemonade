#!/usr/bin/env python3
"""Suggest (and optionally request) reviewers for open PRs from their diffs.

Only considers non-draft PRs with no assignee, no requested reviewer, and no
human review yet. Candidates are repo collaborators with write access; the PR
author and bots are excluded. Scoring:

  - file history: who committed to the changed files recently (exponential
    decay), weighted by each file's share of the diff; new files fall back to
    their nearest directory with history
  - CODEOWNERS: share of changed files each owner covers
  - WG lead: small boost for the lead of the PR's workgroup
  - load: score divided by (1 + open review requests/assignments / 10)

Usage:
    python .github/scripts/auto_assign.py [--repo OWNER/REPO] [--ref REF]
        [--json PATH] [--wg-predictions PATH] [--apply] [--max N]

Requirements:
    - gh CLI authenticated with a token that can list collaborators
    - full git history of the default branch (fetch-depth: 0 in CI)
    - Python 3.9+ (stdlib only)
"""

import argparse
import fnmatch
import json
import math
import re
import subprocess
import sys
import time
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from triage_maintainer import WG_LEADS, load_wg_predictions, resolve_wg  # noqa: E402

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

HISTORY_DAYS = 180
DECAY_DAYS = 90
MAX_REVIEWERS = 2
SECOND_REVIEWER_RATIO = 0.5
MIN_SCORE = 0.15
CODEOWNER_WEIGHT = 0.5
WG_LEAD_WEIGHT = 0.2
LOAD_DIVISOR = 10
NOREPLY_RE = re.compile(r"^(?:\d+\+)?([A-Za-z0-9-]+)@users\.noreply\.github\.com$")


def run(cmd):
    return subprocess.run(
        cmd, check=True, capture_output=True, text=True, encoding="utf-8"
    ).stdout


def gh_json(args, attempts=3):
    # Large `gh pr list --json files` queries intermittently fail upstream.
    for i in range(attempts):
        try:
            return json.loads(run(["gh", *args]))
        except subprocess.CalledProcessError as exc:
            if i == attempts - 1:
                sys.exit(f"gh {' '.join(args[:2])} failed: {exc.stderr.strip()}")
            print(f"  [retry] {exc.stderr.strip()[:200]}", file=sys.stderr)
            time.sleep(5 * (i + 1))


def is_bot(login):
    return not login or login.endswith("[bot]")


def write_collaborators(repo):
    pages = gh_json(
        [
            "api",
            "--paginate",
            "--slurp",
            f"repos/{repo}/collaborators?per_page=100&affiliation=all",
        ]
    )
    return {
        c["login"]
        for page in pages
        for c in page
        if c.get("permissions", {}).get("push") and not is_bot(c["login"])
    }


def file_history(repo, ref):
    """Return {path: [(login, age_days)]} for commits on `ref` in the window."""
    log = run(
        [
            "git",
            "log",
            ref,
            "--no-merges",
            f"--since={HISTORY_DAYS} days ago",
            "--name-only",
            "--format=@@%H\t%ae\t%ct",
        ]
    )
    now = datetime.now(timezone.utc).timestamp()
    commits = []
    for line in log.splitlines():
        if line.startswith("@@"):
            sha, email, ts = line[2:].split("\t")
            commits.append((sha, email.lower(), (now - int(ts)) / 86400, []))
        elif line.strip() and commits:
            commits[-1][3].append(line.strip())

    login_by_email = {}
    for sha, email, _, _ in commits:
        if email in login_by_email:
            continue
        m = NOREPLY_RE.match(email)
        if m:
            login_by_email[email] = m.group(1)
            continue
        try:
            commit = json.loads(run(["gh", "api", f"repos/{repo}/commits/{sha}"]))
            login_by_email[email] = (commit.get("author") or {}).get("login")
        except subprocess.CalledProcessError:
            login_by_email[email] = None

    history = defaultdict(list)
    for _, email, age, paths in commits:
        login = login_by_email.get(email)
        if not login or is_bot(login):
            continue
        for path in paths:
            history[path].append((login, age))
            parts = path.split("/")
            for i in range(1, len(parts)):
                history["/".join(parts[:i]) + "/"].append((login, age))
    return history


def parse_codeowners():
    for candidate in (".github/CODEOWNERS", "CODEOWNERS", "docs/CODEOWNERS"):
        p = Path(candidate)
        if p.exists():
            break
    else:
        return []
    rules = []
    for line in p.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        pattern, *owners = line.split()
        logins = [o.lstrip("@") for o in owners if o.startswith("@") and "/" not in o]
        rules.append((pattern, logins))
    return rules


def codeowners_for(path, rules):
    owners = []
    for pattern, logins in rules:
        anchored = pattern.startswith("/")
        pat = pattern.lstrip("/")
        if pat.endswith("/"):
            hit = path.startswith(pat) if anchored else f"/{pat}" in f"/{path}"
        elif "/" in pat or anchored:
            hit = fnmatch.fnmatch(path, pat) or path.startswith(pat + "/")
        else:
            hit = fnmatch.fnmatch(path.rsplit("/", 1)[-1], pat)
        if hit:
            owners = logins
    return owners


def history_for(path, history):
    if path in history:
        return history[path]
    parts = path.split("/")
    for i in range(len(parts) - 1, 0, -1):
        key = "/".join(parts[:i]) + "/"
        if key in history:
            return history[key]
    return []


def wg_lead_logins(wg):
    return re.findall(r"@([A-Za-z0-9-]+)", WG_LEADS.get(wg, "")) if wg else []


def score_pr(pr, history, rules, eligible, load):
    author = pr["author"]["login"]
    files = pr["files"]
    churn = [max(f["additions"] + f["deletions"], 1) for f in files]
    total = sum(churn) or 1

    scores = defaultdict(float)
    touched = defaultdict(int)
    owned = defaultdict(int)
    for f, c in zip(files, churn):
        share = c / total
        decayed = defaultdict(float)
        for login, age in history_for(f["path"], history):
            decayed[login] += math.exp(-age / DECAY_DAYS)
        norm = sum(decayed.values())
        for login, d in decayed.items():
            scores[login] += share * d / norm
            touched[login] += 1
        for login in codeowners_for(f["path"], rules):
            scores[login] += CODEOWNER_WEIGHT * share
            owned[login] += 1

    for login in wg_lead_logins(pr["wg"]):
        scores[login] += WG_LEAD_WEIGHT

    # MIN_SCORE gates on expertise; load only reorders qualified candidates.
    ranked = []
    for login, s in scores.items():
        if login == author or login not in eligible or s < MIN_SCORE:
            continue
        adjusted = s / (1 + load.get(login, 0) / LOAD_DIVISOR)
        why = []
        if touched[login]:
            why.append(f"history on {touched[login]}/{len(files)} files")
        if owned[login]:
            why.append(f"CODEOWNER of {owned[login]}/{len(files)} files")
        if login in wg_lead_logins(pr["wg"]):
            why.append(f"{pr['wg']} lead")
        if load.get(login):
            why.append(f"load {load[login]}")
        ranked.append(
            {"login": login, "score": round(adjusted, 3), "why": " · ".join(why)}
        )
    ranked.sort(key=lambda r: -r["score"])

    picks = ranked[:1]
    if (
        len(ranked) > 1
        and ranked[1]["score"] >= SECOND_REVIEWER_RATIO * ranked[0]["score"]
    ):
        picks.append(ranked[1])
    return picks[:MAX_REVIEWERS]


def is_unrouted(pr, eligible):
    if pr["assignees"] or pr["reviewRequests"]:
        return False
    author = pr["author"]["login"]
    return not any(
        r["author"]["login"] != author and r["author"]["login"] in eligible
        for r in pr["latestReviews"]
    )


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--repo", default="lemonade-sdk/lemonade", help="OWNER/REPO")
    p.add_argument("--ref", default="origin/main", help="git ref for file history")
    p.add_argument("--json", help="Write suggestions to this path")
    p.add_argument("--wg-predictions", help="Same file triage_maintainer.py takes")
    p.add_argument("--apply", action="store_true", help="Request the reviewers")
    p.add_argument("--max", type=int, default=20, help="Max PRs to apply per run")
    args = p.parse_args()

    print("Fetching collaborators…", file=sys.stderr)
    eligible = write_collaborators(args.repo)
    print(f"  {len(eligible)} with write access", file=sys.stderr)

    print("Indexing file history…", file=sys.stderr)
    history = file_history(args.repo, args.ref)
    rules = parse_codeowners()

    print("Fetching open PRs…", file=sys.stderr)
    prs = gh_json(
        [
            "pr",
            "list",
            "--repo",
            args.repo,
            "--state",
            "open",
            "--limit",
            "500",
            "--json",
            "number,title,author,isDraft,labels,body,files,assignees,"
            "reviewRequests,latestReviews",
        ]
    )
    load = defaultdict(int)
    for pr in prs:
        people = {a["login"] for a in pr["assignees"]}
        people |= {r["login"] for r in pr["reviewRequests"] if r.get("login")}
        for login in people:
            load[login] += 1

    wg_predictions = load_wg_predictions(args.wg_predictions)
    candidates = [pr for pr in prs if not pr["isDraft"] and is_unrouted(pr, eligible)]
    print(f"  {len(candidates)} of {len(prs)} open PRs are unrouted", file=sys.stderr)

    suggestions = {}
    for pr in sorted(candidates, key=lambda pr: pr["number"]):
        labels = [lbl["name"] for lbl in pr["labels"]]
        pr["wg"] = resolve_wg(labels, pr["body"], wg_predictions.get(pr["number"]))[0]
        picks = score_pr(pr, history, rules, eligible, load)
        for r in picks:
            load[r["login"]] += 1
        suggestions[pr["number"]] = picks
        names = ", ".join(f"@{r['login']} ({r['why']})" for r in picks) or "(none)"
        print(f"#{pr['number']} {pr['title'][:60]}\n    → {names}")

    if args.json:
        Path(args.json).write_text(json.dumps(suggestions, indent=2), encoding="utf-8")
        print(f"Wrote {args.json}", file=sys.stderr)

    if args.apply:
        applied = failed = 0
        for num, picks in suggestions.items():
            if not picks or applied >= args.max:
                continue
            cmd = ["gh", "api", "-X", "POST"]
            cmd += [f"repos/{args.repo}/pulls/{num}/requested_reviewers"]
            for r in picks:
                cmd += ["-f", f"reviewers[]={r['login']}"]
            reviewers = ", ".join(f"@{r['login']}" for r in picks)
            try:
                run(cmd)
            except subprocess.CalledProcessError as exc:
                failed += 1
                print(f"  [warn] #{num}: {exc.stderr.strip()[:200]}", file=sys.stderr)
                continue
            print(f"  requested {reviewers} on #{num}", file=sys.stderr)
            applied += 1
        print(
            f"Requested reviewers on {applied} PRs ({failed} failed)", file=sys.stderr
        )
        if failed:
            sys.exit(1)


if __name__ == "__main__":
    main()
