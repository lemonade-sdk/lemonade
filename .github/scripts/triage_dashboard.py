#!/usr/bin/env python3
"""Generate a self-contained triage dashboard for lemonade-sdk/lemonade.

Fetches all open issues + PRs via `gh api`, then writes a single HTML
file with histograms per label family (engine::, area::, runtime::,
priority::, type, component) and click-to-drill-down item lists with
multi-label AND filtering.

Usage:
    python .github/scripts/triage_dashboard.py [--output PATH] [--repo OWNER/REPO]

Requirements:
    - gh CLI authenticated
    - Python 3.9+ (stdlib only)

Open the resulting file directly in a browser. No build step, no
external assets, no auth required at view time — data is embedded.
Re-run the script any time to refresh.
"""

import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

PREFIX_FAMILIES = ("engine::", "area::", "runtime::", "priority::")
TYPE_LABELS = {"bug", "enhancement", "documentation", "question"}
COMPONENT_LABELS = {"cpp", "app", "web ui", "audio"}


def gh_api_pages(path):
    cmd = ["gh", "api", "--paginate", "--slurp", path]
    out = subprocess.run(
        cmd, check=True, capture_output=True, text=True, encoding="utf-8"
    ).stdout
    pages = json.loads(out)
    return [item for page in pages for item in page]


