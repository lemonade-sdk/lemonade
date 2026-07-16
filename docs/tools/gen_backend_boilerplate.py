#!/usr/bin/env python3
"""Generate backend boilerplate (docs + config defaults) from the descriptors.

The C++ backend descriptors (src/cpp/include/lemon/backends/<stem>/<stem>.h) are
the single source of truth for what each backend is. This script boots a `lemond`
server and regenerates the committed artifacts that would otherwise be
hand-maintained:

  * Marker-delimited regions of the backend reference docs, from
    ``/system-info`` ``recipes`` + ``server_models.json``.
  * The whole of ``src/cpp/resources/defaults.json``, mirrored verbatim from
    ``/internal/config/defaults`` (its per-recipe blocks come from each
    descriptor's ``config_defaults()``).

A CI step runs it with ``--check`` and fails if any committed artifact drifts.

Usage:
    python docs/tools/gen_backend_boilerplate.py [--lemond PATH] [--check]

``--check`` regenerates in memory and exits non-zero if any on-disk artifact
differs, without modifying it. For the docs, only the regions between::

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

    def config(self) -> dict:
        return json.loads(self._get("/internal/config", timeout=10))

    def config_defaults_text(self) -> str:
        # Verbatim text of the canonical default config (the server's own
        # serialization) so the committed resources/defaults.json is byte-stable.
        text = self._get("/internal/config/defaults", timeout=10).decode("utf-8")
        return text if text.endswith("\n") else text + "\n"


def md_escape(text: str) -> str:
    return str(text).replace("|", "\\|")


# Preferred README ordering, not an allow-list. Modalities not listed here are
# appended deterministically so future backends cannot be silently omitted.
MODALITY_ORDER = [
    "Text generation",
    "Speech-to-text",
    "Text-to-speech",
    "Audio generation",
    "Image generation",
    "3D generation",
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


def _ordered_modalities(by_mod: dict[str, list]) -> list[str]:
    """Return known modalities first, then future modalities deterministically."""
    preferred = [mod for mod in MODALITY_ORDER if mod in by_mod]
    additional = sorted(set(by_mod) - set(MODALITY_ORDER))
    return preferred + additional


def render_readme_matrix(recipes: dict) -> str:
    # Group descriptor-backed recipes by modality, in descriptor registry order.
    # MODALITY_ORDER controls presentation only; it must never filter recipes.
    by_mod: dict[str, list] = {}
    for recipe, info in _ordered(recipes):
        support_rows = info.get("support", [])
        if not support_rows:
            continue
        mod = info.get("modality")
        if not mod:
            sys.exit(
                f"Backend '{recipe}' has support rows but no documentation modality"
            )

        # Merge support rows sharing a (backend, device summary); union their OS.
        merged: list[dict] = []
        seen: dict[tuple, dict] = {}
        for row in support_rows:
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
            by_mod.setdefault(mod, []).append((recipe, info, merged))

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
    for mod in _ordered_modalities(by_mod):
        recipes_in = by_mod[mod]
        mod_span = sum(len(merged) for _, _, merged in recipes_in)
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


def _cli_default(opt: dict) -> str:
    d = opt.get("default")
    if opt.get("type_name") == "BACKEND" and d == "":
        return "Auto-detected"
    if isinstance(d, str):
        return '`""`' if d == "" else f"`{d}`"
    if isinstance(d, bool):
        return f"`{str(d).lower()}`"
    if d == -1:
        return "auto"
    return f"`{d}`"


def render_cli_recipe_options(recipes: dict) -> str:
    # Per-recipe load options, exactly as the CLI registers them from descriptors.
    # Recipes with no CLI options (kokoro, cloud) are omitted.
    blocks: list[str] = []
    for recipe, info in _ordered(recipes):
        cli_opts = [o for o in info.get("options", []) if o.get("cli_flag")]
        if not info.get("uses_ctx_size") and not cli_opts:
            continue
        blocks.append(f"#### {info.get('display_name', recipe)} (`{recipe}` recipe)\n")
        blocks.append("| Option | Description | Default |")
        blocks.append("|--------|-------------|---------|")
        if info.get("uses_ctx_size"):
            blocks.append("| `--ctx-size SIZE` | Context size for the model | auto |")
        for o in cli_opts:
            blocks.append(
                "| `{flag} {t}` | {h} | {d} |".format(
                    flag=o["cli_flag"],
                    t=o.get("type_name", ""),
                    h=md_escape(o.get("help", "")),
                    d=_cli_default(o),
                )
            )
        blocks.append("")
    return "\n".join(blocks).rstrip()


def _oxford(items: list) -> str:
    items = [f"`{i}`" for i in items]
    if len(items) <= 1:
        return "".join(items)
    if len(items) == 2:
        return f"{items[0]} and {items[1]}"
    return ", ".join(items[:-1]) + f", and {items[-1]}"


def _js_to_title(recipe: str) -> str:
    # Mirror models.js toTitle(): the website's fallback for unlisted display names.
    return re.sub(
        r"\b\w",
        lambda m: m.group(0).upper(),
        recipe.replace("_", " ").replace("-", " "),
    )


def _js_key(recipe: str) -> str:
    # Bare identifier if it's a valid JS key, else quoted (matches models.js style).
    return recipe if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", recipe) else f"'{recipe}'"


def _web_display_name(info: dict, recipe: str) -> str:
    return info.get("web_display_name") or info.get("display_name", recipe)


def render_models_js(recipes: dict) -> str:
    # RECIPE_PRIORITY: every descriptor-backed recipe, ordered alphabetically by
    # display name. Listing all of them (rather than an opt-in subset) means a new
    # backend can never be silently dropped from the website.
    prioritized = sorted(
        recipes, key=lambda r: _web_display_name(recipes[r], r).lower()
    )
    pri_lines = ",\n".join(f"  '{r}'" for r in prioritized)

    # RECIPE_DISPLAY_NAMES: only recipes whose name differs from the JS toTitle()
    # fallback (matching the curated map, which omits redundant entries).
    name_lines = []
    for r, info in _ordered(recipes):
        name = _web_display_name(info, r)
        if name and name != _js_to_title(r):
            name_lines.append(f"  {_js_key(r)}: '{name}'")
    names = ",\n".join(name_lines)

    return (
        f"const RECIPE_PRIORITY = [\n{pri_lines}\n];\n\n"
        f"const RECIPE_DISPLAY_NAMES = {{\n{names}\n}};"
    )


def render_config_example(config: dict) -> str:
    # The canonical config.json, straight from a fresh lemond's /internal/config.
    # `port` is the only environment-dependent field (it reflects the launch port);
    # normalize it to the documented default.
    cfg = dict(config)
    cfg["port"] = 13305
    return "```json\n" + json.dumps(cfg, indent=2) + "\n```"


def render_recipe_values(recipes: dict) -> str:
    # Inline list of recipe values for `--recipe`, plus the collection orchestrator.
    rs = [r for r, _ in _ordered(recipes)] + ["collection.omni"]
    return ", ".join(f"`{r}`" for r in rs)


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

<!-- This file is generated by docs/tools/gen_backend_boilerplate.py from the C++ backend
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
        # Accept HTML (`<!-- ... -->`) markers for Markdown and block (`/* ... */`)
        # markers for code files like .js, so the same generator drives both.
        mid = re.escape(marker_id)
        begin = (
            r"(<!-- BEGIN GENERATED: "
            + mid
            + r" -->|/\* BEGIN GENERATED: "
            + mid
            + r" \*/)"
        )
        end = (
            r"(<!-- END GENERATED: "
            + mid
            + r" -->|/\* END GENERATED: "
            + mid
            + r" \*/)"
        )
        pattern = re.compile(begin + r".*?" + end, re.DOTALL)
        m = pattern.search(text)
        if not m:
            sys.exit(f"Marker region '{marker_id}' not found in target doc")

        # Inline regions (markers mid-line, e.g. inside a table cell) get no
        # surrounding newlines; block regions are wrapped on their own lines.
        inline = m.start() > 0 and text[m.start() - 1] != "\n"
        # Escape backslashes and group-ref markers in the body for re.sub.
        safe_body = body.replace("\\", "\\\\")
        sep = "" if inline else "\n"
        replacement = r"\1" + sep + safe_body + sep + r"\2"
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
        config = server.config()
        defaults_text = server.config_defaults_text()
    recipes = info.get("recipes", {})
    if not recipes:
        sys.exit("/system-info returned no recipes")
    if not config:
        sys.exit("/internal/config returned nothing")

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
        REPO_ROOT
        / "docs"
        / "guide"
        / "cli.md": {
            "sections": {"cli-recipe-options": render_cli_recipe_options(recipes)},
        },
        REPO_ROOT
        / "docs"
        / "guide"
        / "configuration"
        / "custom-models.md": {
            "sections": {"recipe-values": render_recipe_values(recipes)},
        },
        REPO_ROOT
        / "docs"
        / "guide"
        / "configuration"
        / "README.md": {
            "sections": {"config-example": render_config_example(config)},
        },
        REPO_ROOT
        / "docs"
        / "assets"
        / "models.js": {
            "sections": {"models-js-recipes": render_models_js(recipes)},
        },
    }

    # Whole-file generated artifacts (not marker-delimited): resources/defaults.json
    # is the canonical default config, mirrored verbatim from GET
    # /internal/config/defaults (per-recipe blocks come from the descriptors).
    raw_targets: dict = {
        REPO_ROOT / "src" / "cpp" / "resources" / "defaults.json": defaults_text,
    }

    stale = []
    for path, content in raw_targets.items():
        rel = path.relative_to(REPO_ROOT)
        if args.check:
            if not path.exists() or path.read_text() != content:
                stale.append(str(rel))
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
            print(f"Wrote {rel}")

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
                "Stale generated files: "
                + ", ".join(stale)
                + "\nRun: python docs/tools/gen_backend_boilerplate.py"
            )
        print("All generated files are up to date.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
