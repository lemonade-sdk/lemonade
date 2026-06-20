#!/usr/bin/env python3
"""Generate backend reference docs from the self-describing backend descriptors.

The C++ backend descriptors (src/cpp/include/lemon/backends/<stem>/<stem>.h) are
the single source of truth for what each backend is. This script boots a `lemond`
server, reads the descriptor-generated ``/system-info`` ``recipes`` object and
``server_models.json``, and rewrites the marker-delimited regions of the target
doc(s). A CI step runs it with ``--check`` and fails if the committed docs drift.

Usage:
    python docs/tools/gen_backend_docs.py [--lemond PATH] [--check]

``--check`` regenerates in memory and exits non-zero if the on-disk docs differ,
without modifying them.

Only the regions between::

    <!-- BEGIN GENERATED: <id> -->
    <!-- END GENERATED: <id> -->

are rewritten; surrounding prose is left untouched.
"""

import argparse
import json
import re
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SERVER_MODELS = REPO_ROOT / "src" / "cpp" / "resources" / "server_models.json"
TARGET_DOC = REPO_ROOT / "docs" / "dev" / "backends-reference.md"


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def find_lemond(explicit: str | None) -> Path:
    if explicit:
        p = Path(explicit)
        if not p.exists():
            sys.exit(f"lemond not found at {p}")
        return p
    for candidate in [
        REPO_ROOT / "build" / "lemond",
        REPO_ROOT / "build" / "lemond.exe",
    ]:
        if candidate.exists():
            return candidate
    sys.exit("Could not find a built lemond (looked in build/). Pass --lemond PATH.")


