#!/usr/bin/env python3
"""Generate backend reference docs from the self-describing backend descriptors.

The C++ backend descriptors (src/cpp/server/backends/*_descriptor.cpp) are the
single source of truth for what each backend is. This script boots a `lemond`
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

    sections = {
        "backends-overview": render_overview(recipes),
        "backends-matrix": render_support_matrix(recipes),
        "backend-options": render_options(recipes),
        "backend-models": render_models(recipes),
    }

    current = TARGET_DOC.read_text() if TARGET_DOC.exists() else DEFAULT_TEMPLATE
    updated = apply_sections(current, sections)

    if args.check:
        if not TARGET_DOC.exists() or TARGET_DOC.read_text() != updated:
            sys.exit(
                f"{TARGET_DOC.relative_to(REPO_ROOT)} is stale. Run: python docs/tools/gen_backend_docs.py"
            )
        print(f"{TARGET_DOC.relative_to(REPO_ROOT)} is up to date.")
        return 0

    TARGET_DOC.parent.mkdir(parents=True, exist_ok=True)
    TARGET_DOC.write_text(updated)
    print(f"Wrote {TARGET_DOC.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
