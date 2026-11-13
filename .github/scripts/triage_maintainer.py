#!/usr/bin/env python3
"""Generate a maintainer-focused triage page for lemonade-sdk/lemonade.

Single filterable view of all open PRs with controls for:
  - Workgroup (dropdown)
  - Contributor type (Maintainer / First-timer toggles)
  - Staleness bucket
All filters AND together; total always visible.

Usage:
    python .github/scripts/triage_maintainer.py [--output PATH] [--repo OWNER/REPO]
        [--wg-predictions PATH] [--reviewer-suggestions PATH] [--include-rfcs]

Requirements:
    - gh CLI authenticated
    - Python 3.9+ (stdlib only)
"""

import argparse
import json
import re
import subprocess
import sys
import urllib.parse
from datetime import datetime, timedelta, timezone
from pathlib import Path

HOT_LABEL = "priority::\U0001f525hot"
WARM_LABEL = "priority::\U0001f60ewarm"

WRITE_ASSOCIATIONS = {"OWNER", "MEMBER", "COLLABORATOR"}

ACTIVE_WGS = [
    "Auto-Tune",
    "Cross-Vendor Support",
    "Cloud Hybrid",
    "Remote Use",
    "GUI App",
    "Enterprise Grade",
    "Platform Core",
]

WG_LEADS = {
    "Auto-Tune": "@bitgamma",
    "Cross-Vendor Support": "@kenvandine",
    "Cloud Hybrid": "@ramkrishna2910",
    "Remote Use": "@Geramy",
    "GUI App": "@kpoineal",
    "Enterprise Grade": "@jeremyfowers",
    "Platform Core": "@superm1 / @jeremyfowers",
}

ADMIN_MAINTAINERS = {
    "jeremyfowers",
    "kenvandine",
    "ramkrishna2910",
    "superm1",
    "bitgamma",
    "kpoineal",
    "Geramy",
}

# Non-admin contributors with a named role (WG leads, code owners, key reviewers).
# These are community members who get elevated visibility — not maintainers.
KEY_COMMUNITY = {
    "abn",
    "eddierichter-amd",
    "fl0rianr",
    "pwilkin",
    "sawansri",
    "siavashhub",
    "SlawomirNowaczyk",
    "sofiageo",
    "valiabhay",
}

# All named contributors from docs/dev/contribute.md#maintainers
GUIDE_CONTRIBUTORS = ADMIN_MAINTAINERS | KEY_COMMUNITY

CANONICAL_WGS = {
    "auto-tune": "Auto-Tune",
    "autotune": "Auto-Tune",
    "auto tune": "Auto-Tune",
    "cross-vendor support": "Cross-Vendor Support",
    "cross-vendor": "Cross-Vendor Support",
    "cross vendor support": "Cross-Vendor Support",
    "cross vendor": "Cross-Vendor Support",
    "cloud hybrid": "Cloud Hybrid",
    "remote use": "Remote Use",
    "gui app": "GUI App",
    "enterprise grade": "Enterprise Grade",
    "platform-core": "Platform Core",
    "platform core": "Platform Core",
    "platformcore": "Platform Core",
}


def gh_api(path):
    cmd = ["gh", "api", path]
    out = subprocess.run(
        cmd, check=True, capture_output=True, text=True, encoding="utf-8"
    ).stdout
    return json.loads(out)


def gh_api_pages(path):
    cmd = ["gh", "api", "--paginate", "--slurp", path]
    out = subprocess.run(
        cmd, check=True, capture_output=True, text=True, encoding="utf-8"
    ).stdout
    pages = json.loads(out)
    return [item for page in pages for item in page]


def gh_graphql(query, **variables):
    cmd = ["gh", "api", "graphql", "-f", f"query={query}"]
    for key, value in variables.items():
        if value is not None:
            cmd += ["-F", f"{key}={value}"]
    out = subprocess.run(
        cmd, check=True, capture_output=True, text=True, encoding="utf-8"
    ).stdout
    return json.loads(out)["data"]


def search_count(repo, query):
    q = urllib.parse.quote(f"repo:{repo} {query}")
    return gh_api(f"/search/issues?q={q}&per_page=1")["total_count"]


def search_items(repo, query):
    """All issues/PRs matching a search query. The search API caps at 1000."""
    q = urllib.parse.quote(f"repo:{repo} {query}")
    items, page = [], 1
    while True:
        res = gh_api(f"/search/issues?q={q}&per_page=100&page={page}")
        items.extend(res["items"])
        if len(items) >= res["total_count"] or len(res["items"]) < 100:
            break
        if page == 10:
            print(f"  [warn] '{query}' exceeds 1000 results", file=sys.stderr)
            break
        page += 1
    return items


def parse_ts(s):
    return datetime.fromisoformat(s.replace("Z", "+00:00"))


# Bots relabel PRs constantly, which bumps updated_at; only these timeline
# events count as activity, and only when a human (or a pushed commit) did it.
_PR_ACTIVITY_QUERY = """
query($owner: String!, $name: String!, $cursor: String) {
  repository(owner: $owner, name: $name) {
    pullRequests(states: OPEN, first: 50, after: $cursor) {
      pageInfo { hasNextPage endCursor }
      nodes {
        number createdAt author { login }
        timelineItems(last: 40, itemTypes: [ISSUE_COMMENT, PULL_REQUEST_REVIEW,
            PULL_REQUEST_COMMIT, HEAD_REF_FORCE_PUSHED_EVENT, REOPENED_EVENT,
            READY_FOR_REVIEW_EVENT]) {
          nodes {
            __typename
            ... on IssueComment { createdAt author { login __typename } }
            ... on PullRequestReview { createdAt author { login __typename } }
            ... on PullRequestCommit { commit { committedDate } }
            ... on HeadRefForcePushedEvent { createdAt actor { __typename } }
            ... on ReopenedEvent { createdAt actor { __typename } }
            ... on ReadyForReviewEvent { createdAt actor { __typename } }
          }
        }
      }
    }
  }
}
"""


def fetch_pr_activity(repo):
    """Return {pr_number: {"last_activity": datetime, "engagement": int}}.

    engagement counts human comments and reviews by someone other than the
    author, so reviewed PRs aren't reported as unanswered.
    """
    owner, name = repo.split("/", 1)
    result, cursor = {}, None
    while True:
        conn = gh_graphql(_PR_ACTIVITY_QUERY, owner=owner, name=name, cursor=cursor)[
            "repository"
        ]["pullRequests"]
        for pr in conn["nodes"]:
            author = (pr.get("author") or {}).get("login")
            last = parse_ts(pr["createdAt"])
            engagement = 0
            for item in pr["timelineItems"]["nodes"]:
                kind = item["__typename"]
                if kind == "PullRequestCommit":
                    last = max(last, parse_ts(item["commit"]["committedDate"]))
                    continue
                who = item.get("author") or item.get("actor") or {}
                if who.get("__typename") == "Bot":
                    continue
                last = max(last, parse_ts(item["createdAt"]))
                if kind in ("IssueComment", "PullRequestReview") and (
                    who.get("login") != author
                ):
                    engagement += 1
            result[pr["number"]] = {"last_activity": last, "engagement": engagement}
        if not conn["pageInfo"]["hasNextPage"]:
            return result
        cursor = conn["pageInfo"]["endCursor"]


def fetch_top_contributors(repo, top_n=50):
    try:
        raw = gh_api_pages(f"/repos/{repo}/contributors?per_page=100")
        logins = {c["login"] for c in raw[:top_n] if c.get("login")}
        print(f"  fetched {len(logins)} top contributors", file=sys.stderr)
        return logins
    except Exception as exc:
        print(f"  [warn] contributors API failed ({exc})", file=sys.stderr)
        return set()


def community_tier(login, author_association, top_contributors):
    if login in KEY_COMMUNITY:
        return "wg-lead"
    if login in top_contributors:
        return "active"
    if author_association in ("FIRST_TIME_CONTRIBUTOR", "FIRST_TIMER"):
        return "first-timer"
    return "returning"


def declared_wg(body):
    m = re.search(r"scope of WG:\s*([^\n\r<]{1,60})", body or "", re.IGNORECASE)
    if not m:
        return None
    # Only accept known canonical WGs — rejects RFC/checkbox garbage
    return CANONICAL_WGS.get(m.group(1).strip().lower())


def mentioned_wg(body):
    body_lower = (body or "").lower()
    for key in sorted(CANONICAL_WGS, key=len, reverse=True):
        if key in body_lower:
            return CANONICAL_WGS[key]
    return None


def label_wg(labels):
    for name in labels:
        if name.startswith("wg::"):
            return CANONICAL_WGS.get(name[4:].replace("-", " ").lower())
    return None


def resolve_wg(labels, body, prediction):
    """Return (wg, source, reason). Explicit signals beat predictions;
    predictions beat a loose body mention."""
    if wg := label_wg(labels):
        return wg, "label", None
    if wg := declared_wg(body):
        return wg, "declared", None
    if prediction:
        return prediction[0], "predicted", prediction[1]
    if wg := mentioned_wg(body):
        return wg, "mentioned", None
    return None, None, None


def parse_rfc_status(labels):
    """Return RFC readiness from PR labels, or None."""
    for p in [
        "rfc:required",
        "rfc:on-roadmap",
        "rfc:open-for-comment",
        "rfc:not-required",
    ]:
        if p in labels:
            return p.replace("rfc:", "")
    return None


def load_wg_predictions(path):
    """Load {"<pr number>": ["<WG name>", "<reason>"]}, rejecting unknown WGs."""
    if not path:
        return {}
    raw = json.loads(Path(path).read_text(encoding="utf-8"))
    preds = {}
    for num, (wg, reason) in raw.items():
        if wg not in ACTIVE_WGS:
            sys.exit(f"{path}: PR #{num} has unknown WG {wg!r}")
        preds[int(num)] = (wg, reason)
    return preds


