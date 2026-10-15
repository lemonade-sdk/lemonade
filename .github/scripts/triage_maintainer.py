#!/usr/bin/env python3
"""Generate a maintainer-focused triage page for lemonade-sdk/lemonade.

Single filterable view of all open PRs with controls for:
  - Workgroup (dropdown)
  - Contributor type (Maintainer / First-timer toggles)
  - Staleness bucket
All filters AND together; total always visible.

Usage:
    python .github/scripts/triage_maintainer.py [--output PATH] [--repo OWNER/REPO]

Requirements:
    - gh CLI authenticated
    - Python 3.9+ (stdlib only)
"""

import argparse
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

WRITE_ASSOCIATIONS = {"OWNER", "MEMBER", "COLLABORATOR"}

ACTIVE_WGS = [
    "Auto-Tune",
    "Cross-Vendor Support",
    "Cloud Hybrid",
    "Remote Use",
    "GUI App",
    "Enterprise Grade",
]

WG_LEADS = {
    "Auto-Tune": "@bitgamma",
    "Cross-Vendor Support": "@kenvandine",
    "Cloud Hybrid": "@ramkrishna2910",
    "Remote Use": "@Geramy",
    "GUI App": "@kpoineal",
    "Enterprise Grade": "@jeremyfowers",
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
    if author_association == "FIRST_TIME_CONTRIBUTOR":
        return "first-timer"
    return "returning"


def parse_wg(body):
    if not body:
        return None
    m = re.search(r"scope of WG:\s*([^\n\r<]{1,60})", body, re.IGNORECASE)
    if m:
        candidate = m.group(1).strip()
        # Only accept known canonical WGs — rejects RFC/checkbox garbage
        return CANONICAL_WGS.get(candidate.lower())
    body_lower = body.lower()
    for key in sorted(CANONICAL_WGS, key=len, reverse=True):
        if key in body_lower:
            return CANONICAL_WGS[key]
    return None


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


def fetch_prs(repo, top_contributors):
    now = datetime.now(timezone.utc)
    raw = gh_api_pages(f"/repos/{repo}/issues?state=open&per_page=100")
    prs = []
    for it in raw:
        if it.get("pull_request") is None:
            continue
        if it.get("draft"):
            continue
        updated = datetime.fromisoformat(it["updated_at"].replace("Z", "+00:00"))
        created = datetime.fromisoformat(it["created_at"].replace("Z", "+00:00"))
        login = (it.get("user") or {}).get("login", "?")
        assoc = it.get("author_association", "NONE")
        is_maint = assoc in WRITE_ASSOCIATIONS or login in ADMIN_MAINTAINERS
        body = it.get("body") or ""
        days_stale = (now - updated).days
        prs.append(
            {
                "number": it["number"],
                "title": it["title"],
                "url": it["html_url"],
                "author": login,
                "labels": [lbl["name"] for lbl in it.get("labels", [])],
                "assignees": [
                    a["login"] for a in it.get("assignees", []) if a and a.get("login")
                ],
                "comments": it.get("comments", 0),
                "created_at": it["created_at"],
                "updated_at": it["updated_at"],
                "days_stale": days_stale,
                "days_old": (now - created).days,
                "wg": parse_wg(body),
                "rfc_status": parse_rfc_status(
                    [lbl["name"] for lbl in it.get("labels", [])]
                ),
                "is_maintainer": is_maint,
                "community_tier": (
                    None if is_maint else community_tier(login, assoc, top_contributors)
                ),
                "is_draft": bool(it.get("draft", False)),
            }
        )
    return prs


def fetch_activity(repo, days=30):
    """Fetch closed PRs from the last N days to build daily activity data.

    Returns a list of dicts with date, opened, merged, closed_not_merged counts
    for each day in the window, plus the running open count reconstructed from
    today backwards.
    """
    now = datetime.now(timezone.utc)
    cutoff = now.replace(hour=0, minute=0, second=0, microsecond=0)
    from datetime import timedelta

    window_start = cutoff - timedelta(days=days - 1)

    # Paginate closed PRs until we go past the window
    closed_prs = []
    page = 1
    while True:
        cmd = [
            "gh",
            "api",
            f"/repos/{repo}/pulls?state=closed&sort=updated&direction=desc&per_page=100&page={page}",
        ]
        try:
            out = subprocess.run(
                cmd, check=True, capture_output=True, text=True, encoding="utf-8"
            ).stdout
            batch = json.loads(out)
        except Exception:
            break
        if not batch:
            break
        for pr in batch:
            closed = pr.get("closed_at") or ""
            if closed:
                closed_dt = datetime.fromisoformat(closed.replace("Z", "+00:00"))
                if closed_dt < window_start:
                    # Past the window — stop paginating
                    closed_prs_sorted = True
                    break
                closed_prs.append(pr)
        else:
            page += 1
            continue
        break  # inner break triggered

    # Also get open PRs created in the window (already fetched but re-fetch
    # created_at dates here to count opens per day)
    open_prs = []
    try:
        cmd = [
            "gh",
            "api",
            "--paginate",
            "--slurp",
            f"/repos/{repo}/pulls?state=open&sort=created&direction=desc&per_page=100",
        ]
        out = subprocess.run(
            cmd, check=True, capture_output=True, text=True, encoding="utf-8"
        ).stdout
        pages = json.loads(out)
        open_prs = [pr for page in pages for pr in page]
    except Exception:
        pass

    # Build per-day buckets
    from datetime import timedelta

    days_list = [window_start + timedelta(days=i) for i in range(days)]
    day_keys = [d.strftime("%Y-%m-%d") for d in days_list]
    opened = {k: 0 for k in day_keys}
    merged = {k: 0 for k in day_keys}
    closed_nm = {k: 0 for k in day_keys}

    for pr in open_prs:
        created = pr.get("created_at", "")
        if created:
            d = created[:10]
            if d in opened:
                opened[d] += 1

    for pr in closed_prs:
        created = pr.get("created_at", "")
        if created:
            d = created[:10]
            if d in opened:
                opened[d] += 1
        closed = pr.get("closed_at", "")
        if closed:
            d = closed[:10]
            if d in merged:
                if pr.get("merged_at"):
                    merged[d] += 1
                else:
                    closed_nm[d] += 1

    # Reconstruct running open count: start from today's known open count,
    # go backwards subtracting opens and adding closes each day.
    # We don't have exact daily snapshots, so we approximate from events.
    # running_open[i] = open count at END of day i
    total_open_now = len([pr for pr in open_prs if not pr.get("draft")])
    running = {}
    current = total_open_now
    for d in reversed(day_keys):
        running[d] = current
        # Going backwards: undo today's events
        current = current - opened.get(d, 0) + merged.get(d, 0) + closed_nm.get(d, 0)
        current = max(current, 0)

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
  #filter-bar { padding:14px 24px; border-bottom:1px solid var(--border);
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
  #clear-btn { padding:5px 12px; border:1px solid var(--border); border-radius:6px;
               background:none; color:var(--muted); cursor:pointer;
               font:inherit; font-size:13px; align-self:flex-end; }
  #clear-btn:hover { color:var(--fg); border-color:var(--fg); }

  /* result bar */
  .flex-row { display:flex; align-items:center; flex-wrap:wrap; }
  /* activity chart */
  #activity-chart { padding:16px 24px 20px; border-bottom:1px solid var(--border); background:var(--panel); }
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
  #cumulative-chart { width:100%; overflow:hidden; }

  #result-bar { padding:10px 24px; gap:16px; border-bottom:1px solid var(--border); font-size:13px; }
  #result-count { font-weight:600; }
  .result-breakdown { color:var(--muted); font-size:12px; }
  .chip { display:inline-block; padding:1px 8px; border-radius:10px;
          font-size:11px; font-weight:600; margin:0 2px; }
  .chip.wg     { background:#1f3a5f; border:1px solid #79c0ff; color:#79c0ff; }
  .chip.maint  { background:#21262d; border:1px solid var(--border); color:var(--muted); }
  .chip.first  { background:#1f6feb; color:#fff; }
  .chip.lead   { background:#3d3000; border:1px solid var(--tier-lead); color:var(--tier-lead); }
  .chip.active { background:#1f3a5f; border:1px solid var(--tier-active); color:var(--tier-active); }
  .chip.stale  { background:var(--stale); color:#000; }
  .chip.hot    { background:var(--hot); color:#fff; }
  .chip.warm   { background:#e3a000; color:#000; }
  .chip.draft  { background:var(--draft); color:var(--fg); }

  /* pr list */
  #pr-list { padding:16px 24px; max-width:1100px; margin:0 auto; }
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
</style>
</head>
<body>

<header>
  <h1>PR Triage — <span style="color:var(--muted)">__REPO__</span></h1>
  <div class="header-right">
    <div>
      <div class="total-badge" id="hdr-total">—</div>
      <div class="total-label">open PRs</div>
    </div>
    <div style="font-size:12px;color:var(--muted);line-height:1.8;">
      <div id="hdr-wg-count">— with workgroup</div>
      <div id="hdr-maint-count">— maintainer PRs</div>
      <div id="hdr-first-count">— first-timers</div>
      <div id="hdr-stale-count">— stale 30d+</div>
      <div id="hdr-rfc-count" style="color:var(--hot)">— RFC required</div>
    </div>
    <a class="header-link" href="index.html">&#8592; Label dashboard</a>
  </div>
  <span class="gen-time">generated __GENERATED_AT__</span>
</header>

<div id="activity-chart">
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
<div id="filter-bar">
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
  <button id="clear-btn">Clear all</button>
</div>

<div id="result-bar" class="flex-row">
  <span id="result-count"></span>
  <span class="result-breakdown" id="result-breakdown"></span>
</div>

<div id="pr-list"></div>

<footer>
  Re-run <code>python .github/scripts/triage_maintainer.py</code> to refresh.
  &nbsp;|&nbsp; <a href="index.html">Label dashboard</a>
</footer>

<script>
const DATA = __DATA__;
const ALL_WGS = __ALL_WGS__;

// ── filter state ──
const state = {
  wg: "",           // "" = all, "__unclaimed__" = no wg, or a wg name
  maintainer: false,
  firstTimer: false,
  wgLead: false,
  staleDays: 0,     // 0 = any, 14, 30, or 60
  rfcRequired: false,
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
  if(pr.wg) chips.push(`<span class="chip wg">${esc(pr.wg)}</span>`);
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
        <span class="${ageC}">updated ${ageText(pr.days_stale)} ago</span>
        <span>${pr.comments} comment${pr.comments===1?"":"s"}</span>
        <span>→ ${assignees}</span>
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
  document.getElementById("result-breakdown").textContent =
    parts.length ? "Filters: " + parts.join(" · ") : "All open PRs";
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
  state.wgLead=false; state.staleDays=0; state.rfcRequired=false;
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

renderActivityChart();
renderWGBreakdown();
setTimeout(() => renderCumulativeChart(), 50);
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
    args = p.parse_args()

    print("Fetching top contributors…", file=sys.stderr)
    top_contributors = fetch_top_contributors(args.repo)

    print(f"Fetching open PRs from {args.repo}…", file=sys.stderr)
    prs = fetch_prs(args.repo, top_contributors)
    community = sum(1 for pr in prs if not pr["is_maintainer"])
    print(f"  fetched {len(prs)} open PRs ({community} community)", file=sys.stderr)

    print("Fetching 30-day activity…", file=sys.stderr)
    activity = fetch_activity(args.repo, days=30)  # JS window slider slices client-side
    print(f"  built {len(activity)} days of activity data", file=sys.stderr)

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
    )

    out = Path(args.output)
    out.write_text(html, encoding="utf-8")
    print(f"Wrote {out.resolve()} ({len(html):,} chars)", file=sys.stderr)


if __name__ == "__main__":
    main()
