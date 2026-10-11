#!/usr/bin/env python3
"""Generate a self-contained triage dashboard for lemonade-sdk/lemonade.

Fetches all open issues + PRs via `gh api`, then writes a single HTML
file with histograms per label family (engine::, area::, runtime::,
priority::, type, component) and click-to-drill-down item lists with
multi-label AND filtering.

Also supports workgroup grouping, staleness buckets, contributor tiers,
and RFC tracking from GitHub Discussions.

Usage:
    python .github/scripts/triage_dashboard.py [--output PATH] [--repo OWNER/REPO] [--include-discussions]

Requirements:
    - gh CLI authenticated
    - Python 3.9+ (stdlib only)

Open the resulting file directly in a browser. No build step, no
external assets, no auth required at view time — data is embedded.
Re-run the script any time to refresh.
"""

import argparse
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

PREFIX_FAMILIES = ("engine::", "area::", "runtime::", "priority::")
TYPE_LABELS = {"bug", "enhancement", "documentation", "question"}
COMPONENT_LABELS = {"cpp", "app", "web ui", "audio"}

WRITE_ASSOCIATIONS = {"OWNER", "MEMBER", "COLLABORATOR"}

STALENESS_BUCKETS = [
    (60, "stale::60d+"),
    (30, "stale::30d"),
    (14, "stale::14d"),
]

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
}

RFC_CATEGORY = "Request for Comment (RFC)"


def gh_api_pages(path):
    cmd = ["gh", "api", "--paginate", "--slurp", path]
    out = subprocess.run(
        cmd, check=True, capture_output=True, text=True, encoding="utf-8"
    ).stdout
    pages = json.loads(out)
    return [item for page in pages for item in page]


def parse_wg(body):
    """Extract working group name from a PR body.

    Tries the PR-template checkbox pattern first, then scans for canonical
    WG name substrings as a fallback for PRs that mention the WG in prose.
    Returns a canonical WG name string or None.
    """
    if not body:
        return None
    m = re.search(r"scope of WG:\s*([^\n\r<]{1,60})", body, re.IGNORECASE)
    if m:
        candidate = m.group(1).strip()
        return CANONICAL_WGS.get(candidate.lower(), candidate)
    body_lower = body.lower()
    for key in sorted(CANONICAL_WGS, key=len, reverse=True):
        if key in body_lower:
            return CANONICAL_WGS[key]
    return None


def staleness_bucket(days):
    for threshold, label in STALENESS_BUCKETS:
        if days >= threshold:
            return label
    return "active"


def contributor_tier(author_association):
    """Map GitHub author_association to a three-tier contributor label."""
    if author_association in WRITE_ASSOCIATIONS:
        return "maintainer"
    if author_association == "FIRST_TIME_CONTRIBUTOR":
        return "first-timer"
    return "returning"


def compute_priority_score(item):
    """Compute a review-urgency score for the PR Focus panel."""
    score = 0
    labels = item.get("labels", [])
    days_stale = item.get("days_stale", 0)
    if any("hot" in lbl for lbl in labels if lbl.startswith("priority::")):
        score += 1000
    elif any("warm" in lbl for lbl in labels if lbl.startswith("priority::")):
        score += 500
    if days_stale >= 60:
        score += 300
    elif days_stale >= 30:
        score += 200
    elif days_stale >= 14:
        score += 100
    if not item.get("assignees"):
        score += 150
    if item.get("contributor_tier") == "first-timer":
        score += 75
    score += min(item.get("comments", 0) * 5, 100)
    return score


def fetch_items(repo):
    now = datetime.now(timezone.utc)
    raw = gh_api_pages(f"/repos/{repo}/issues?state=open&per_page=100")
    items = []
    for it in raw:
        updated = datetime.fromisoformat(it["updated_at"].replace("Z", "+00:00"))
        days = (now - updated).days
        assoc = it.get("author_association", "NONE")
        body = it.get("body") or ""
        is_pr = it.get("pull_request") is not None
        wg = parse_wg(body) if is_pr else None
        tier = contributor_tier(assoc)
        item = {
            "number": it["number"],
            "title": it["title"],
            "url": it["html_url"],
            "author": (it.get("user") or {}).get("login", "?"),
            "labels": [lbl["name"] for lbl in it.get("labels", [])],
            "assignees": [
                a["login"] for a in it.get("assignees", []) if a and a.get("login")
            ],
            "comments": it.get("comments", 0),
            "is_pr": is_pr,
            "created_at": it["created_at"],
            "updated_at": it["updated_at"],
            "days_stale": days,
            "staleness_bucket": staleness_bucket(days),
            "wg": wg,
            "contributor_tier": tier,
            "is_draft": bool(it.get("draft", False)),
        }
        item["priority_score"] = compute_priority_score(item)
        items.append(item)
    return items