def fetch_prs(repo, top_contributors, wg_predictions):
    now = datetime.now(timezone.utc)
    # pulls API gives requested_reviewers; issues API does not
    raw_pulls = gh_api_pages(f"/repos/{repo}/pulls?state=open&per_page=100")
    requested_reviewers = {
        p["number"]: [
            r["login"] for r in p.get("requested_reviewers", []) if r and r.get("login")
        ]
        for p in raw_pulls
    }
    activity = fetch_pr_activity(repo)
    raw = gh_api_pages(f"/repos/{repo}/issues?state=open&per_page=100")
    prs = []
    for it in raw:
        if it.get("pull_request") is None:
            continue
        if it.get("draft"):
            continue
        act = activity.get(it["number"])
        last_activity = act["last_activity"] if act else parse_ts(it["created_at"])
        created = datetime.fromisoformat(it["created_at"].replace("Z", "+00:00"))
        login = (it.get("user") or {}).get("login", "?")
        assoc = it.get("author_association", "NONE")
        is_maint = assoc in WRITE_ASSOCIATIONS or login in ADMIN_MAINTAINERS
        body = it.get("body") or ""
        labels = [lbl["name"] for lbl in it.get("labels", [])]
        days_stale = (now - last_activity).days
        wg, wg_source, wg_reason = resolve_wg(
            labels, body, wg_predictions.get(it["number"])
        )
        prs.append(
            {
                "number": it["number"],
                "title": it["title"],
                "url": it["html_url"],
                "author": login,
                "labels": labels,
                "assignees": [
                    a["login"] for a in it.get("assignees", []) if a and a.get("login")
                ],
                "review_requested": requested_reviewers.get(it["number"], []),
                "comments": it.get("comments", 0),
                "engagement": act["engagement"] if act else 0,
                "created_at": it["created_at"],
                "updated_at": it["updated_at"],
                "last_activity": last_activity.strftime("%Y-%m-%dT%H:%M:%SZ"),
                "days_stale": days_stale,
                "days_old": (now - created).days,
                "wg": wg,
                "wg_source": wg_source,
                "wg_reason": wg_reason,
                "rfc_status": parse_rfc_status(labels),
                "is_maintainer": is_maint,
                "community_tier": (
                    None if is_maint else community_tier(login, assoc, top_contributors)
                ),
                "is_draft": bool(it.get("draft", False)),
            }
        )
        prs[-1]["priority_score"] = compute_priority_score(prs[-1])
    return prs


def fetch_activity(repo, days=30):
    """Daily opened / merged / closed-unmerged PR counts and the open count.

    Uses created:/closed: searches so the window isn't skewed by how recently
    a PR was last touched. The open count is rebuilt backwards from today.
    """
    today = datetime.now(timezone.utc).replace(
        hour=0, minute=0, second=0, microsecond=0
    )
    start = today - timedelta(days=days - 1)
    since = start.strftime("%Y-%m-%d")
    day_keys = [(start + timedelta(days=i)).strftime("%Y-%m-%d") for i in range(days)]
    opened = dict.fromkeys(day_keys, 0)
    merged = dict.fromkeys(day_keys, 0)
    closed_nm = dict.fromkeys(day_keys, 0)

    for pr in search_items(repo, f"is:pr created:>={since}"):
        d = pr["created_at"][:10]
        if d in opened:
            opened[d] += 1
    for pr in search_items(repo, f"is:pr closed:>={since}"):
        d = (pr.get("closed_at") or "")[:10]
        if d not in merged:
            continue
        if (pr.get("pull_request") or {}).get("merged_at"):
            merged[d] += 1
        else:
            closed_nm[d] += 1

    running, current = {}, search_count(repo, "is:pr is:open")
    for d in reversed(day_keys):
        running[d] = current
        current = max(current - opened[d] + merged[d] + closed_nm[d], 0)
    return [
        {
            "date": d,
            "opened": opened[d],
            "merged": merged[d],
            "closed_nm": closed_nm[d],
            "running_open": running[d],
        }
        for d in day_keys
    ]


def compute_priority_score(pr):
    """Score PRs by near-term landing potential.

    Priority: guide contributor + fresh + has reviewer = highest.
    Old stale PRs score low regardless of author — they need triage separately.
    """
    score = 0
    labels = set(pr["labels"])
    tier = pr.get("community_tier")
    author = pr.get("author", "")
    days_stale = pr["days_stale"]
    days_old = pr.get("days_old", 99)
    is_guide = pr.get("is_maintainer") or author in GUIDE_CONTRIBUTORS

    # Community-validated urgency
    if HOT_LABEL in labels:
        score += 1000
    elif WARM_LABEL in labels:
        score += 500

    # Blocked or correctness issue
    if "rfc:required" in labels:
        score += 400
    if "bug" in labels:
        score += 250

    # Contributor trust from contribute.md guide
    if is_guide:
        score += 500
        # Extra freshness boost: guide contributor + recently opened
        if days_old <= 7:
            score += 400  # very new from a trusted contributor
        elif days_old <= 21:
            score += 200
        elif days_old <= 45:
            score += 75
    elif tier == "active":
        score += 200
        if days_old <= 14:
            score += 100
    elif tier == "returning":
        score += 50
    elif tier == "first-timer":
        score -= 100

    # Staleness penalty — applies to everyone including guide contributors
    if days_stale >= 45:
        score -= 400
    elif days_stale >= 30:
        score -= 200
    elif days_stale >= 14:
        score -= 75
    elif days_stale <= 3:
        score += 100  # very fresh activity

    # Reviewer assigned = close to landing
    if pr.get("review_requested"):
        score += 150
    else:
        score -= 50

    score += min(pr["comments"], 15) * 5

    return score


_RFC_QUERY = """
query($owner: String!, $name: String!, $cursor: String) {
  repository(owner: $owner, name: $name) {
    discussions(first: 100, after: $cursor) {
      pageInfo { hasNextPage endCursor }
      nodes {
        number title url closed createdAt author { login }
        category { name }
        labels(first: 20) { nodes { name } }
        comments(last: 20) {
          totalCount
          nodes { createdAt replies(last: 1) { nodes { createdAt } } }
        }
      }
    }
  }
}
"""

RFC_STATUS_LABELS = ("rfc:on-roadmap", "rfc:not-planned", "rfc:open-for-comment")


def fetch_rfcs(repo):
    """Open RFC discussions, stalest first, with status from their rfc:* label."""
    now = datetime.now(timezone.utc)
    owner, name = repo.split("/", 1)
    out, cursor = [], None
    while True:
        conn = gh_graphql(_RFC_QUERY, owner=owner, name=name, cursor=cursor)[
            "repository"
        ]["discussions"]
        for d in conn["nodes"]:
            if d["closed"] or d["category"]["name"] != "Request for Comment (RFC)":
                continue
            labels = {lbl["name"] for lbl in d["labels"]["nodes"]}
            status = next(
                (lbl[4:] for lbl in RFC_STATUS_LABELS if lbl in labels), "unlabeled"
            )
            last = parse_ts(d["createdAt"])
            for c in d["comments"]["nodes"]:
                last = max(last, parse_ts(c["createdAt"]))
                for r in c["replies"]["nodes"]:
                    last = max(last, parse_ts(r["createdAt"]))
            out.append(
                {
                    "number": d["number"],
                    "title": d["title"],
                    "url": d["url"],
                    "author": (d.get("author") or {}).get("login") or "?",
                    "comments": d["comments"]["totalCount"],
                    "days_open": (now - parse_ts(d["createdAt"])).days,
                    "days_quiet": (now - last).days,
                    "rfc_status": status,
                }
            )
        if not conn["pageInfo"]["hasNextPage"]:
            break
        cursor = conn["pageInfo"]["endCursor"]
    out.sort(key=lambda x: x["days_quiet"], reverse=True)
    return out


def fetch_issue_trend(repo, days=90):
    """Return [{date, open_issues, open_prs}] for each day in the window.

    Rebuilt backwards from today's open counts using created:/closed: searches;
    a since= listing would drop items that were open but untouched.
    """
    today = datetime.now(timezone.utc).replace(
        hour=0, minute=0, second=0, microsecond=0
    )
    start = today - timedelta(days=days)
    since = start.strftime("%Y-%m-%d")
    day_list = [start + timedelta(days=i) for i in range(days + 1)]
    series = {}
    for kind in ("issue", "pr"):
        created = [
            parse_ts(i["created_at"])
            for i in search_items(repo, f"is:{kind} created:>={since}")
        ]
        closed = [
            parse_ts(i["closed_at"])
            for i in search_items(repo, f"is:{kind} closed:>={since}")
        ]
        open_now = search_count(repo, f"is:{kind} is:open")
        series[kind] = [
            open_now
            - sum(c >= day + timedelta(days=1) for c in created)
            + sum(c >= day + timedelta(days=1) for c in closed)
            for day in day_list
        ]
    return [
        {
            "date": day.strftime("%Y-%m-%d"),
            "open_issues": series["issue"][i],
            "open_prs": series["pr"][i],
        }
        for i, day in enumerate(day_list)
    ]


def fetch_weekly_pulse(repo, wg_predictions):
    """Return a quantitative summary of the past 7 days of activity."""
    now = datetime.now(timezone.utc)
    cutoff = now - timedelta(days=7)

    def parse(s):
        return datetime.fromisoformat(s.replace("Z", "+00:00"))

    # Merged PRs last 7 days — walk backwards through closed PRs
    merged_raw = []
    page = 1
    while True:
        cmd = [
            "gh",
            "api",
            f"repos/{repo}/pulls?state=closed&sort=updated&direction=desc&per_page=100&page={page}",
        ]
        try:
            batch = json.loads(
                subprocess.run(
                    cmd, check=True, capture_output=True, encoding="utf-8"
                ).stdout
            )
        except Exception:
            break
        if not batch:
            break
        merged_raw.extend(
            p for p in batch if p.get("merged_at") and parse(p["merged_at"]) >= cutoff
        )
        oldest = min((parse(p["updated_at"]) for p in batch), default=now)
        if oldest < cutoff:
            break
        page += 1

    # Open issues and open PRs (single paginated call each)
    issues_raw = gh_api_pages(
        f"/repos/{repo}/issues?state=open&sort=created&direction=desc&per_page=100"
    )
    all_open_issues = [i for i in issues_raw if not i.get("pull_request")]
    new_issues = [i for i in all_open_issues if parse(i["created_at"]) >= cutoff]
    new_bugs = [
        i for i in new_issues if any(l["name"] == "bug" for l in i.get("labels", []))
    ]

    open_prs_raw = gh_api_pages(
        f"/repos/{repo}/pulls?state=open&sort=created&direction=desc&per_page=100"
    )
    new_prs = [p for p in open_prs_raw if parse(p["created_at"]) >= cutoff]

    # WG breakdown of merged
    from collections import Counter

    wg_merged = Counter()
    for p in merged_raw:
        labels = [l["name"] for l in p.get("labels", [])]
        wg, _, _ = resolve_wg(
            labels, p.get("body") or "", wg_predictions.get(p["number"])
        )
        wg_merged[wg or "Unclassified"] += 1

    top_contributors = [
        {"login": login, "merged": count}
        for login, count in Counter(
            (p.get("user") or {}).get("login", "?") for p in merged_raw
        ).most_common(5)
    ]

    return {
        "generated_at": now.strftime("%Y-%m-%d %H:%M UTC"),
        "window_days": 7,
        "merged_count": len(merged_raw),
        "merged": [
            {
                "number": p["number"],
                "title": p["title"],
                "url": p["html_url"],
                "author": (p.get("user") or {}).get("login", "?"),
                "merged_at": p["merged_at"],
            }
            for p in sorted(
                merged_raw, key=lambda p: p["merged_at"] or "", reverse=True
            )
        ],
        "new_prs_count": len(new_prs),
        "open_prs_total": len(open_prs_raw),
        "new_issues_count": len(new_issues),
        "new_bugs_count": len(new_bugs),
        "open_issues_total": len(all_open_issues),
        "wg_merged": dict(wg_merged),
        "top_contributors": top_contributors,
    }