def fetch_items(repo):
    items = gh_api_pages(f"/repos/{repo}/issues?state=open&per_page=100")
    return [
        {
            "number": it["number"],
            "title": it["title"],
            "url": it["html_url"],
            "author": (it.get("user") or {}).get("login", "?"),
            "labels": [lbl["name"] for lbl in it.get("labels", [])],
            "assignees": [
                a["login"] for a in it.get("assignees", []) if a and a.get("login")
            ],
            "comments": it.get("comments", 0),
            "is_pr": it.get("pull_request") is not None,
            "created_at": it["created_at"],
        }
        for it in items
    ]


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
  }
  * { box-sizing: border-box; }
  body { font: 14px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI", system-ui, sans-serif; margin: 0; background: var(--bg); color: var(--fg); }
  a { color: var(--link); text-decoration: none; }
  a:hover { text-decoration: underline; }
  header { padding: 16px 24px; border-bottom: 1px solid var(--border); display: flex; justify-content: space-between; align-items: baseline; gap: 24px; }
  header h1 { margin: 0; font-size: 18px; font-weight: 600; }
  .stats { color: var(--muted); font-size: 13px; }
  .stats strong { color: var(--fg); }
  main { display: grid; grid-template-columns: minmax(360px, 480px) 1fr; gap: 0; min-height: calc(100vh - 60px); }
  #families { padding: 16px 24px; border-right: 1px solid var(--border); overflow-y: auto; max-height: calc(100vh - 60px); }
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
  #drilldown { padding: 16px 24px; overflow-y: auto; max-height: calc(100vh - 60px); }
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
  .item-labels { display: flex; gap: 4px; flex-wrap: wrap; margin-top: 6px; }
  .item-labels span { font-family: ui-monospace, monospace; font-size: 11px; padding: 1px 6px; border-radius: 10px; background: var(--bg); border: 1px solid var(--border); color: var(--muted); cursor: pointer; }
  .item-labels span:hover { color: var(--fg); border-color: var(--link); }
  .empty { color: var(--muted); text-align: center; padding: 48px 16px; }
  .footer { color: var(--muted); font-size: 12px; padding: 8px 24px; border-top: 1px solid var(--border); text-align: center; }
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
<main>
  <aside id="families"></aside>
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
<div class="footer">Re-run <code>python .github/scripts/triage_dashboard.py</code> to refresh.</div>
<script>
const DATA = __DATA__;
const FAMILY_ORDER = ["engine::", "area::", "runtime::", "priority::", "type", "component", "other", "__unlabeled__", "assignee::"];
const FAMILY_META = {
  "assignee::":    {name: "Assignee",   color: "var(--link)",       description: "Assigned vs. unassigned · click a user to filter"},
  "engine::":      {name: "Engine",     color: "var(--engine)",     description: "Backend / inference engine"},
  "area::":        {name: "Area",       color: "var(--area)",       description: "Component / surface area"},
  "runtime::":     {name: "Runtime",    color: "var(--runtime)",    description: "GPU/compute runtime"},
  "priority::":    {name: "Priority",   color: "var(--priority)",   description: "Engagement-driven heat"},
  "type":          {name: "Type",       color: "var(--type)",       description: "Bug / feature / docs / question"},
  "component":     {name: "Component",  color: "var(--component)",  description: "Pre-existing component labels"},
  "other":         {name: "Other",      color: "var(--other)",      description: "Anything else applied to items"},
  "__unlabeled__": {name: "Unlabeled",  color: "var(--unlabeled)",  description: "Items with no labels at all"},
};
const ASSIGNEE_UNASSIGNED = "assignee::__none__";

const TYPE_LABELS = new Set(["bug", "enhancement", "documentation", "question"]);
const COMPONENT_LABELS = new Set(["cpp", "app", "web ui", "audio"]);

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
    const assignees = it.assignees || [];
    if (assignees.length === 0) {
      fams["assignee::"][ASSIGNEE_UNASSIGNED] = (fams["assignee::"][ASSIGNEE_UNASSIGNED] || 0) + 1;
    } else {
      for (const login of assignees) {
        const key = "assignee::" + login;
        fams["assignee::"][key] = (fams["assignee::"][key] || 0) + 1;
      }
    }
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
let dateFrom = null;  // "YYYY-MM-DD" or null
let dateTo = null;    // "YYYY-MM-DD" or null
const collapsedFamilies = new Set(["assignee::"]);  // assignee family is hidden by default
const families = buildFamilies(DATA);

function prettyFilterName(f) {
  if (f === "__unlabeled__") return "(no labels)";
  if (f === ASSIGNEE_UNASSIGNED) return "unassigned";
  if (f.startsWith("assignee::")) return "@" + f.slice("assignee::".length);
  return f;
}

function dateInRange(iso) {
  if (!dateFrom && !dateTo) return true;
  const day = iso.slice(0, 10);  // "YYYY-MM-DD"
  if (dateFrom && day < dateFrom) return false;
  if (dateTo && day > dateTo) return false;
  return true;
}

function hasAnyFilter() {
  return activeFilters.size > 0 || dateFrom || dateTo;
}

function renderFamilies() {
  const wrap = document.getElementById("families");
  wrap.innerHTML = "";
  for (const fname of FAMILY_ORDER) {
    const labels = families[fname];
    if (Object.keys(labels).length === 0) continue;
    const meta = FAMILY_META[fname];
    const entries = Object.entries(labels).sort((a, b) => b[1] - a[1]);
    const total = entries.reduce((s, e) => s + e[1], 0);
    const max = Math.max(...Object.values(labels));
    const collapsible = fname === "assignee::";
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
  if (!hasAnyFilter()) return false;  // nothing active = show nothing
  if (!dateInRange(item.created_at)) return false;
  // Assignee filters are OR'd among themselves; label filters AND.
  const assigneeFilters = [];
  const labelFilters = [];
  for (const f of activeFilters) {
    if (f === ASSIGNEE_UNASSIGNED || f.startsWith("assignee::")) assigneeFilters.push(f);
    else labelFilters.push(f);
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

function renderFilterBar() {
  const bar = document.getElementById("filter-bar");
  bar.innerHTML = "";
  if (activeFilters.size === 0) {
    bar.innerHTML = `<span class="hint">Click a label or assignee on the left to drill in. Labels AND together; assignees within the Assignee family OR together.</span>`;
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
    wrap.innerHTML = `<div class="empty">Pick one or more labels, an assignee, or a date range to see matching items.</div>`;
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
      </div>
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
    args = p.parse_args()

    print(f"Fetching open issues + PRs from {args.repo}…", file=sys.stderr)
    items = fetch_items(args.repo)
    print(f"  fetched {len(items)} items", file=sys.stderr)

    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    html = (
        HTML_TEMPLATE.replace("__REPO__", args.repo)
        .replace("__GENERATED_AT__", now)
        .replace("__DATA__", script_safe_json(items))
    )

    out = Path(args.output)
    out.write_text(html, encoding="utf-8")
    print(f"Wrote {out.resolve()} ({len(html):,} chars)", file=sys.stderr)


if __name__ == "__main__":
    main()