def fetch_rfcs(repo):
    """Fetch open Discussions in the RFC category. Returns [] on any error."""
    try:
        raw = gh_api_pages(f"/repos/{repo}/discussions?per_page=100")
    except Exception as exc:
        print(
            f"  [warn] discussions API failed ({exc}); skipping RFCs", file=sys.stderr
        )
        return []

    now = datetime.now(timezone.utc)
    rfcs = []
    for d in raw:
        if (d.get("category") or {}).get("name") != RFC_CATEGORY:
            continue
        if d.get("state", "open") != "open":
            continue
        updated = datetime.fromisoformat(d["updated_at"].replace("Z", "+00:00"))
        days = (now - updated).days
        label_names = [lbl["name"] for lbl in (d.get("labels") or [])]
        rfc_labels = [lbl for lbl in label_names if lbl.startswith("rfc:")]
        status = rfc_labels[0].replace("rfc:", "") if rfc_labels else "new"
        rfcs.append(
            {
                "number": d["number"],
                "title": d["title"],
                "url": d["html_url"],
                "author": (d.get("user") or {}).get("login", "?"),
                "labels": label_names,
                "comments": d.get("comments", 0),
                "created_at": d["created_at"],
                "updated_at": d["updated_at"],
                "days_stale": days,
                "rfc_status": status,
            }
        )
    rfcs.sort(key=lambda r: r["updated_at"], reverse=True)
    return rfcs


def family_of(label):
    for prefix in PREFIX_FAMILIES:
        if label.startswith(prefix):
            return prefix
    if label in TYPE_LABELS:
        return "type"
    if label in COMPONENT_LABELS:
        return "component"
    return "other"


def script_safe_json(obj):
    """Serialize obj as JSON safe to embed inside an HTML <script> block.

    json.dumps alone is not safe here: a string such as "</script>" would
    close the script tag and let the rest of the value be parsed as HTML,
    enabling injection. We escape <, >, and & so no markup can break out of
    the script context, and U+2028 / U+2029 which are valid in JSON strings
    but illegal as raw characters in JavaScript string literals.
    """
    return (
        json.dumps(obj)
        .replace("<", "\\u003c")
        .replace(">", "\\u003e")
        .replace("&", "\\u0026")
        .replace(" ", "\\u2028")
        .replace(" ", "\\u2029")
    )