def build_focus_data(prs):
    """Return {wg: [top3 community PRs by priority_score]} for the focus panel."""
    from collections import defaultdict

    by_wg = defaultdict(list)
    for pr in prs:
        if (
            pr.get("wg")
            and not pr.get("is_draft")
            and pr.get("community_tier") != "first-timer"
        ):
            by_wg[pr["wg"]].append(pr)
    focus = {}
    for wg, wg_prs in by_wg.items():
        top = sorted(wg_prs, key=lambda p: p["priority_score"], reverse=True)[:3]
        focus[wg] = [
            {
                "number": p["number"],
                "title": p["title"],
                "url": p["url"],
                "author": p["author"],
                "score": p["priority_score"],
                "days_stale": p["days_stale"],
                "days_old": p.get("days_old", 0),
                "wg": p["wg"],
                "labels": p["labels"],
                "assignees": p["assignees"],
                "community_tier": p.get("community_tier"),
                "is_maintainer": p.get("is_maintainer", False),
                "rfc_status": p.get("rfc_status"),
                "review_requested": p.get("review_requested", []),
            }
            for p in top
        ]
    return focus


def script_safe_json(obj):
    ls = " "
    ps = " "
    return (
        json.dumps(obj)
        .replace("<", "\\u003c")
        .replace(">", "\\u003e")
        .replace("&", "\\u0026")
        .replace(ls, "\\u2028")
        .replace(ps, "\\u2029")
    )