class Lemond:
    """Boots a throwaway lemond on a free port with an isolated cache dir."""

    def __init__(self, binary: Path):
        self.binary = binary
        self.port = free_port()
        self._cache = tempfile.TemporaryDirectory(prefix="lemond-docs-")
        self._proc: subprocess.Popen | None = None

    def __enter__(self):
        self._proc = subprocess.Popen(
            [str(self.binary), self._cache.name, "--port", str(self.port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.time() + 60
        while time.time() < deadline:
            try:
                self._get("/api/v1/health")
                return self
            except Exception:
                if self._proc.poll() is not None:
                    sys.exit("lemond exited before becoming ready")
                time.sleep(0.5)
        self.__exit__(None, None, None)
        sys.exit("lemond did not become ready within 60s")

    def __exit__(self, *exc):
        if self._proc and self._proc.poll() is None:
            try:
                self._get("/internal/shutdown", timeout=2)
            except Exception:
                pass
            try:
                self._proc.wait(timeout=10)
            except Exception:
                self._proc.kill()
        self._cache.cleanup()

    def _get(self, path: str, timeout: float = 5):
        url = f"http://127.0.0.1:{self.port}{path}"
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.read()

    def system_info(self) -> dict:
        return json.loads(self._get("/api/v1/system-info", timeout=30))


def md_escape(text: str) -> str:
    return str(text).replace("|", "\\|")


MODALITY_ORDER = [
    "Text generation",
    "Speech-to-text",
    "Text-to-speech",
    "Image generation",
]
OS_LABEL = {"windows": "Windows", "linux": "Linux", "macos": "macOS"}
OS_ORDER = ["windows", "linux", "macos"]


def _fmt_os(os_set) -> str:
    return ", ".join(OS_LABEL.get(o, o) for o in OS_ORDER if o in os_set)


def _code_devices(summary: str) -> str:
    # Light formatting: render bare arch tokens as <code>, matching the README style.
    summary = re.sub(r"\bx86_64\b", "<code>x86_64</code>", summary)
    summary = re.sub(r"\barm64\b", "<code>arm64</code>", summary)
    return summary


def _ordered(recipes: dict) -> list:
    # Recipes in descriptor registry order (stable, deterministic doc rendering).
    return sorted(recipes.items(), key=lambda kv: kv[1].get("order", 999))


def render_readme_matrix(recipes: dict) -> str:
    # Group descriptor-backed recipes by modality, in descriptor registry order.
    by_mod: dict[str, list] = {m: [] for m in MODALITY_ORDER}
    for recipe, info in _ordered(recipes):
        mod = info.get("modality")
        if not mod or mod not in by_mod:
            continue
        # Merge support rows sharing a (backend, device summary); union their OS.
        merged: list[dict] = []
        seen: dict[tuple, dict] = {}
        for row in info.get("support", []):
            key = (row["backend"], row.get("device_summary", ""))
            if key in seen:
                seen[key]["os"] |= set(row.get("os", []))
            else:
                d = {
                    "backend": row["backend"],
                    "summary": row.get("device_summary", ""),
                    "os": set(row.get("os", [])),
                }
                seen[key] = d
                merged.append(d)
        if merged:
            by_mod[mod].append((recipe, info, merged))

    out = [
        "<table>",
        "  <thead>",
        "    <tr>",
        "      <th>Modality</th>",
        "      <th>Engine</th>",
        "      <th>Backend</th>",
        "      <th>Device</th>",
        "      <th>OS</th>",
        "    </tr>",
        "  </thead>",
        "  <tbody>",
    ]
    for mod in MODALITY_ORDER:
        recipes_in = by_mod[mod]
        if not recipes_in:
            continue
        mod_span = sum(len(m) for _, _, m in recipes_in)
        first_mod = True
        for recipe, info, merged in recipes_in:
            engine = f"<code>{recipe}</code>" + (
                " (experimental)" if info.get("experimental") else ""
            )
            first_recipe = True
            for d in merged:
                out.append("    <tr>")
                if first_mod:
                    out.append(
                        f'      <td rowspan="{mod_span}"><strong>{mod}</strong></td>'
                    )
                    first_mod = False
                if first_recipe:
                    out.append(f'      <td rowspan="{len(merged)}">{engine}</td>')
                    first_recipe = False
                out.append(f'      <td><code>{d["backend"]}</code></td>')
                out.append(f"      <td>{_code_devices(d['summary'])}</td>")
                out.append(f"      <td>{_fmt_os(d['os'])}</td>")
                out.append("    </tr>")
    out += ["  </tbody>", "</table>"]
    return "\n".join(out)


def _oxford(items: list) -> str:
    items = [f"`{i}`" for i in items]
    if len(items) <= 1:
        return "".join(items)
    if len(items) == 2:
        return f"{items[0]} and {items[1]}"
    return ", ".join(items[:-1]) + f", and {items[-1]}"


def render_npu_exclusivity(recipes: dict) -> str:
    npu = [
        r
        for r, info in _ordered(recipes)
        if any(
            row.get("backend") == "npu"
            or any(d.get("device") == "amd_npu" for d in row.get("devices", []))
            for row in info.get("support", [])
        )
    ]
    return f"- **NPU Exclusivity:** {_oxford(npu)} are mutually exclusive on the NPU."


def render_overview(recipes: dict) -> str:
    rows = [
        "| Recipe | Name | Selectable backend | Uses ctx_size | Backends |",
        "|--------|------|--------------------|---------------|----------|",
    ]
    for recipe in sorted(recipes):
        info = recipes[recipe]
        if "display_name" not in info:
            continue  # not a descriptor-backed recipe on this run
        backends = sorted({b["backend"] for b in info.get("support", [])}) or sorted(
            info.get("backends", {})
        )
        rows.append(
            "| `{r}` | {n} | {s} | {c} | {b} |".format(
                r=recipe,
                n=md_escape(info.get("display_name", "")),
                s="yes" if info.get("selectable_backend") else "no",
                c="yes" if info.get("uses_ctx_size") else "no",
                b=", ".join(backends) if backends else "—",
            )
        )
    return "\n".join(rows)


def render_support_matrix(recipes: dict) -> str:
    rows = [
        "| Recipe | Backend | OS | Device families |",
        "|--------|---------|----|-----------------|",
    ]
    for recipe in sorted(recipes):
        info = recipes[recipe]
        for row in info.get("support", []):
            fams = []
            for d in row.get("devices", []):
                f = d.get("families") or []
                fams.append(d["device"] + (f" ({', '.join(f)})" if f else ""))
            rows.append(
                "| `{r}` | {b} | {o} | {d} |".format(
                    r=recipe,
                    b=row.get("backend", ""),
                    o=", ".join(sorted(row.get("os", []))),
                    d=md_escape("; ".join(fams)) if fams else "—",
                )
            )
    return "\n".join(rows)


def render_options(recipes: dict) -> str:
    blocks = []
    for recipe in sorted(recipes):
        info = recipes[recipe]
        opts = info.get("options")
        if not opts:
            continue
        blocks.append(f"#### `{recipe}` — {info.get('display_name', recipe)}\n")
        blocks.append("| Option | CLI flag | Type | Default | Description |")
        blocks.append("|--------|----------|------|---------|-------------|")
        if info.get("uses_ctx_size"):
            blocks.append(
                "| `ctx_size` | `--ctx-size` | SIZE | -1 | Context size for the model |"
            )
        for o in opts:
            blocks.append(
                "| `{n}` | {f} | {t} | {d} | {h} |".format(
                    n=o["name"],
                    f=f"`{o['cli_flag']}`" if o.get("cli_flag") else "—",
                    t=o.get("type_name", ""),
                    d=md_escape(
                        json.dumps(o.get("default"))
                        if not isinstance(o.get("default"), str)
                        else o.get("default") or '""'
                    ),
                    h=md_escape(o.get("help", "")),
                )
            )
        blocks.append("")
    return "\n".join(blocks).rstrip()


def render_models(recipes: dict) -> str:
    models = json.loads(SERVER_MODELS.read_text())
    by_recipe: dict[str, list] = {}
    for name, data in models.items():
        if not isinstance(data, dict):
            continue
        by_recipe.setdefault(data.get("recipe", "(unspecified)"), []).append(
            (name, data)
        )
    blocks = []
    for recipe in sorted(by_recipe):
        entries = sorted(by_recipe[recipe])
        display = recipes.get(recipe, {}).get("display_name", recipe)
        blocks.append(f"#### `{recipe}` — {display} ({len(entries)} models)\n")
        blocks.append("| Model | Size (GB) | Labels |")
        blocks.append("|-------|-----------|--------|")
        for name, data in entries:
            blocks.append(
                "| `{n}` | {s} | {l} |".format(
                    n=md_escape(name),
                    s=data.get("size", ""),
                    l=md_escape(", ".join(data.get("labels", []))) or "—",
                )
            )
        blocks.append("")
    return "\n".join(blocks).rstrip()


DEFAULT_TEMPLATE = """# Backend reference

<!-- This file is generated by docs/tools/gen_backend_docs.py from the C++ backend
descriptors. Do not edit the regions between the GENERATED markers by hand; run
the generator instead. Prose outside the markers is preserved. -->

## Backends

<!-- BEGIN GENERATED: backends-overview -->
<!-- END GENERATED: backends-overview -->

## Support matrix

<!-- BEGIN GENERATED: backends-matrix -->
<!-- END GENERATED: backends-matrix -->

## Recipe options

<!-- BEGIN GENERATED: backend-options -->
<!-- END GENERATED: backend-options -->

## Models

<!-- BEGIN GENERATED: backend-models -->
<!-- END GENERATED: backend-models -->
"""


def apply_sections(text: str, sections: dict[str, str]) -> str:
    for marker_id, body in sections.items():
        pattern = re.compile(
            r"(<!-- BEGIN GENERATED: "
            + re.escape(marker_id)
            + r" -->).*?(<!-- END GENERATED: "
            + re.escape(marker_id)
            + r" -->)",
            re.DOTALL,
        )
        if not pattern.search(text):
            sys.exit(f"Marker region '{marker_id}' not found in target doc")
        # Escape backslashes and group-ref markers in the body for re.sub.
        safe_body = body.replace("\\", "\\\\")
        replacement = r"\1" + "\n" + safe_body + "\n" + r"\2"
        text = pattern.sub(replacement, text)
    return text


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--lemond", help="Path to the built lemond binary")
    ap.add_argument(
        "--check", action="store_true", help="Fail if docs are stale; do not write"
    )
    args = ap.parse_args()

    binary = find_lemond(args.lemond)
    with Lemond(binary) as server:
        info = server.system_info()
    recipes = info.get("recipes", {})
    if not recipes:
        sys.exit("/system-info returned no recipes")

    # Each target doc maps marker IDs -> generated content. backends-reference.md
    # is created from a template if missing; the others must already contain their
    # markers (the regions were added to the curated docs by hand once).
    targets: dict = {
        TARGET_DOC: {
            "sections": {
                "backends-overview": render_overview(recipes),
                "backends-matrix": render_support_matrix(recipes),
                "backend-options": render_options(recipes),
                "backend-models": render_models(recipes),
            },
            "template": DEFAULT_TEMPLATE,
        },
        REPO_ROOT
        / "README.md": {
            "sections": {"backends-matrix": render_readme_matrix(recipes)},
        },
        REPO_ROOT
        / "docs"
        / "guide"
        / "configuration"
        / "multi-model.md": {
            "sections": {"npu-exclusivity": render_npu_exclusivity(recipes)},
        },
    }

    stale = []
    for path, spec in targets.items():
        rel = path.relative_to(REPO_ROOT)
        current = path.read_text() if path.exists() else spec.get("template", "")
        if not current:
            sys.exit(f"{rel} is missing and has no template")
        updated = apply_sections(current, spec["sections"])
        if args.check:
            if not path.exists() or path.read_text() != updated:
                stale.append(str(rel))
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(updated)
            print(f"Wrote {rel}")

    if args.check:
        if stale:
            sys.exit(
                "Stale generated docs: "
                + ", ".join(stale)
                + "\nRun: python docs/tools/gen_backend_docs.py"
            )
        print("All generated docs are up to date.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