HTML_TEMPLATE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Lemonade triage — __REPO__</title>
<style>
  /* See MAINTAINER_TEMPLATE in triage_maintainer.py for the companion page styles. */
  :root {
    --bg: #0d1117;
    --panel: #161b22;
    --panel-2: #21262d;
    --border: #30363d;
    --fg: #e6edf3;
    --muted: #7d8590;
    --link: #58a6ff;
    --engine: #d7f086;
    --area: #c5def5;
    --runtime: #fbca04;
    --priority: #b60205;
    --type: #d876e3;
    --component: #a371f7;
    --other: #6e7681;
    --unlabeled: #f85149;
    --wg: #79c0ff;
    --staleness: #ffa657;
    --tier: #56d364;
    --hot-bg: #3d1a1a;
    --draft: #484f58;
  }
  * { box-sizing: border-box; }
  body { font: 14px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI", system-ui, sans-serif; margin: 0; background: var(--bg); color: var(--fg); }
  a { color: var(--link); text-decoration: none; }
  a:hover { text-decoration: underline; }
  header { padding: 16px 24px; border-bottom: 1px solid var(--border); display: flex; justify-content: space-between; align-items: baseline; gap: 24px; }
  header h1 { margin: 0; font-size: 18px; font-weight: 600; }
  .stats { color: var(--muted); font-size: 13px; }
  .stats strong { color: var(--fg); }
  .panel-heading { font-size: 12px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; color: var(--muted); margin: 0 0 8px; }
  .section-band { padding: 10px 24px; border-bottom: 1px solid var(--border); }
  #summary-card { display: flex; gap: 12px; flex-wrap: wrap; align-items: center; background: var(--panel); font-size: 13px; }
  #summary-card .card-label { font-size: 11px; }
  .stat-pill { padding: 4px 10px; border-radius: 12px; border: 1px solid var(--border); font-variant-numeric: tabular-nums; cursor: pointer; font-size: 12.5px; background: var(--panel-2); }
  .stat-pill:hover { border-color: var(--link); color: var(--link); }
  .stat-pill.urgent { border-color: var(--staleness); color: var(--staleness); }
  main { display: grid; grid-template-columns: minmax(360px, 480px) 1fr; gap: 0; min-height: calc(100vh - 100px); }
  #families { padding: 16px 24px; border-right: 1px solid var(--border); overflow-y: auto; max-height: calc(100vh - 100px); }
  .family { margin-bottom: 24px; }
  .family h2 { font-size: 12px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; color: var(--muted); margin: 0 0 8px; display: flex; align-items: center; gap: 8px; }
  .family h2 .swatch { width: 10px; height: 10px; border-radius: 2px; display: inline-block; }
  .family h2 .chevron { display: inline-block; width: 10px; color: var(--muted); font-size: 10px; }
  .family h2 .family-count { color: var(--muted); font-weight: 500; font-variant-numeric: tabular-nums; margin-left: 4px; }
  .family.collapsible h2:hover { color: var(--fg); }
  .label-row { display: grid; grid-template-columns: 1fr 36px; gap: 8px; align-items: center; padding: 4px 6px; border-radius: 4px; cursor: pointer; margin: 2px 0; }
  .label-row:hover { background: var(--panel); }
  .label-row.active { background: var(--panel-2); outline: 1px solid var(--link); }
  .label-name { font-family: ui-monospace, "SF Mono", Menlo, monospace; font-size: 12.5px; position: relative; padding-left: 4px; }
  .label-name .bar { position: absolute; left: 0; right: 0; bottom: -2px; height: 2px; border-radius: 2px; }
  .label-count { text-align: right; font-variant-numeric: tabular-nums; color: var(--muted); font-size: 12.5px; }
  #drilldown { padding: 16px 24px; overflow-y: auto; max-height: calc(100vh - 100px); }
  #filter-bar { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; margin-bottom: 16px; }
  #filter-bar .filter-pill { display: inline-flex; align-items: center; gap: 6px; padding: 4px 8px; background: var(--panel); border: 1px solid var(--border); border-radius: 12px; font-family: ui-monospace, monospace; font-size: 12px; }
  #filter-bar .filter-pill button { background: none; border: none; color: var(--muted); cursor: pointer; padding: 0; font-size: 14px; line-height: 1; }
  #filter-bar .filter-pill button:hover { color: var(--fg); }
  #filter-bar .clear-all { background: none; border: 1px solid var(--border); color: var(--muted); padding: 4px 10px; border-radius: 4px; cursor: pointer; font-size: 12px; }
  #filter-bar .clear-all:hover { color: var(--fg); border-color: var(--fg); }
  #filter-bar .hint { color: var(--muted); font-size: 13px; }
  #date-filter { display: flex; gap: 12px; align-items: center; flex-wrap: wrap; margin-bottom: 12px; padding: 8px 12px; background: var(--panel); border: 1px solid var(--border); border-radius: 6px; font-size: 12.5px; }
  #date-filter label { display: inline-flex; align-items: center; gap: 6px; color: var(--muted); }
  #date-filter input[type="date"] { background: var(--bg); color: var(--fg); border: 1px solid var(--border); border-radius: 4px; padding: 3px 6px; font: inherit; color-scheme: dark; }
  #date-filter button { background: none; border: 1px solid var(--border); color: var(--muted); padding: 4px 10px; border-radius: 4px; cursor: pointer; font-size: 12px; }
  #date-filter button:hover { color: var(--fg); border-color: var(--fg); }
  .item-assignees { color: var(--muted); font-size: 12px; margin-top: 4px; }
  .item-assignees strong { color: var(--fg); font-weight: 500; }
  #items-meta { color: var(--muted); font-size: 13px; margin-bottom: 12px; }
  .item { padding: 10px 12px; border: 1px solid var(--border); border-radius: 6px; margin-bottom: 8px; background: var(--panel); }
  .item-head { display: flex; gap: 8px; align-items: baseline; flex-wrap: wrap; }
  .item-num { color: var(--muted); font-variant-numeric: tabular-nums; }
  .item-kind { font-size: 11px; padding: 1px 6px; border-radius: 10px; color: var(--bg); font-weight: 600; }
  .item-kind.issue { background: #3fb950; }
  .item-kind.pr { background: #a371f7; }
  .item-title { flex: 1; min-width: 0; font-weight: 500; }
  .item-title a { color: var(--fg); }
  .item-meta { color: var(--muted); font-size: 12px; margin-top: 4px; display: flex; gap: 12px; flex-wrap: wrap; }
  .item-extra { display: flex; gap: 8px; flex-wrap: wrap; margin-top: 4px; align-items: center; }
  .item-wg { color: var(--wg); font-size: 11.5px; }
  .item-tier { font-size: 11px; padding: 1px 6px; border-radius: 8px; }
  .item-tier.first-timer { background: #1f6feb30; border: 1px solid #1f6feb; color: #79c0ff; }
  .item-labels { display: flex; gap: 4px; flex-wrap: wrap; margin-top: 6px; }
  .item-labels span { font-family: ui-monospace, monospace; font-size: 11px; padding: 1px 6px; border-radius: 10px; background: var(--bg); border: 1px solid var(--border); color: var(--muted); cursor: pointer; }
  .item-labels span:hover { color: var(--fg); border-color: var(--link); }
  .empty { color: var(--muted); text-align: center; padding: 48px 16px; }
  #pr-focus { margin-bottom: 16px; }
  #pr-focus h2 { }
  .focus-divider { border: none; border-top: 1px solid var(--border); margin: 0 0 16px; }
  .focus-item { padding: 8px 10px; border: 1px solid var(--border); border-radius: 6px; margin-bottom: 6px; background: var(--panel); }
  .focus-item.is-hot { border-color: #b60205; background: var(--hot-bg); }
  .focus-item-title { font-weight: 500; font-size: 13px; margin-bottom: 4px; }
  .focus-item-title a { color: var(--fg); }
  .focus-meta { color: var(--muted); font-size: 11.5px; display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
  .badge { display: inline-block; padding: 1px 6px; border-radius: 8px; font-size: 11px; font-weight: 600; }
  .badge.draft { background: var(--draft); color: var(--fg); }
  .badge.first-timer { background: #1f6feb; color: #fff; }
  .badge.stale { background: var(--staleness); color: #000; }
  .badge.hot { background: #b60205; color: #fff; }
  .badge.warm { background: #e3a000; color: #000; }
  .badge.wg { background: #1f3a5f; border: 1px solid var(--wg); color: var(--wg); font-weight: 500; }
  #rfc-panel { border-top: 1px solid var(--border); }
  #rfc-panel summary { cursor: pointer; font-size: 13px; list-style: none; display: flex; align-items: center; gap: 8px; }
  #rfc-panel summary::-webkit-details-marker { display: none; }
  #rfc-panel summary::before { content: "▸"; font-size: 10px; }
  #rfc-panel[open] summary::before { content: "▾"; }
  #rfc-panel summary:hover { color: var(--fg); }
  #rfc-panel[open] summary { border-bottom: 1px solid var(--border); }
  #rfc-list { display: grid; grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 12px; padding: 16px 24px; }
  .rfc-card { padding: 10px 12px; border: 1px solid var(--border); border-radius: 6px; background: var(--panel); }
  .rfc-card-title { font-weight: 500; margin-bottom: 6px; font-size: 13px; }
  .rfc-card-title a { color: var(--fg); }
  .rfc-card-meta { color: var(--muted); font-size: 12px; display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
  .rfc-status { padding: 2px 7px; border-radius: 8px; font-size: 11px; font-weight: 600; }
  .rfc-status.open-for-comment { background: #1f6feb20; border: 1px solid #1f6feb; color: #79c0ff; }
  .rfc-status.on-roadmap { background: #23863620; border: 1px solid #2ea043; color: #56d364; }
  .rfc-status.new { background: var(--panel-2); border: 1px solid var(--border); color: var(--muted); }
  .footer { color: var(--muted); font-size: 12px; padding: 8px 24px; border-top: 1px solid var(--border); text-align: center; }

  .site-nav { display:flex; gap:0; background:var(--panel-2); border-bottom:1px solid var(--border); padding:0 24px; }
  .site-nav a { color:var(--muted); font-size:13px; font-weight:500; padding:10px 18px; border-bottom:2px solid transparent; text-decoration:none; white-space:nowrap; }
  .site-nav a:hover { color:var(--fg); text-decoration:none; }
  .site-nav a.active { color:var(--fg); border-bottom-color:var(--link); }
</style>
</head>
<body>
<header>
  <h1>Triage — <span style="color: var(--muted);">__REPO__</span></h1>
  <div class="stats">
    <strong id="total">0</strong> open
    (<span id="total-issues">0</span> issues · <span id="total-prs">0</span> PRs) ·
    generated <strong>__GENERATED_AT__</strong>
  </div>
</header>
<div id="summary-card" class="section-band"></div>
<main>
  <aside id="families">
    <div id="pr-focus"></div>
    <hr class="focus-divider" id="focus-divider" style="display:none">
  </aside>
  <section id="drilldown">
    <div id="date-filter">
      <label>From <input type="date" id="date-from"></label>
      <label>To <input type="date" id="date-to"></label>
      <button id="date-clear" type="button">Clear dates</button>
      <span class="hint" id="date-hint" style="color: var(--muted);">Filters items by creation date.</span>
    </div>
    <div id="filter-bar"></div>
    <div id="items-meta"></div>
    <div id="items"></div>
  </section>
</main>
<details id="rfc-panel">
  <summary class="panel-heading section-band">Open RFCs <span id="rfc-count" style="opacity:0.7;font-weight:400"></span></summary>
  <div id="rfc-list"></div>
</details>
<div class="footer">Re-run <code>python .github/scripts/triage_dashboard.py</code> to refresh.</div>
<script>
const DATA = __DATA__;
const RFC_DATA = __RFC_DATA__;
const PR_FOCUS_TOP_N = 10;

const FAMILY_ORDER = ["wg::", "staleness::", "contributor::", "engine::", "area::", "runtime::", "priority::", "type", "component", "other", "__unlabeled__", "assignee::"];
const FAMILY_META = {
  "wg::":          {name: "Workgroup",    color: "var(--wg)",        description: "Working group scope declared in PR body"},
  "staleness::":   {name: "Staleness",    color: "var(--staleness)", description: "Days since last activity"},
  "contributor::": {name: "Contributor",  color: "var(--tier)",      description: "First-timer · returning · maintainer"},
  "assignee::":    {name: "Assignee",     color: "var(--link)",      description: "Assigned vs. unassigned · click a user to filter"},
  "engine::":      {name: "Engine",       color: "var(--engine)",    description: "Backend / inference engine"},
  "area::":        {name: "Area",         color: "var(--area)",      description: "Component / surface area"},
  "runtime::":     {name: "Runtime",      color: "var(--runtime)",   description: "GPU/compute runtime"},
  "priority::":    {name: "Priority",     color: "var(--priority)",  description: "Engagement-driven heat"},
  "type":          {name: "Type",         color: "var(--type)",      description: "Bug / feature / docs / question"},
  "component":     {name: "Component",    color: "var(--component)", description: "Pre-existing component labels"},
  "other":         {name: "Other",        color: "var(--other)",     description: "Anything else applied to items"},
  "__unlabeled__": {name: "Unlabeled",    color: "var(--unlabeled)", description: "Items with no labels at all"},
};
const ASSIGNEE_UNASSIGNED = "assignee::__none__";

const TYPE_LABELS = new Set(["bug", "enhancement", "documentation", "question"]);
const COMPONENT_LABELS = new Set(["cpp", "app", "web ui", "audio"]);

const STALENESS_ORDER = ["stale::60d+", "stale::30d", "stale::14d", "active"];

function familyOf(label) {
  for (const prefix of ["engine::", "area::", "runtime::", "priority::"]) {
    if (label.startsWith(prefix)) return prefix;
  }
  if (TYPE_LABELS.has(label)) return "type";
  if (COMPONENT_LABELS.has(label)) return "component";
  return "other";
}

function buildFamilies(items) {
  const fams = {};
  for (const f of FAMILY_ORDER) fams[f] = {};
  for (const it of items) {
    // WG (PRs only)
    if (it.is_pr && it.wg) {
      const key = "wg::" + it.wg;
      fams["wg::"][key] = (fams["wg::"][key] || 0) + 1;
    }
    // Staleness
    const sb = it.staleness_bucket || "active";
    fams["staleness::"][sb] = (fams["staleness::"][sb] || 0) + 1;
    // Contributor tier (PRs only)
    if (it.is_pr) {
      const tier = "contributor::" + (it.contributor_tier || "returning");
      fams["contributor::"][tier] = (fams["contributor::"][tier] || 0) + 1;
    }
    // Assignee
    const assignees = it.assignees || [];
    if (assignees.length === 0) {
      fams["assignee::"][ASSIGNEE_UNASSIGNED] = (fams["assignee::"][ASSIGNEE_UNASSIGNED] || 0) + 1;
    } else {
      for (const login of assignees) {
        const key = "assignee::" + login;
        fams["assignee::"][key] = (fams["assignee::"][key] || 0) + 1;
      }
    }
    // Labels / unlabeled
    if (it.labels.length === 0) {
      fams["__unlabeled__"]["__unlabeled__"] = (fams["__unlabeled__"]["__unlabeled__"] || 0) + 1;
      continue;
    }
    for (const lbl of it.labels) {
      const f = familyOf(lbl);
      fams[f][lbl] = (fams[f][lbl] || 0) + 1;
    }
  }
  return fams;
}

let activeFilters = new Set();
let dateFrom = null;
let dateTo = null;
const collapsedFamilies = new Set(["assignee::", "contributor::"]);
const families = buildFamilies(DATA);

function prettyFilterName(f) {
  if (f === "__unlabeled__") return "(no labels)";
  if (f === ASSIGNEE_UNASSIGNED) return "unassigned";
  if (f.startsWith("assignee::")) return "@" + f.slice("assignee::".length);
  if (f.startsWith("wg::")) return f.slice("wg::".length);
  if (f.startsWith("contributor::")) return f.slice("contributor::".length);
  if (f === "active") return "active (< 14d)";
  if (f === "stale::14d") return "stale 14–30d";
  if (f === "stale::30d") return "stale 30–60d";
  if (f === "stale::60d+") return "stale 60d+";
  return f;
}

function dateInRange(iso) {
  if (!dateFrom && !dateTo) return true;
  const day = iso.slice(0, 10);
  if (dateFrom && day < dateFrom) return false;
  if (dateTo && day > dateTo) return false;
  return true;
}

function hasAnyFilter() {
  return activeFilters.size > 0 || dateFrom || dateTo;
}

function renderFamilies() {
  const wrap = document.getElementById("families");
  // Preserve pr-focus and divider; rebuild everything after them
  const focus = document.getElementById("pr-focus");
  const divider = document.getElementById("focus-divider");
  wrap.innerHTML = "";
  wrap.appendChild(focus);
  wrap.appendChild(divider);

  for (const fname of FAMILY_ORDER) {
    const labels = families[fname];
    if (Object.keys(labels).length === 0) continue;
    const meta = FAMILY_META[fname];
    // Sort staleness by logical order, everything else by count desc
    let entries;
    if (fname === "staleness::") {
      entries = STALENESS_ORDER.filter(k => labels[k] !== undefined).map(k => [k, labels[k]]);
    } else {
      entries = Object.entries(labels).sort((a, b) => b[1] - a[1]);
    }
    const total = entries.reduce((s, e) => s + e[1], 0);
    const max = Math.max(...entries.map(e => e[1]));
    const collapsible = fname === "assignee::" || fname === "contributor::";
    const isCollapsed = collapsible && collapsedFamilies.has(fname);
    const section = document.createElement("div");
    section.className = "family" + (collapsible ? " collapsible" : "");
    const chevron = collapsible ? `<span class="chevron">${isCollapsed ? "▸" : "▾"}</span>` : "";
    const countBadge = collapsible ? `<span class="family-count">${total}</span>` : "";
    section.innerHTML = `<h2${collapsible ? ' style="cursor:pointer; user-select:none;"' : ""}>${chevron}<span class="swatch" style="background:${meta.color}"></span>${meta.name}${countBadge} <span style="font-weight:400; text-transform:none; letter-spacing:0; opacity:0.7;">— ${meta.description}</span></h2>`;
    if (collapsible) {
      section.querySelector("h2").addEventListener("click", () => {
        if (collapsedFamilies.has(fname)) collapsedFamilies.delete(fname);
        else collapsedFamilies.add(fname);
        renderFamilies();
      });
    }
    if (!isCollapsed) {
      for (const [lbl, count] of entries) {
        const row = document.createElement("div");
        row.className = "label-row" + (activeFilters.has(lbl) ? " active" : "");
        const pct = (count / max * 100).toFixed(0);
        const displayName = prettyFilterName(lbl);
        row.innerHTML = `
          <div class="label-name">${escape(displayName)}<div class="bar" style="background:${meta.color}; width:${pct}%"></div></div>
          <div class="label-count">${count}</div>
        `;
        row.addEventListener("click", () => toggleFilter(lbl));
        section.appendChild(row);
      }
    }
    wrap.appendChild(section);
  }
}

function toggleFilter(label) {
  if (activeFilters.has(label)) activeFilters.delete(label);
  else activeFilters.add(label);
  render();
}

function matchesFilters(item) {
  if (!hasAnyFilter()) return false;
  if (!dateInRange(item.created_at)) return false;
  const assigneeFilters = [], labelFilters = [], wgFilters = [], staleFilters = [], tierFilters = [];
  for (const f of activeFilters) {
    if (f === ASSIGNEE_UNASSIGNED || f.startsWith("assignee::")) assigneeFilters.push(f);
    else if (f.startsWith("wg::")) wgFilters.push(f);
    else if (f === "active" || f.startsWith("stale::")) staleFilters.push(f);
    else if (f.startsWith("contributor::")) tierFilters.push(f);
    else labelFilters.push(f);
  }
  if (wgFilters.length > 0) {
    const wgNames = wgFilters.map(f => f.slice("wg::".length));
    if (!wgNames.includes(item.wg)) return false;
  }
  if (staleFilters.length > 0) {
    if (!staleFilters.includes(item.staleness_bucket)) return false;
  }
  if (tierFilters.length > 0) {
    const tiers = tierFilters.map(f => f.slice("contributor::".length));
    if (!tiers.includes(item.contributor_tier)) return false;
  }
  if (assigneeFilters.length > 0) {
    const assignees = item.assignees || [];
    const anyMatch = assigneeFilters.some(f => {
      if (f === ASSIGNEE_UNASSIGNED) return assignees.length === 0;
      return assignees.includes(f.slice("assignee::".length));
    });
    if (!anyMatch) return false;
  }
  for (const f of labelFilters) {
    if (f === "__unlabeled__") {
      if (item.labels.length !== 0) return false;
    } else {
      if (!item.labels.includes(f)) return false;
    }
  }
  return true;
}

function renderSummaryCard() {
  const card = document.getElementById("summary-card");
  const prs = DATA.filter(it => it.is_pr);
  const stale60 = prs.filter(it => it.days_stale >= 60).length;
  const stale30 = prs.filter(it => it.days_stale >= 30).length;
  const unassigned = prs.filter(it => (it.assignees || []).length === 0).length;
  const firstTimers = prs.filter(it => it.contributor_tier === "first-timer").length;

  const pills = [
    {label: `${stale60} PRs > 60d stale`,   filter: "stale::60d+",            urgent: stale60 > 0},
    {label: `${stale30} PRs > 30d stale`,   filter: "stale::30d",             urgent: stale30 > 5},
    {label: `${unassigned} PRs unassigned`, filter: ASSIGNEE_UNASSIGNED,      urgent: unassigned > 10},
    {label: `${firstTimers} first-timers`,  filter: "contributor::first-timer",urgent: firstTimers > 0},
  ];

  card.innerHTML = '<span class="card-label">PR Cleanup Drive</span>';
  for (const p of pills) {
    const el = document.createElement("span");
    el.className = "stat-pill" + (p.urgent ? " urgent" : "");
    el.textContent = p.label;
    el.title = "Click to filter";
    el.addEventListener("click", () => { activeFilters.clear(); toggleFilter(p.filter); });
    card.appendChild(el);
  }
}

function renderPRFocus() {
  const panel = document.getElementById("pr-focus");
  const divider = document.getElementById("focus-divider");
  const prs = DATA
    .filter(it => it.is_pr && !it.is_draft)
    .sort((a, b) => b.priority_score - a.priority_score)
    .slice(0, PR_FOCUS_TOP_N);

  if (prs.length === 0) { panel.innerHTML = ""; divider.style.display = "none"; return; }
  divider.style.display = "";

  panel.innerHTML = `<h2>Review Now <span style="font-weight:400;text-transform:none;letter-spacing:0;opacity:0.7;">— top ${prs.length} by urgency</span></h2>`;
  for (const it of prs) {
    const labels = it.labels || [];
    const isHot  = labels.some(l => l.startsWith("priority::") && l.includes("hot"));
    const isWarm = labels.some(l => l.startsWith("priority::") && l.includes("warm"));

    const badges = [];
    if (isHot)  badges.push('<span class="badge hot">HOT</span>');
    else if (isWarm) badges.push('<span class="badge warm">WARM</span>');
    if (it.contributor_tier === "first-timer") badges.push('<span class="badge first-timer">first-timer</span>');
    if (it.is_draft) badges.push('<span class="badge draft">draft</span>');
    if (it.days_stale >= 14) badges.push(`<span class="badge stale">${it.days_stale}d stale</span>`);
    if (it.wg) badges.push(`<span class="badge wg">${escape(it.wg)}</span>`);

    const div = document.createElement("div");
    div.className = "focus-item" + (isHot ? " is-hot" : "");
    div.innerHTML = `
      <div class="focus-item-title"><a href="${it.url}" target="_blank" rel="noopener">#${it.number} ${escape(it.title)}</a></div>
      <div class="focus-meta"><span>by ${escape(it.author)}</span><span>${it.comments} comments</span>${badges.join("")}</div>`;
    panel.appendChild(div);
  }
}

function renderRFCs() {
  const countEl = document.getElementById("rfc-count");
  const list = document.getElementById("rfc-list");
  countEl.textContent = RFC_DATA.length ? `— ${RFC_DATA.length} open` : "";
  if (RFC_DATA.length === 0) {
    list.innerHTML = '<div class="empty">No open RFCs found.</div>';
    return;
  }
  list.innerHTML = "";
  const STATUS_LABELS = {
    "open-for-comment": "Open for comment",
    "on-roadmap": "On roadmap",
    "new": "New",
  };
  for (const rfc of RFC_DATA) {
    const card = document.createElement("div");
    card.className = "rfc-card";
    const statusClass = (rfc.rfc_status || "new").replace(/[^a-z-]/g, "");
    const statusText = STATUS_LABELS[rfc.rfc_status] || rfc.rfc_status;
    card.innerHTML = `
      <div class="rfc-card-title"><a href="${rfc.url}" target="_blank" rel="noopener">#${rfc.number} ${escape(rfc.title)}</a></div>
      <div class="rfc-card-meta">
        <span class="rfc-status ${statusClass}">${escape(statusText)}</span>
        <span>by ${escape(rfc.author)}</span>
        <span>${rfc.comments} comment${rfc.comments === 1 ? "" : "s"}</span>
        <span>${rfc.days_stale}d ago</span>
      </div>`;
    list.appendChild(card);
  }
}

function renderFilterBar() {
  const bar = document.getElementById("filter-bar");
  bar.innerHTML = "";
  if (activeFilters.size === 0) {
    bar.innerHTML = `<span class="hint">Click a label, workgroup, assignee, or staleness bucket on the left to drill in.</span>`;
    return;
  }
  for (const f of activeFilters) {
    const pill = document.createElement("span");
    pill.className = "filter-pill";
    pill.innerHTML = `<span>${escape(prettyFilterName(f))}</span><button title="Remove">×</button>`;
    pill.querySelector("button").addEventListener("click", () => toggleFilter(f));
    bar.appendChild(pill);
  }
  const clear = document.createElement("button");
  clear.className = "clear-all";
  clear.textContent = "Clear all";
  clear.addEventListener("click", () => { activeFilters.clear(); render(); });
  bar.appendChild(clear);
}

function renderItems() {
  const wrap = document.getElementById("items");
  const meta = document.getElementById("items-meta");
  if (!hasAnyFilter()) {
    wrap.innerHTML = `<div class="empty">Pick one or more filters on the left, or click a PR Cleanup Drive pill above, to see matching items.</div>`;
    meta.textContent = "";
    return;
  }
  const matches = DATA.filter(matchesFilters);
  matches.sort((a, b) => b.comments - a.comments || b.number - a.number);
  const dateBits = [];
  if (dateFrom) dateBits.push("from " + dateFrom);
  if (dateTo) dateBits.push("to " + dateTo);
  const dateSummary = dateBits.length ? ` · created ${dateBits.join(" ")}` : "";
  meta.innerHTML = `<strong>${matches.length}</strong> item${matches.length === 1 ? "" : "s"} matching active filters${dateSummary} · sorted by comment count`;
  wrap.innerHTML = "";
  if (matches.length === 0) {
    wrap.innerHTML = `<div class="empty">No open items match this combination.</div>`;
    return;
  }
  for (const it of matches) {
    const div = document.createElement("div");
    div.className = "item";
    const age = ageOf(it.created_at);
    const assignees = it.assignees || [];
    const assigneeRow = assignees.length === 0
      ? `<div class="item-assignees">assigned to: <em>unassigned</em></div>`
      : `<div class="item-assignees">assigned to: ${assignees.map(a => `<strong data-assignee="${escape(a)}" style="cursor:pointer;">@${escape(a)}</strong>`).join(", ")}</div>`;
    const wgBadge = it.wg ? `<span class="item-wg" style="cursor:pointer;" data-wg="${escape(it.wg)}">WG: ${escape(it.wg)}</span>` : "";
    const tierBadge = it.is_pr && it.contributor_tier === "first-timer"
      ? `<span class="item-tier first-timer" style="cursor:pointer;" data-tier="contributor::first-timer">first-timer</span>`
      : "";
    div.innerHTML = `
      <div class="item-head">
        <span class="item-kind ${it.is_pr ? "pr" : "issue"}">${it.is_pr ? "PR" : "issue"}</span>
        <span class="item-num">#${it.number}</span>
        <span class="item-title"><a href="${it.url}" target="_blank" rel="noopener">${escape(it.title)}</a></span>
      </div>
      <div class="item-meta">
        <span>by ${escape(it.author)}</span>
        <span>${age}</span>
        <span>${it.comments} comment${it.comments === 1 ? "" : "s"}</span>
        <span style="color:${it.days_stale >= 60 ? 'var(--staleness)' : 'var(--muted)'};">${it.days_stale}d since update</span>
      </div>
      ${(wgBadge || tierBadge) ? `<div class="item-extra">${wgBadge}${tierBadge}</div>` : ""}
      ${assigneeRow}
      <div class="item-labels">
        ${it.labels.map(l => `<span data-label="${escape(l)}">${escape(l)}</span>`).join("")}
      </div>
    `;
    div.querySelectorAll(".item-labels span").forEach(span => {
      span.addEventListener("click", e => { e.stopPropagation(); toggleFilter(span.dataset.label); });
    });
    div.querySelectorAll(".item-assignees strong[data-assignee]").forEach(el => {
      el.addEventListener("click", e => { e.stopPropagation(); toggleFilter("assignee::" + el.dataset.assignee); });
    });
    const wgEl = div.querySelector("[data-wg]");
    if (wgEl) wgEl.addEventListener("click", e => { e.stopPropagation(); toggleFilter("wg::" + wgEl.dataset.wg); });
    const tierEl = div.querySelector("[data-tier]");
    if (tierEl) tierEl.addEventListener("click", e => { e.stopPropagation(); toggleFilter(tierEl.dataset.tier); });
    wrap.appendChild(div);
  }
}

function render() {
  renderFamilies();
  renderFilterBar();
  renderItems();
}

function ageOf(iso) {
  const then = new Date(iso);
  const now = new Date();
  const days = Math.floor((now - then) / 86400000);
  if (days < 1) return "today";
  if (days < 30) return `${days}d ago`;
  if (days < 365) return `${Math.floor(days/30)}mo ago`;
  return `${Math.floor(days/365)}y ago`;
}

function escape(s) {
  return String(s).replace(/[&<>"']/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
}

// Stats
document.getElementById("total").textContent = DATA.length;
document.getElementById("total-issues").textContent = DATA.filter(it => !it.is_pr).length;
document.getElementById("total-prs").textContent = DATA.filter(it => it.is_pr).length;

// One-time renders (not filter-dependent)
renderSummaryCard();
renderPRFocus();
renderRFCs();

// Date filter wiring
const dateFromEl = document.getElementById("date-from");
const dateToEl = document.getElementById("date-to");
dateFromEl.addEventListener("change", () => { dateFrom = dateFromEl.value || null; render(); });
dateToEl.addEventListener("change", () => { dateTo = dateToEl.value || null; render(); });
document.getElementById("date-clear").addEventListener("click", () => {
  dateFrom = null; dateTo = null;
  dateFromEl.value = ""; dateToEl.value = "";
  render();
});

render();
</script>
</body>
</html>
"""


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--output", default="triage_dashboard.html", help="Output HTML path")
    p.add_argument("--repo", default="lemonade-sdk/lemonade", help="OWNER/REPO")
    p.add_argument(
        "--include-discussions",
        action="store_true",
        help="Fetch open RFC discussions and include them in the dashboard.",
    )
    args = p.parse_args()

    print(f"Fetching open issues + PRs from {args.repo}…", file=sys.stderr)
    items = fetch_items(args.repo)
    print(f"  fetched {len(items)} items", file=sys.stderr)

    rfcs = []
    if args.include_discussions:
        print("Fetching RFC discussions…", file=sys.stderr)
        rfcs = fetch_rfcs(args.repo)
        print(f"  fetched {len(rfcs)} open RFCs", file=sys.stderr)

    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    html = (
        HTML_TEMPLATE.replace("__REPO__", args.repo)
        .replace("__GENERATED_AT__", now)
        .replace("__DATA__", script_safe_json(items))
        .replace("__RFC_DATA__", script_safe_json(rfcs))
    )

    out = Path(args.output)
    out.write_text(html, encoding="utf-8")
    print(f"Wrote {out.resolve()} ({len(html):,} chars)", file=sys.stderr)


if __name__ == "__main__":
    main()