MAINTAINER_TEMPLATE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Lemonade PR triage — __REPO__</title>
<style>
  :root {
    --bg:#0d1117; --panel:#161b22; --panel-2:#21262d; --border:#30363d;
    --fg:#e6edf3; --muted:#7d8590; --link:#58a6ff;
    --stale:#ffa657; --first:#56d364; --draft:#484f58;
    --hot:#b60205; --tier-lead:#ffd700; --tier-active:#79c0ff;
  }
  * { box-sizing:border-box; }
  body { font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",system-ui,sans-serif;
         margin:0; background:var(--bg); color:var(--fg); }
  a { color:var(--link); text-decoration:none; }
  a:hover { text-decoration:underline; }

  /* header */
  header { padding:14px 24px; border-bottom:1px solid var(--border);
           display:flex; justify-content:space-between; align-items:center;
           gap:16px; flex-wrap:wrap; }
  header h1 { margin:0; font-size:17px; font-weight:600; }
  .header-right { display:flex; align-items:center; gap:20px; flex-wrap:wrap; }
  .total-badge { font-size:28px; font-weight:700; color:var(--fg); line-height:1; }
  .total-label { font-size:11px; color:var(--muted); text-transform:uppercase;
                 letter-spacing:0.5px; margin-top:2px; }
  .header-link { font-size:13px; color:var(--muted); }
  .header-link:hover { color:var(--fg); }
  .gen-time { font-size:12px; color:var(--muted); }

  /* filter bar */
  .page-band { padding:14px 24px; border-bottom:1px solid var(--border); }
  div#filter-bar {
                background:var(--panel); display:flex; gap:16px;
                flex-wrap:wrap; align-items:flex-end; }
  .filter-group { display:flex; flex-direction:column; gap:5px; }
  .filter-group label { font-size:11px; font-weight:600; text-transform:uppercase;
                        letter-spacing:0.5px; color:var(--muted); }
  select { background:var(--bg); color:var(--fg); border:1px solid var(--border);
           border-radius:6px; padding:5px 10px; font:inherit; font-size:13px;
           cursor:pointer; min-width:180px; }
  select:focus { outline:1px solid var(--link); }
  .toggle-row { display:flex; gap:8px; }
  .toggle { padding:5px 12px; border:1px solid var(--border); border-radius:6px;
            background:var(--bg); color:var(--muted); cursor:pointer;
            font:inherit; font-size:13px; white-space:nowrap; }
  .toggle.on { border-color:var(--link); color:var(--fg); background:var(--panel-2); }
  .toggle:hover { border-color:var(--link); color:var(--fg); }
  button#clear-btn { padding:5px 12px; border:1px solid var(--border); border-radius:6px;
               background:none; color:var(--muted); cursor:pointer;
               font:inherit; font-size:13px; align-self:flex-end; }
  button#clear-btn:hover { color:var(--fg); border-color:var(--fg); }

  /* result bar */
  .flex-row { display:flex; align-items:center; flex-wrap:wrap; }
  /* activity chart */
  div#activity-chart { background:var(--panel); }
  .chart-header { display:flex; align-items:center; gap:16px; flex-wrap:wrap; margin-bottom:14px; }
  .chart-header h2 { font-size:12px; font-weight:600; text-transform:uppercase; letter-spacing:0.5px; color:var(--muted); margin:0; }
  .legend-item { display:inline-flex; align-items:center; gap:5px; font-size:12px; color:var(--muted); }
  .legend-dot { width:10px; height:10px; border-radius:2px; flex-shrink:0; }
  .window-ctrl { display:flex; align-items:center; gap:8px; margin-left:auto; font-size:12px; color:var(--muted); }
  .window-ctrl input[type=range] { accent-color:var(--link); width:100px; cursor:pointer; }
  .window-ctrl strong { color:var(--fg); min-width:40px; display:inline-block; }
  .chart-area { position:relative; }
  .chart-wrap { display:flex; gap:2px; align-items:flex-end; height:140px; padding-bottom:24px; }
  .chart-bar-group { display:flex; flex-direction:column; align-items:center; flex:1; position:relative; cursor:pointer; }
  .chart-bar-group:hover .chart-tooltip { display:block; }
  .chart-segments { display:flex; flex-direction:column-reverse; width:100%; gap:1px; border-radius:3px 3px 0 0; overflow:hidden; }
  .bar-seg { width:100%; min-height:2px; }
  .bar-opened { background:#58a6ff; }
  .bar-merged  { background:#56d364; }
  .bar-closed  { background:#f85149; }
  .chart-date { font-size:9px; color:var(--muted); position:absolute; bottom:-20px; left:50%; transform:translateX(-50%) rotate(-35deg); transform-origin:top center; white-space:nowrap; }
  .chart-baseline { position:absolute; bottom:24px; left:0; right:0; height:1px; background:var(--border); }
  .chart-tooltip { display:none; position:absolute; bottom:calc(100% + 4px); left:50%; transform:translateX(-50%); background:#1c2128; border:1px solid var(--border); border-radius:6px; padding:8px 12px; font-size:11.5px; white-space:nowrap; z-index:20; pointer-events:none; box-shadow:0 4px 12px #0006; }
  .chart-tooltip div { color:var(--fg); line-height:1.8; }
  .chart-stats { display:flex; gap:24px; margin-top:10px; flex-wrap:wrap; }
  .stat-box { font-size:12px; }
  .stat-box .num { font-size:20px; font-weight:700; line-height:1; }
  .stat-box .lbl { color:var(--muted); margin-top:2px; }
  .chart-row { display:grid; grid-template-columns:1fr 1fr; gap:16px; padding:16px 24px 20px; border-bottom:1px solid var(--border); background:var(--panel); }
  .mini-chart { background:var(--panel-2); border:1px solid var(--border); border-radius:8px; padding:14px 16px; }
  .mini-chart h3 { font-size:11px; font-weight:600; text-transform:uppercase; letter-spacing:0.5px; color:var(--muted); margin:0 0 12px; }
  /* WG breakdown bars */
  .wg-bar-row { display:flex; align-items:center; gap:8px; margin-bottom:8px; font-size:12px; }
  .wg-bar-label { width:140px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; color:var(--muted); text-align:right; flex-shrink:0; }
  .wg-bar-track { flex:1; background:var(--bg); border-radius:3px; height:12px; position:relative; }
  .wg-bar-fill { height:100%; border-radius:3px; display:flex; align-items:center; }
  .wg-bar-open   { background:#58a6ff88; }
  .wg-bar-unclaim { background:var(--border); }
  .wg-bar-count { position:absolute; right:-28px; top:0; font-size:11px; color:var(--muted); line-height:12px; }
  /* Cumulative open line */
  div#cumulative-chart { width:100%; overflow:hidden; }

  div#result-bar { gap:16px; font-size:13px; }
  span#result-count { font-weight:600; }
  .result-breakdown { color:var(--muted); font-size:12px; }
  .chip { display:inline-block; padding:1px 8px; border-radius:10px;
          font-size:11px; font-weight:600; margin:0 2px; }
  .chip.wg     { background:#1f3a5f; border:1px solid #79c0ff; color:#79c0ff; }
  .chip.wg.predicted { background:transparent; border-style:dashed; }
  .suggest { color:#56d364; }
  .suggest strong { cursor:help; border-bottom:1px dotted #56d364; }
  .chip.maint  { background:#21262d; border:1px solid var(--border); color:var(--muted); }
  .chip.first  { background:#1f6feb; color:#fff; }
  .chip.lead   { background:#3d3000; border:1px solid var(--tier-lead); color:var(--tier-lead); }
  .chip.active { background:#1f3a5f; border:1px solid var(--tier-active); color:var(--tier-active); }
  .chip.stale  { background:var(--stale); color:#000; }
  .chip.hot    { background:var(--hot); color:#fff; }
  .chip.warm   { background:#e3a000; color:#000; }
  .chip.draft  { background:var(--draft); color:var(--fg); }

  /* pr list */
  div#pr-list { padding:16px 24px; max-width:1100px; margin:0 auto; }
  .group-header { font-size:12px; font-weight:700; text-transform:uppercase;
                  letter-spacing:0.5px; color:var(--muted); margin:24px 0 10px;
                  padding-bottom:6px; border-bottom:1px solid var(--border);
                  display:flex; align-items:center; gap:8px; }
  .group-header:first-child { margin-top:0; }
  .pr-card { padding:10px 14px; border:1px solid var(--border); border-radius:6px;
             margin-bottom:8px; background:var(--panel); }
  .pr-card.is-draft { opacity:0.7; }
  .pr-head { display:flex; gap:8px; align-items:baseline; flex-wrap:wrap; }
  .pr-num { color:var(--muted); font-variant-numeric:tabular-nums; font-size:13px; }
  .pr-title { font-weight:500; flex:1; min-width:0; }
  .pr-title a { color:var(--fg); }
  .pr-meta { color:var(--muted); font-size:12px; margin-top:4px;
             display:flex; gap:12px; flex-wrap:wrap; align-items:center; }
  .pr-chips { display:flex; gap:4px; flex-wrap:wrap; margin-top:6px; }
  .pr-chips .label-chip { font-family:ui-monospace,monospace; font-size:11px;
                          padding:1px 6px; border-radius:10px; background:var(--bg);
                          border:1px solid var(--border); color:var(--muted); }
  .age-60 { color:var(--stale); font-weight:600; }
  .age-30 { color:var(--stale); }
  .age-14 { color:#e3a000; }
  .empty  { color:var(--muted); text-align:center; padding:60px 0; font-size:14px; }

  footer { text-align:center; font-size:12px; color:var(--muted);
           padding:12px 24px; border-top:1px solid var(--border); margin-top:16px; }

  .site-nav { display:flex; gap:0; background:var(--panel-2); border-bottom:1px solid var(--border); padding:0 24px; }
  .site-nav a { color:var(--muted); font-size:13px; font-weight:500; padding:10px 18px; border-bottom:2px solid transparent; text-decoration:none; white-space:nowrap; }
  .site-nav a:hover { color:var(--fg); text-decoration:none; }
  .site-nav a.active { color:var(--fg); border-bottom-color:var(--link); }

  /* focus panel */
  details#focus-panel { padding:14px 24px; border-bottom:1px solid var(--border); background:var(--panel); }
  details#focus-panel summary { font-size:12px; font-weight:700; text-transform:uppercase; letter-spacing:0.5px; color:var(--muted); cursor:pointer; user-select:none; }
  details#focus-panel summary:hover { color:var(--fg); }
  .focus-wg-section { margin-top:14px; }
  .focus-wg-label { font-size:11px; font-weight:600; text-transform:uppercase; letter-spacing:0.5px; color:var(--muted); margin-bottom:6px; }
  details#pulse-banner { border-bottom:1px solid var(--border); }
  details#pulse-banner summary::-webkit-details-marker { display:none; }
  details#pulse-banner[open] summary .pulse-toggle::after { content:" ▴"; }
  details#pulse-banner:not([open]) summary .pulse-toggle::after { content:" ▾"; }
  .pulse-toggle { color:var(--muted);font-size:11px;margin-left:auto; }
  .pulse-contrib { background:var(--panel-2);border:1px solid var(--border);border-radius:4px;padding:2px 8px;font-size:12px;color:var(--fg); }
  .pulse-table { width:100%;border-collapse:collapse;font-size:12px;margin-top:8px; }
  .pulse-table th { text-align:left;color:var(--muted);font-weight:600;font-size:11px;text-transform:uppercase;letter-spacing:.5px;padding:4px 8px;border-bottom:1px solid var(--border); }
  .pulse-table td { padding:5px 8px;border-bottom:1px solid var(--border);vertical-align:top; }
  .pulse-table tr:last-child td { border-bottom:none; }
  .pulse-section-label { font-size:11px;text-transform:uppercase;color:var(--muted);letter-spacing:.5px; }
  .pulse-subsummary { cursor:pointer;font-size:12px;font-weight:600;color:var(--muted);padding:4px 0;list-style:none; }
  .pulse-subsummary::-webkit-details-marker { display:none; }
  .pulse-subsummary::before { content:'▸ '; }
  details[open] > .pulse-subsummary::before { content:'▾ '; }
  .focus-cards { display:flex; gap:10px; flex-wrap:wrap; }
  .focus-card { background:var(--panel-2); border:1px solid var(--border); border-radius:6px; padding:10px 14px; flex:1; min-width:220px; max-width:360px; position:relative; }
  .focus-card:hover { border-color:var(--link); }
  .focus-card a { color:var(--fg); font-size:13px; font-weight:500; }
  .focus-card a:hover { color:var(--link); text-decoration:underline; }
  .focus-reviewer { font-size:11px; margin-top:5px; color:#56d364; }
  .focus-reviewer strong { color:#56d364; }
  .focus-reviewer.no-reviewer { color:#ffa657; }
  .score-badge { position:absolute; top:10px; right:10px; font-size:11px; font-weight:700; background:var(--panel); border:1px solid var(--border); border-radius:4px; padding:1px 6px; color:var(--muted); }
  .focus-meta { font-size:11px; color:var(--muted); margin-top:4px; display:flex; gap:8px; flex-wrap:wrap; }

  /* RFC panel */
  details#rfc-panel { padding:14px 24px; border-bottom:1px solid var(--border); background:var(--panel); }
  details#rfc-panel summary { font-size:12px; font-weight:700; text-transform:uppercase; letter-spacing:0.5px; color:var(--muted); cursor:pointer; user-select:none; }
  details#rfc-panel summary:hover { color:var(--fg); }
  .rfc-table { width:100%; border-collapse:collapse; font-size:12px; margin-top:12px; }
  .rfc-table th { text-align:left; color:var(--muted); font-weight:600; font-size:11px; text-transform:uppercase; letter-spacing:0.5px; padding:4px 8px; border-bottom:1px solid var(--border); }
  .rfc-table td { padding:6px 8px; border-bottom:1px solid var(--border); vertical-align:top; }
  .rfc-table tr:last-child td { border-bottom:none; }
  .rfc-stale td { color:var(--stale); }
  .rfc-very-stale td { color:var(--hot); }
  .rfc-status-chip { display:inline-block; padding:1px 6px; border-radius:8px; font-size:10px; font-weight:600; background:var(--panel-2); border:1px solid var(--border); }

  /* trend chart */
  div#trend-chart-wrap { width:100%; overflow:hidden; }
</style>
</head>
<body>

<header>
  <div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:12px;width:100%;">
    <div>
      <h1 style="margin:0;font-size:17px;font-weight:600;">PR Triage — <span style="color:var(--muted);font-weight:400">__REPO__</span></h1>
      <div style="font-size:11px;color:var(--muted);margin-top:2px;">generated __GENERATED_AT__</div>
    </div>
    <a class="header-link" href="index.html" style="font-size:13px;color:var(--muted);">&#8592; Label dashboard</a>
  </div>
  <div id="hdr-stats" style="display:flex;gap:20px;flex-wrap:wrap;align-items:center;width:100%;padding-top:10px;border-top:1px solid var(--border);margin-top:10px;">
    <div style="text-align:center;">
      <div class="total-badge" id="hdr-total">—</div>
      <div class="total-label">open PRs</div>
    </div>
    <div style="display:flex;gap:16px;flex-wrap:wrap;font-size:13px;">
      <span id="hdr-wg-count" style="color:var(--wg);">— with workgroup</span>
      <span id="hdr-maint-count" style="color:var(--muted);">— maintainer PRs</span>
      <span id="hdr-first-count" style="color:var(--first);">— first-timers</span>
      <span id="hdr-stale-count" style="color:var(--staleness);">— stale 30d+</span>
      <span id="hdr-rfc-count" style="color:var(--hot);">— RFC required</span>
    </div>
  </div>
</header>

<details id="pulse-banner" class="page-band" open>
  <summary id="pulse-summary" style="cursor:pointer;list-style:none;display:flex;align-items:center;gap:16px;flex-wrap:wrap;"></summary>
  <div id="pulse-detail" style="margin-top:14px;"></div>
</details>

<div id="activity-chart" class="page-band">
  <div class="chart-header">
    <h2>PR Activity</h2>
    <span class="legend-item"><span class="legend-dot bar-opened"></span>Opened</span>
    <span class="legend-item"><span class="legend-dot bar-merged"></span>Merged</span>
    <span class="legend-item"><span class="legend-dot bar-closed"></span>Closed (not merged)</span>
    <span class="legend-item"><span style="display:inline-block;width:12px;height:2px;background:var(--staleness);border-radius:1px;margin-right:1px"></span>Open backlog</span>
    <div class="window-ctrl">
      <label for="day-window">Window:</label>
      <input type="range" id="day-window" min="7" max="30" value="10" step="1">
      <strong id="day-window-label">10 days</strong>
    </div>
  </div>
  <div class="chart-area">
    <div class="chart-baseline"></div>
    <div class="chart-wrap" id="chart-bars"></div>
  </div>
  <div class="chart-stats" id="chart-stats"></div>
</div>
<div class="chart-row">
  <div class="mini-chart">
    <h3>Open PRs by Workgroup</h3>
    <div id="wg-breakdown"></div>
  </div>
  <div class="mini-chart">
    <h3>Cumulative Open PRs <span id="cumul-window" style="font-weight:400;text-transform:none;letter-spacing:0"></span></h3>
    <div id="cumulative-chart"></div>
  </div>
</div>
<div class="chart-row">
  <div class="mini-chart" style="grid-column:1/-1">
    <h3>Open count — last 90 days &nbsp;<span style="color:var(--stale);font-size:10px;font-weight:400;text-transform:none">&#9644; Issues</span> &nbsp;<span style="color:#d2a8ff;font-size:10px;font-weight:400;text-transform:none">&#9644; PRs</span></h3>
    <div id="trend-chart-wrap"></div>
  </div>
</div>

<details id="focus-panel">
  <summary>&#9650; WG Focus — top community PRs by merit &nbsp;<span id="focus-count" style="font-weight:400;text-transform:none;letter-spacing:0;color:var(--muted)"></span></summary>
  <div id="focus-body"></div>
</details>

<div id="filter-bar" class="page-band">
  <div class="filter-group">
    <label>Filter by</label>
    <select id="wg-select">
      <option value="">All PRs</option>
      <optgroup label="Workgroup">
        __WG_OPTIONS__
        <option value="__unclaimed__">Unclaimed (no WG)</option>
      </optgroup>
      <optgroup label="Contributor">
        <option value="__maintainers__">Maintainer PRs</option>
        <option value="__community__">Community PRs</option>
      </optgroup>
    </select>
  </div>

  <div class="filter-group">
    <label>Contributor</label>
    <div class="toggle-row">
      <button class="toggle" id="tog-maintainer">Maintainer PRs</button>
      <button class="toggle" id="tog-first">First-timers</button>
      <button class="toggle" id="tog-lead">WG Leads / Code Owners</button>
    </div>
  </div>

  <div class="filter-group">
    <label>Staleness</label>
    <div class="toggle-row">
      <button class="toggle" id="tog-stale14">14d+</button>
      <button class="toggle" id="tog-stale30">30d+</button>
      <button class="toggle" id="tog-stale60">60d+</button>
    </div>
  </div>

  <div class="filter-group">
    <label>RFC</label>
    <div class="toggle-row">
      <button class="toggle" id="tog-rfc-required">RFC Required</button>
    </div>
  </div>

  <div class="filter-group">
    <label>Routing</label>
    <div class="toggle-row">
      <button class="toggle" id="tog-suggested">Suggested reviewer</button>
    </div>
  </div>
  <button id="clear-btn">Clear all</button>
</div>

<div id="result-bar" class="flex-row page-band">
  <span id="result-count"></span>
  <span class="result-breakdown" id="result-breakdown"></span>
</div>

<div id="pr-list"></div>

<div id="new-prs-panel" class="page-band">
  <div class="chart-header" style="margin-bottom:10px;">
    <h2>New PRs — needs reviewer <span id="new-prs-count" style="font-weight:400;color:var(--muted);text-transform:none;letter-spacing:0;font-size:12px;"></span></h2>
  </div>
  <div id="new-prs-body"></div>
</div>

<details id="rfc-panel">
  <summary>RFC Tracker — <span id="rfc-panel-count">0</span> open RFCs</summary>
  <div id="rfc-body" style="overflow-x:auto"></div>
</details>

<footer>
  Re-run <code>python .github/scripts/triage_maintainer.py</code> to refresh.
  &nbsp;|&nbsp; <a href="index.html">Label dashboard</a>
</footer>

<script>
const DATA = __DATA__;
const ALL_WGS = __ALL_WGS__;
const FOCUS_DATA = __FOCUS_DATA__;
const RFC_DATA = __RFC_DATA__;
const TREND_DATA = __TREND_DATA__;
const PULSE = __PULSE_DATA__;

// ── filter state ──
const state = {
  wg: "",           // "" = all, "__unclaimed__" = no wg, or a wg name
  maintainer: false,
  firstTimer: false,
  wgLead: false,
  staleDays: 0,     // 0 = any, 14, 30, or 60
  rfcRequired: false,
  suggested: false,
};

// ── helpers ──
function esc(s) {
  return String(s).replace(/[&<>"']/g,
    c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
}
function ageText(d) {
  if(d<1) return "today"; if(d<30) return d+"d";
  if(d<365) return Math.floor(d/30)+"mo"; return Math.floor(d/365)+"y";
}
function ageClass(d) { return d>=60?"age-60":d>=30?"age-30":d>=14?"age-14":""; }

function prChips(pr) {
  const chips = [];
  if(pr.is_draft) chips.push('<span class="chip draft">draft</span>');
  if(pr.labels.some(l=>l.startsWith("priority::")&&l.includes("hot")))
    chips.push('<span class="chip hot">HOT</span>');
  else if(pr.labels.some(l=>l.startsWith("priority::")&&l.includes("warm")))
    chips.push('<span class="chip warm">WARM</span>');
  if(pr.wg && pr.wg_source==="predicted")
    chips.push(`<span class="chip wg predicted" title="auto-classified: ${esc(pr.wg_reason||"")}">${esc(pr.wg)} (auto)</span>`);
  else if(pr.wg) chips.push(`<span class="chip wg">${esc(pr.wg)}</span>`);
  if(pr.is_maintainer) chips.push('<span class="chip maint">maintainer</span>');
  else if(pr.community_tier==="wg-lead") chips.push('<span class="chip lead">WG lead / owner</span>');
  else if(pr.community_tier==="active") chips.push('<span class="chip active">active contributor</span>');
  else if(pr.community_tier==="first-timer") chips.push('<span class="chip first">first-timer</span>');
  if(pr.days_stale>=14) chips.push(`<span class="chip stale">${pr.days_stale}d stale</span>`);
  if(pr.rfc_status) {
    const RFC_LABEL = {"required":"RFC required","on-roadmap":"RFC on roadmap","open-for-comment":"RFC open","not-required":"RFC not required"};
    const rfcColor = pr.rfc_status==="required" ? "var(--hot)" : pr.rfc_status==="on-roadmap" ? "var(--first)" : "var(--muted)";
    chips.push(`<span style="display:inline-block;padding:1px 6px;border-radius:8px;font-size:11px;font-weight:600;background:var(--panel-2);border:1px solid ${rfcColor};color:${rfcColor}">${RFC_LABEL[pr.rfc_status]||pr.rfc_status}</span>`);
  }
  return chips.join(" ");
}

function prCard(pr) {
  const ageC = ageClass(pr.days_stale);
  const assignees = pr.assignees.length
    ? pr.assignees.map(a=>`@${esc(a)}`).join(", ")
    : "<em>unassigned</em>";
  const labelPills = pr.labels
    .map(l=>`<span class="label-chip">${esc(l)}</span>`).join("");
  const chips = prChips(pr);
  return `
    <div class="pr-card${pr.is_draft?" is-draft":""}">
      <div class="pr-head">
        <span class="pr-num">#${pr.number}</span>
        <span class="pr-title">
          <a href="${pr.url}" target="_blank" rel="noopener">${esc(pr.title)}</a>
        </span>
      </div>
      <div class="pr-meta">
        <span>by <strong><a href="https://github.com/${esc(pr.author)}"
          target="_blank" rel="noopener">${esc(pr.author)}</a></strong></span>
        <span>opened ${ageText(pr.days_old)} ago</span>
        <span class="${ageC}" title="Last human comment, review, or push; bot label changes are ignored">active ${ageText(pr.days_stale)} ago</span>
        <span>${pr.comments} comment${pr.comments===1?"":"s"}</span>
        <span>→ ${assignees}</span>
        ${pr.suggested_reviewers.length ? `<span class="suggest">suggested: ${pr.suggested_reviewers
          .map(r=>`<strong title="${esc(r.why)}">@${esc(r.login)}</strong>`).join(", ")}</span>` : ""}
      </div>
      <div class="pr-chips">${chips}${labelPills?`<span style="margin-left:4px">${labelPills}</span>`:""}</div>
    </div>`;
}

// ── filtering ──
function matchesPR(pr) {
  // No filter active = show everything
  const SPECIAL = ["__unclaimed__","__maintainers__","__community__"];
  if(state.wg === "__unclaimed__"   && pr.wg) return false;
  if(state.wg === "__maintainers__" && !pr.is_maintainer) return false;
  if(state.wg === "__community__"   && pr.is_maintainer) return false;
  if(state.wg && !SPECIAL.includes(state.wg) && pr.wg !== state.wg) return false;
  if(state.maintainer && !pr.is_maintainer) return false;
  if(state.firstTimer && pr.community_tier !== "first-timer") return false;
  if(state.wgLead && pr.community_tier !== "wg-lead") return false;
  if(state.staleDays && pr.days_stale < state.staleDays) return false;
  if(state.rfcRequired && pr.rfc_status !== "required") return false;
  if(state.suggested && !pr.suggested_reviewers.length) return false;
  return true;
}

// ── grouping ──
function groupKey(pr) {
  const wgFiltered = state.wg && !["__maintainers__","__community__","__unclaimed__"].includes(state.wg);
  const contribFiltered = state.maintainer || state.firstTimer || state.wgLead
    || state.wg === "__maintainers__" || state.wg === "__community__";

  // Filtering by a specific WG: group by contributor tier
  if(wgFiltered) {
    if(pr.is_maintainer) return {key:"__maint__", label:"Maintainer PRs", order:0};
    if(pr.community_tier==="wg-lead") return {key:"__lead__", label:"Key Contributors", order:1};
    if(pr.community_tier==="active") return {key:"__active__", label:"Active Contributors", order:2};
    if(pr.community_tier==="first-timer") return {key:"__first__", label:"First-timers", order:4};
    return {key:"__returning__", label:"Returning Contributors", order:3};
  }

  // Filtering by contributor type: group by WG
  if(contribFiltered) {
    if(pr.wg) return {key:"wg:"+pr.wg, label:`WG: ${pr.wg}`, order:1};
    return {key:"__unclaimed__", label:"No Workgroup", order:2};
  }

  // All PRs: WGs in canonical order, then no-WG buckets by contributor tier
  const wgIdx = ALL_WGS.indexOf(pr.wg);
  if(pr.wg && wgIdx !== -1) return {key:"wg:"+pr.wg, label:`WG: ${pr.wg}`, order:wgIdx};
  // Unknown WG name (shouldn't happen) — show before no-WG buckets
  if(pr.wg) return {key:"wg:"+pr.wg, label:`WG: ${pr.wg}`, order:ALL_WGS.length};
  const noWgBase = ALL_WGS.length + 1;
  if(pr.is_maintainer) return {key:"__maint__", label:"Maintainer PRs — no WG", order:noWgBase};
  if(pr.community_tier==="wg-lead") return {key:"__lead__", label:"Key Contributors — no WG", order:noWgBase+1};
  if(pr.community_tier==="active") return {key:"__active__", label:"Active Contributors — no WG", order:noWgBase+2};
  if(pr.community_tier==="first-timer") return {key:"__first__", label:"First-timers — no WG", order:noWgBase+4};
  return {key:"__returning__", label:"Returning Contributors — no WG", order:noWgBase+3};
}

// ── render ──
function render() {
  const matches = DATA.filter(matchesPR);

  // Sort: hot first, then most recently updated first
  matches.sort((a,b)=>{
    const ah = a.labels.some(l=>l.startsWith("priority::")&&l.includes("hot"))?1:0;
    const bh = b.labels.some(l=>l.startsWith("priority::")&&l.includes("hot"))?1:0;
    return bh-ah || a.days_stale-b.days_stale;
  });

  // Group
  const groups = {};
  for(const pr of matches) {
    const g = groupKey(pr);
    if(!groups[g.key]) groups[g.key] = {...g, prs:[]};
    groups[g.key].prs.push(pr);
  }
  const sorted = Object.values(groups).sort((a,b)=>a.order-b.order||a.label.localeCompare(b.label));

  const container = document.getElementById("pr-list");
  if(!matches.length) {
    container.innerHTML = '<div class="empty">No PRs match the current filters.</div>';
  } else {
    let html = "";
    for(const g of sorted) {
      html += `<div class="group-header">
        <span>${esc(g.label)}</span>
        <span style="font-weight:400;text-transform:none;letter-spacing:0;color:var(--muted)">
          ${g.prs.length} PR${g.prs.length===1?"":"s"}
        </span>
      </div>`;
      html += g.prs.map(prCard).join("");
    }
    container.innerHTML = html;
  }

  // Result bar
  document.getElementById("result-count").textContent =
    `Showing ${matches.length} of ${DATA.length} open PRs`;

  const parts = [];
  if(state.wg==="__unclaimed__")   parts.push("no WG");
  else if(state.wg==="__maintainers__") parts.push("maintainers");
  else if(state.wg==="__community__")   parts.push("community");
  else if(state.wg) parts.push(state.wg);
  if(state.maintainer) parts.push("maintainers only");
  if(state.firstTimer) parts.push("first-timers only");
  if(state.wgLead) parts.push("WG leads / owners only");
  if(state.staleDays) parts.push(`${state.staleDays}d+ stale`);
  if(state.rfcRequired) parts.push("RFC required");
  if(state.suggested) parts.push("has suggested reviewer");
  document.getElementById("result-breakdown").textContent =
    parts.length ? "Filters: " + parts.join(" · ") : "All open PRs";
}

function renderPulse() {
  const summary = document.getElementById("pulse-summary");
  const detail  = document.getElementById("pulse-detail");
  if (!PULSE || !summary) return;
  const p = PULSE;
  const wgMerged = p.wg_merged || {};
  const rfcActive = RFC_DATA.filter(r=>r.days_quiet<=14);
  const rfcStale  = RFC_DATA.filter(r=>r.days_quiet>=30);
  const rfcNeedReview = RFC_DATA.filter(r=>r.days_quiet<=14).slice().sort((a,b)=>b.comments-a.comments);
  const rfcHotStale   = RFC_DATA.filter(r=>r.days_quiet>14).slice().sort((a,b)=>b.comments-a.comments).slice(0,5);

  // Top WGs by merge count
  const topWGs = Object.entries(wgMerged).sort((a,b)=>b[1]-a[1]).slice(0,3);
  const wgSummary = topWGs.map(([wg,n])=>`<strong>${esc(wg)}</strong> (${n})`).join(", ");

  // Top contributors
  const topC = (p.top_contributors||[]).slice(0,4).map(c=>`@${esc(c.login)} (${c.merged})`).join(", ");

  // Prose narrative
  const wgLead = topWGs[0] ? `${topWGs[0][0]} led with ${topWGs[0][1]}` : "";
  const rfcLine = rfcActive.length
    ? `${rfcActive.length} RFC${rfcActive.length>1?"s":""} actively discussed`
    : "no RFC activity";
  const staleLine = rfcStale.length ? `${rfcStale.length} gone quiet 30d+` : "";

  summary.innerHTML = `<span style="font-size:13px;font-weight:700;color:var(--fg);">Last week</span>
    <span style="font-size:13px;color:var(--muted);margin-left:4px;">
      <strong style="color:var(--fg)">${p.merged_count} PRs merged</strong> —
      ${wgLead ? wgLead + ". " : ""}${topC ? "Top contributors: " + topC + "." : ""}
      ${p.new_issues_count} new issue${p.new_issues_count!==1?"s":""}, ${p.new_bugs_count} bug${p.new_bugs_count!==1?"s":""}.
      ${rfcLine}${staleLine ? "; " + staleLine : ""}.
    </span>
    <span class="pulse-toggle" style="margin-left:auto;"></span>`;

  // Merged PRs sub-section (collapsed)
  const mergedRows = (p.merged||[]).slice(0,20).map(m=>`
    <tr>
      <td><a href="${esc(m.url)}" target="_blank" rel="noopener">#${m.number}</a></td>
      <td>${esc((m.title||"").slice(0,80))}</td>
      <td style="color:var(--muted)">@${esc(m.author)}</td>
      <td style="color:var(--muted)">${(m.merged_at||"").slice(0,10)}</td>
    </tr>`).join("");

  // RFC sub-section (collapsed)
  const rfcRows = [...rfcActive.map(r=>[r,"#56d364","active"]),
    ...rfcHotStale.map(r=>[r,r.days_quiet>=30?"#ff7b72":"#ffa657",`stale ${r.days_quiet}d`])]
    .map(([r,color,status])=>`
      <tr>
        <td><a href="${esc(r.url)}" target="_blank" rel="noopener">#${r.number}</a></td>
        <td>${esc((r.title||"").slice(0,72))}</td>
        <td style="color:${color}">${status}</td>
        <td>${r.comments}</td>
      </tr>`).join("");

  detail.innerHTML = `
    <details style="margin-bottom:10px;">
      <summary class="pulse-subsummary">${p.merged_count} merged PRs — by workgroup: ${wgSummary}</summary>
      <table class="pulse-table" style="margin-top:8px;">
        <thead><tr><th>#</th><th>Title</th><th>Author</th><th>Merged</th></tr></thead>
        <tbody>${mergedRows}</tbody>
      </table>
    </details>
    <details>
      <summary class="pulse-subsummary">RFCs — ${rfcActive.length} active · ${rfcStale.length} stale · ${RFC_DATA.length} total</summary>
      <table class="pulse-table" style="margin-top:8px;">
        <thead><tr><th>#</th><th>Title</th><th>Status</th><th>Comments</th></tr></thead>
        <tbody>${rfcRows}</tbody>
      </table>
    </details>`;
}

function renderNewPRs() {
  const body = document.getElementById("new-prs-body");
  const countEl = document.getElementById("new-prs-count");
  if (!body) return;
  const newPRs = DATA.filter(pr =>
    !pr.is_draft &&
    pr.days_old <= 14 &&
    pr.community_tier !== "first-timer" &&
    (!pr.review_requested || !pr.review_requested.length)
  ).sort((a, b) => a.days_old - b.days_old);
  countEl.textContent = newPRs.length
    ? `${newPRs.length} PR${newPRs.length>1?"s":""} need a reviewer`
    : "all covered";
  if (!newPRs.length) {
    body.innerHTML = `<div style="color:#56d364;font-size:12px;">All recent PRs have a reviewer assigned.</div>`;
    return;
  }
  let html = `<div class="focus-cards">`;
  for (const pr of newPRs) {
    const sugg = (pr.suggested_reviewers||[]).slice(0,2);
    const suggLine = sugg.length
      ? `<div class="focus-reviewer">suggest: ${sugg.map(r=>`<strong>@${esc(r.login)}</strong><span title="${esc(r.why)}" style="cursor:help;color:var(--muted)"> ⓘ</span>`).join(", ")}</div>`
      : `<div class="focus-reviewer no-reviewer">⚠ no suggestion — needs manual triage</div>`;
    const wgChip = pr.wg
      ? `<span class="chip wg${pr.wg_source==="predicted"?" predicted":""}" style="font-size:10px;">${esc(pr.wg)}</span> `
      : "";
    const ageBg = pr.days_old <= 2 ? "background:#0d4429;color:#56d364;" : "";
    html += `<div class="focus-card" style="border-color:${sugg.length?"var(--border)":"#ffa657"}">
      <span class="score-badge" style="${ageBg}">${pr.days_old}d old</span>
      <div>${wgChip}<a href="${esc(pr.url)}" target="_blank" rel="noopener">#${pr.number} ${esc(pr.title.length>58?pr.title.slice(0,58)+"…":pr.title)}</a></div>
      <div class="focus-meta"><span>by @${esc(pr.author)}</span><span style="color:var(--muted)">${esc(pr.community_tier||"")}</span></div>
      ${suggLine}
    </div>`;
  }
  html += `</div>`;
  body.innerHTML = html;
}

function renderFocusPanel() {
  const body = document.getElementById("focus-body");
  const countEl = document.getElementById("focus-count");
  const wgs = Object.keys(FOCUS_DATA).sort();
  if (!wgs.length) { body.innerHTML = '<div style="color:var(--muted);font-size:12px;padding:10px 0">No workgroup data</div>'; return; }
  let total = 0;
  let html = "";
  for (const wg of ALL_WGS) {
    const prs = FOCUS_DATA[wg];
    if (!prs || !prs.length) continue;
    total += prs.length;
    html += `<div class="focus-wg-section"><div class="focus-wg-label">${esc(wg)}</div><div class="focus-cards">`;
    for (const pr of prs) {
      const reasons = [];
      if (pr.labels.some(l=>l.includes("hot"))) reasons.push("HOT");
      else if (pr.labels.some(l=>l.includes("warm"))) reasons.push("WARM");
      if (pr.labels.includes("bug")) reasons.push("bug");
      if (pr.rfc_status==="required") reasons.push("RFC required");
      if (pr.days_stale>=60) reasons.push(`${pr.days_stale}d stale`);
      else if (pr.days_stale>=30) reasons.push(`${pr.days_stale}d stale`);
      if (!pr.assignees.length) reasons.push("unassigned");
      if (pr.community_tier==="first-timer") reasons.push("first-timer");
      const revLine = pr.review_requested && pr.review_requested.length
        ? `<div class="focus-reviewer">reviewer: ${pr.review_requested.slice(0,2).map(r=>`<strong>@${esc(r)}</strong>`).join(", ")}</div>`
        : `<div class="focus-reviewer no-reviewer">⚠ no reviewer yet</div>`;
      html += `<div class="focus-card">
        <span class="score-badge">${pr.score}</span>
        <div><a href="${pr.url}" target="_blank" rel="noopener">#${pr.number} ${esc(pr.title.length>60?pr.title.slice(0,60)+"…":pr.title)}</a></div>
        <div class="focus-meta">${reasons.map(r=>`<span>${esc(r)}</span>`).join(" · ")}</div>
        ${revLine}
      </div>`;
    }
    html += "</div></div>";
  }
  body.innerHTML = html;
  countEl.textContent = `${total} highlight${total===1?"":"s"} across ${wgs.filter(w=>FOCUS_DATA[w]?.length).length} workgroups`;
}

function renderRFCPanel() {
  const body = document.getElementById("rfc-body");
  const countEl = document.getElementById("rfc-panel-count");
  countEl.textContent = RFC_DATA.length;
  if (!RFC_DATA.length) {
    body.innerHTML = '<div style="color:var(--muted);font-size:12px;padding:10px 0">No open RFCs found</div>';
    return;
  }
  const STATUS_COLOR = {"on-roadmap":"var(--first)","open-for-comment":"var(--link)","not-planned":"var(--hot)","unlabeled":"var(--muted)"};
  const STATUS_LABEL = {"on-roadmap":"on roadmap","open-for-comment":"open for comment","not-planned":"not planned","unlabeled":"no status label"};
  let html = `<table class="rfc-table"><thead><tr>
    <th>Title</th><th>Author</th><th>Days open</th><th>Last activity</th><th>Comments</th><th>Status</th>
  </tr></thead><tbody>`;
  for (const rfc of RFC_DATA) {
    const rowCls = rfc.days_quiet>=30?"rfc-very-stale":rfc.days_quiet>=14?"rfc-stale":"";
    const color = STATUS_COLOR[rfc.rfc_status] || "var(--muted)";
    const label = STATUS_LABEL[rfc.rfc_status] || rfc.rfc_status;
    html += `<tr class="${rowCls}">
      <td><a href="${esc(rfc.url)}" target="_blank" rel="noopener">#${rfc.number} ${esc(rfc.title)}</a></td>
      <td>@${esc(rfc.author)}</td>
      <td>${rfc.days_open}d</td>
      <td>${rfc.days_quiet===0?"today":rfc.days_quiet+"d ago"}</td>
      <td>${rfc.comments}</td>
      <td><span class="rfc-status-chip" style="color:${color};border-color:${color}">${esc(label)}</span></td>
    </tr>`;
  }
  html += "</tbody></table>";
  body.innerHTML = html;
}

function renderTrendChart() {
  const el = document.getElementById("trend-chart-wrap");
  if (!TREND_DATA.length) { el.innerHTML = ""; return; }
  const W = el.getBoundingClientRect().width || el.parentElement.getBoundingClientRect().width || 600;
  const H = 100;
  const maxIssues = Math.max(...TREND_DATA.map(d=>d.open_issues), 1);
  const maxPRs    = Math.max(...TREND_DATA.map(d=>d.open_prs), 1);
  const maxY = Math.max(maxIssues, maxPRs, 1);
  const n = TREND_DATA.length;
  const ptIssues = TREND_DATA.map((d,i)=>[((i/(n-1||1))*W).toFixed(1), (H-((d.open_issues/maxY)*H)).toFixed(1)]);
  const ptPRs    = TREND_DATA.map((d,i)=>[((i/(n-1||1))*W).toFixed(1), (H-((d.open_prs/maxY)*H)).toFixed(1)]);
  const pathI = ptIssues.map((p,i)=>`${i===0?"M":"L"} ${p[0]} ${p[1]}`).join(" ");
  const pathP = ptPRs.map((p,i)=>`${i===0?"M":"L"} ${p[0]} ${p[1]}`).join(" ");
  const idxs = [0, Math.floor(n/4), Math.floor(n/2), Math.floor(3*n/4), n-1];
  const xLabels = idxs.map(i=>{
    if(i>=n) return "";
    const x = (i/(n-1||1))*W;
    return `<text x="${x.toFixed(1)}" y="${H+14}" fill="var(--muted)" font-size="9" text-anchor="middle">${TREND_DATA[i].date.slice(5)}</text>`;
  }).join("");
  el.innerHTML = `<svg width="100%" viewBox="0 0 ${W} ${H+18}" preserveAspectRatio="none" style="display:block;overflow:visible">
    <path d="${pathI}" fill="none" stroke="var(--stale)" stroke-width="1.5" stroke-linejoin="round" opacity="0.85"/>
    <path d="${pathP}" fill="none" stroke="#d2a8ff" stroke-width="1.5" stroke-linejoin="round" opacity="0.85"/>
    ${xLabels}
    <text x="4" y="10" fill="var(--muted)" font-size="9">${maxY}</text>
  </svg>`;
}

function updateSummary() {
  document.getElementById("hdr-total").textContent = DATA.length;
  document.getElementById("hdr-wg-count").textContent =
    DATA.filter(pr=>pr.wg).length + " with workgroup";
  document.getElementById("hdr-maint-count").textContent =
    DATA.filter(pr=>pr.is_maintainer).length + " maintainer PRs";
  document.getElementById("hdr-first-count").textContent =
    DATA.filter(pr=>pr.community_tier==="first-timer").length + " first-timers";
  document.getElementById("hdr-stale-count").textContent =
    DATA.filter(pr=>pr.days_stale>=30).length + " stale 30d+";
  document.getElementById("hdr-rfc-count").textContent =
    DATA.filter(pr=>pr.rfc_status==="required").length + " RFC required";
}

// ── wire up controls ──
document.getElementById("wg-select").addEventListener("change", e=>{
  state.wg = e.target.value; render();
});

function wireToggle(id, stateKey) {
  document.getElementById(id).addEventListener("click", function() {
    state[stateKey] = !state[stateKey];
    this.classList.toggle("on", state[stateKey]);
    render();
  });
}
wireToggle("tog-maintainer", "maintainer");
wireToggle("tog-first",      "firstTimer");
wireToggle("tog-lead",       "wgLead");
wireToggle("tog-rfc-required", "rfcRequired");
wireToggle("tog-suggested",    "suggested");

// Stale toggles are exclusive (picking one clears others)
const staleToggles = [
  {id:"tog-stale14", days:14},
  {id:"tog-stale30", days:30},
  {id:"tog-stale60", days:60},
];
for(const {id, days} of staleToggles) {
  document.getElementById(id).addEventListener("click", function() {
    const newDays = state.staleDays === days ? 0 : days;
    state.staleDays = newDays;
    staleToggles.forEach(t=>{
      document.getElementById(t.id).classList.toggle("on", newDays===t.days);
    });
    render();
  });
}

document.getElementById("clear-btn").addEventListener("click", ()=>{
  state.wg=""; state.maintainer=false; state.firstTimer=false;
  state.wgLead=false; state.staleDays=0; state.rfcRequired=false; state.suggested=false;
  document.getElementById("wg-select").value = "";
  document.querySelectorAll(".toggle").forEach(b=>b.classList.remove("on"));
  render();
});


const ACTIVITY = __ACTIVITY_DATA__;
let chartWindow = 10;

function renderActivityChart() {
  const data = ACTIVITY.slice(-chartWindow);
  const container = document.getElementById("chart-bars");
  const statsEl   = document.getElementById("chart-stats");
  if (!data.length) { container.innerHTML = '<span style="color:var(--muted);font-size:12px">No activity data</span>'; return; }

  const maxBar  = Math.max(...data.map(d => d.opened + d.merged + d.closed_nm), 1);
  const maxOpen = Math.max(...data.map(d => d.running_open), 1);
  const H = 116;

  // Stats summary
  const totalOpened = data.reduce((s,d) => s + d.opened, 0);
  const totalMerged = data.reduce((s,d) => s + d.merged, 0);
  const totalClosed = data.reduce((s,d) => s + d.closed_nm, 0);
  const netChange   = totalOpened - totalMerged - totalClosed;
  const currentOpen = data[data.length - 1].running_open;
  statsEl.innerHTML = `
    <div class="stat-box"><div class="num" style="color:#58a6ff">${totalOpened}</div><div class="lbl">opened</div></div>
    <div class="stat-box"><div class="num" style="color:#56d364">${totalMerged}</div><div class="lbl">merged</div></div>
    <div class="stat-box"><div class="num" style="color:#f85149">${totalClosed}</div><div class="lbl">closed (not merged)</div></div>
    <div class="stat-box"><div class="num" style="color:${netChange>0?"#f85149":netChange<0?"#56d364":"var(--muted)"}">${netChange>0?"+":""}${netChange}</div><div class="lbl">net backlog change</div></div>
    <div class="stat-box"><div class="num" style="color:var(--staleness)">${currentOpen}</div><div class="lbl">open now</div></div>
  `;

  // Bars
  let html = "";
  data.forEach((d, i) => {
    const total  = d.opened + d.merged + d.closed_nm;
    const hOpen  = total ? Math.max(Math.round((d.opened    / maxBar) * H), d.opened   ? 2 : 0) : 0;
    const hMerge = total ? Math.max(Math.round((d.merged    / maxBar) * H), d.merged   ? 2 : 0) : 0;
    const hClose = total ? Math.max(Math.round((d.closed_nm / maxBar) * H), d.closed_nm? 2 : 0) : 0;
    const label  = d.date.slice(5);
    html += `<div class="chart-bar-group">
      <div class="chart-segments" style="height:${H}px">
        ${hClose ? `<div class="bar-seg bar-closed" style="height:${hClose}px"></div>` : ""}
        ${hMerge ? `<div class="bar-seg bar-merged" style="height:${hMerge}px"></div>` : ""}
        ${hOpen  ? `<div class="bar-seg bar-opened" style="height:${hOpen}px"></div>`  : ""}
      </div>
      <div class="chart-tooltip">
        <div><strong>${d.date}</strong></div>
        <div style="color:#58a6ff">&#9650; Opened: ${d.opened}</div>
        <div style="color:#56d364">&#10003; Merged: ${d.merged}</div>
        <div style="color:#f85149">&#10005; Closed: ${d.closed_nm}</div>
        <div style="color:var(--staleness)">&#9679; Open backlog: ${d.running_open}</div>
      </div>
      <div class="chart-date">${label}</div>
    </div>`;
  });
  container.innerHTML = html;

  // SVG open-backlog line
  requestAnimationFrame(() => {
    const wrap = container;
    const W = wrap.offsetWidth;
    if (!W) return;
    const barW = W / data.length;
    const pts  = data.map((d, i) =>
      `${((i + 0.5) * barW).toFixed(1)},${(H - Math.round((d.running_open / maxOpen) * H)).toFixed(1)}`
    ).join(" ");
    // Remove old SVG if any
    wrap.querySelectorAll("svg").forEach(s => s.remove());
    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("width", W);
    svg.setAttribute("height", H);
    svg.style.cssText = "position:absolute;top:0;left:0;pointer-events:none;overflow:visible";
    svg.innerHTML = `
      <polyline points="${pts}" fill="none" stroke="var(--staleness)" stroke-width="2" stroke-linejoin="round" opacity="0.9"/>
      ${data.map((d, i) => {
        const x = ((i + 0.5) * barW).toFixed(1);
        const y = (H - Math.round((d.running_open / maxOpen) * H)).toFixed(1);
        return `<circle cx="${x}" cy="${y}" r="3" fill="var(--staleness)" opacity="0.9"/>`;
      }).join("")}
    `;
    wrap.style.position = "relative";
    wrap.appendChild(svg);
  });
}


function renderWGBreakdown() {
  const el = document.getElementById("wg-breakdown");
  // Count open PRs per WG from current DATA
  const counts = {};
  let unclaimed = 0;
  for (const pr of DATA) {
    if (pr.wg) counts[pr.wg] = (counts[pr.wg] || 0) + 1;
    else unclaimed++;
  }
  const entries = Object.entries(counts).sort((a,b) => b[1]-a[1]);
  if (unclaimed) entries.push(["Unclaimed", unclaimed]);
  const max = Math.max(...entries.map(e=>e[1]), 1);
  const COLORS = ["#58a6ff","#56d364","#79c0ff","#ffa657","#d2a8ff","#a5d6ff"];
  let html = "";
  entries.forEach(([wg, count], i) => {
    const pct = Math.round((count / max) * 100);
    const color = wg === "Unclaimed" ? "var(--border)" : COLORS[i % COLORS.length];
    html += `<div class="wg-bar-row">
      <div class="wg-bar-label" title="${esc(wg)}">${esc(wg)}</div>
      <div class="wg-bar-track">
        <div class="wg-bar-fill" style="width:${pct}%;background:${color}"></div>
        <span class="wg-bar-count">${count}</span>
      </div>
    </div>`;
  });
  if (!entries.length) html = '<div style="color:var(--muted);font-size:12px">No WG data</div>';
  el.innerHTML = html;
}

function renderCumulativeChart() {
  const data = ACTIVITY.slice(-chartWindow);
  const el = document.getElementById("cumulative-chart");
  document.getElementById("cumul-window").textContent = `— last ${chartWindow}d`;
  if (!data.length) { el.innerHTML = ""; return; }

  const W = el.getBoundingClientRect().width || el.parentElement.getBoundingClientRect().width || 300;
  const H = 100;
  const maxOpen = Math.max(...data.map(d => d.running_open), 1);
  const minOpen = Math.min(...data.map(d => d.running_open));
  // Use a padded range so the line isn't squashed at top/bottom
  const range = Math.max(maxOpen - minOpen, 1);
  const pad = Math.ceil(range * 0.15);
  const lo = Math.max(minOpen - pad, 0);
  const hi = maxOpen + pad;

  const pts = data.map((d, i) => {
    const x = (i / (data.length - 1 || 1)) * W;
    const y = H - ((d.running_open - lo) / (hi - lo)) * H;
    return [x.toFixed(1), y.toFixed(1)];
  });

  // Gradient fill under the line
  const areaPath = [
    `M ${pts[0][0]} ${H}`,
    ...pts.map(p => `L ${p[0]} ${p[1]}`),
    `L ${pts[pts.length-1][0]} ${H}`,
    "Z"
  ].join(" ");

  const linePath = pts.map((p,i) => `${i===0?"M":"L"} ${p[0]} ${p[1]}`).join(" ");

  // Y axis labels
  const yLabels = [lo, Math.round((lo+hi)/2), hi].map((v,i) => {
    const y = H - ((v - lo) / (hi - lo)) * H;
    return `<text x="0" y="${y.toFixed(1)}" fill="var(--muted)" font-size="9" dominant-baseline="middle">${v}</text>`;
  }).join("");

  // X axis dates
  const xLabels = [0, Math.floor(data.length/2), data.length-1].map(i => {
    if (i >= data.length) return "";
    const x = (i / (data.length - 1 || 1)) * W;
    return `<text x="${x.toFixed(1)}" y="${H+12}" fill="var(--muted)" font-size="9" text-anchor="middle">${data[i].date.slice(5)}</text>`;
  }).join("");

  el.innerHTML = `<svg width="100%" viewBox="0 0 ${W} ${H+18}" preserveAspectRatio="none" style="display:block">
    <defs>
      <linearGradient id="cg" x1="0" y1="0" x2="0" y2="1">
        <stop offset="0%" stop-color="var(--staleness)" stop-opacity="0.3"/>
        <stop offset="100%" stop-color="var(--staleness)" stop-opacity="0.02"/>
      </linearGradient>
    </defs>
    <path d="${areaPath}" fill="url(#cg)"/>
    <path d="${linePath}" fill="none" stroke="var(--staleness)" stroke-width="2" stroke-linejoin="round"/>
    ${pts.map(p=>`<circle cx="${p[0]}" cy="${p[1]}" r="2.5" fill="var(--staleness)"/>`).join("")}
    ${yLabels}
    ${xLabels}
  </svg>`;
}

// Slider wiring
const windowSlider = document.getElementById("day-window");
const windowLabel  = document.getElementById("day-window-label");
windowSlider.addEventListener("input", () => {
  chartWindow = parseInt(windowSlider.value);
  windowLabel.textContent = chartWindow + (chartWindow === 1 ? " day" : " days");
  renderActivityChart();
renderWGBreakdown();
setTimeout(() => renderCumulativeChart(), 50);
  renderCumulativeChart();
});

updateSummary();
renderActivityChart();
renderWGBreakdown();
setTimeout(() => renderCumulativeChart(), 50);
setTimeout(() => renderTrendChart(), 60);
renderPulse();
renderNewPRs();
renderFocusPanel();
renderRFCPanel();
render();
</script>
</body>
</html>
"""


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument(
        "--output", default="triage_maintainer.html", help="Output HTML path"
    )
    p.add_argument("--repo", default="lemonade-sdk/lemonade", help="OWNER/REPO")
    p.add_argument(
        "--wg-predictions",
        help='JSON {"<pr>": ["<WG>", "<reason>"]}; fills WG for PRs with no '
        "wg:: label or declared WG, shown as (auto)",
    )
    p.add_argument(
        "--reviewer-suggestions",
        help="JSON written by auto_assign.py --json; shown as suggested reviewers",
    )
    p.add_argument(
        "--include-rfcs",
        action="store_true",
        help="Fetch GitHub Discussions RFCs and render the RFC panel",
    )
    args = p.parse_args()
    wg_predictions = load_wg_predictions(args.wg_predictions)
    reviewer_suggestions = {}
    if args.reviewer_suggestions:
        raw = Path(args.reviewer_suggestions).read_text(encoding="utf-8")
        reviewer_suggestions = {int(k): v for k, v in json.loads(raw).items()}

    print("Fetching top contributors…", file=sys.stderr)
    top_contributors = fetch_top_contributors(args.repo)

    print(f"Fetching open PRs from {args.repo}…", file=sys.stderr)
    prs = fetch_prs(args.repo, top_contributors, wg_predictions)
    community = sum(1 for pr in prs if not pr["is_maintainer"])
    print(f"  fetched {len(prs)} open PRs ({community} community)", file=sys.stderr)
    sources = {}
    for pr in prs:
        sources[pr["wg_source"] or "none"] = (
            sources.get(pr["wg_source"] or "none", 0) + 1
        )
    print(f"  WG sources: {sources}", file=sys.stderr)
    for pr in prs:
        pr["suggested_reviewers"] = reviewer_suggestions.get(pr["number"], [])

    print("Fetching 30-day activity…", file=sys.stderr)
    activity = fetch_activity(args.repo, days=30)
    print(f"  built {len(activity)} days of activity data", file=sys.stderr)

    print("Building 90-day issue/PR trend…", file=sys.stderr)
    trend = fetch_issue_trend(args.repo, days=90)
    print(f"  built {len(trend)} data points", file=sys.stderr)

    focus = build_focus_data(prs)
    focus_wg_count = sum(1 for v in focus.values() if v)
    print(
        f"  focus panel: {focus_wg_count} workgroups with highlights", file=sys.stderr
    )

    rfcs = []
    if args.include_rfcs:
        print("Fetching RFCs…", file=sys.stderr)
        rfcs = fetch_rfcs(args.repo)
        print(f"  found {len(rfcs)} open RFCs", file=sys.stderr)

    print("Building weekly pulse…", file=sys.stderr)
    pulse = fetch_weekly_pulse(args.repo, wg_predictions)
    print(
        f"  pulse: {pulse['merged_count']} merged, "
        f"{pulse['new_prs_count']} new PRs, "
        f"{pulse['new_issues_count']} new issues last 7d",
        file=sys.stderr,
    )

    wg_options = chr(10).join(
        f'      <option value="{wg}">{wg} (lead: {WG_LEADS[wg]})</option>'
        for wg in ACTIVE_WGS
    )
    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    html = (
        MAINTAINER_TEMPLATE.replace("__REPO__", args.repo)
        .replace("__GENERATED_AT__", now)
        .replace("__DATA__", script_safe_json(prs))
        .replace("__ALL_WGS__", script_safe_json(ACTIVE_WGS))
        .replace("__WG_OPTIONS__", wg_options)
        .replace("__ACTIVITY_DATA__", script_safe_json(activity))
        .replace("__FOCUS_DATA__", script_safe_json(focus))
        .replace("__RFC_DATA__", script_safe_json(rfcs))
        .replace("__TREND_DATA__", script_safe_json(trend))
        .replace("__PULSE_DATA__", script_safe_json(pulse))
    )

    out = Path(args.output)
    out.write_text(html, encoding="utf-8")
    print(f"Wrote {out.resolve()} ({len(html):,} chars)", file=sys.stderr)


if __name__ == "__main__":
    main()
