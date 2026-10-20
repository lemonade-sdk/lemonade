#!/usr/bin/env python3
"""Static App documentation contract; run in CI and when editing the App UI.

This verifies links, navigation, npm commands, and a few essential UI anchors.
It does not replace a manual walkthrough or real-server end-to-end tests.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PAGES = (
    "docs/guide/app.md",
    "docs/guide/app-models.md",
    "docs/guide/app-chat.md",
    "docs/guide/app-audio-and-3d.md",
    "docs/guide/app-operations.md",
)
DOCS = (*PAGES, "docs/dev/app.md", "docs/guide/README.md", "src/app/README.md")
ERRORS = []


def complain(message: str) -> None:
    ERRORS.append(message)


def inspect_links(path: Path, text: str) -> None:
    # Only the standard Markdown link shape; this deliberately ignores external
    # URLs, mailto:, data:, and links in HTML or reference-style definitions.
    for target in re.findall(r"\]\(([^)]+)\)", text):
        value = target.split(' "', 1)[0].strip("<>")
        if not value or value.startswith(("#", "https:", "http:", "mailto:", "data:")):
            continue
        rel = value.split("#", 1)[0].split("?", 1)[0]
        if rel and not (path.parent / rel).exists():
            complain(f"{path.relative_to(ROOT)}: broken link {value}")


def main() -> int:
    nav_path = ROOT / "mkdocs.yml"
    if not nav_path.is_file():
        complain("mkdocs.yml does not exist")
        nav = ""
    else:
        nav = nav_path.read_text(encoding="utf-8")

    for filename in DOCS:
        path = ROOT / filename
        if not path.is_file():
            complain(f"missing documentation: {filename}")
            continue
        text = path.read_text(encoding="utf-8")
        visible_text = re.sub(r"https?://[^\s)]+", "", text)
        if re.search(r"\bGUI3\b|gui3\.md", visible_text, flags=re.I):
            complain(f"{filename}: contains obsolete GUI3 branding/link")
        inspect_links(path, text)
        if filename in PAGES and filename.removeprefix("docs/") not in nav:
            complain(f"{filename}: page not referenced by mkdocs.yml navigation")

    if (ROOT / "docs/guide/gui3.md").exists():
        complain("old docs/guide/gui3.md still exists")
    if (ROOT / "src/app/docs/audio-and-3d-modalities.md").exists():
        complain("unpublished src/app/docs/audio-and-3d-modalities.md still exists")

    manifest_path = ROOT / "src/app/package.json"
    if manifest_path.is_file():
        scripts = json.loads(manifest_path.read_text(encoding="utf-8")).get(
            "scripts", {}
        )
        for filename in ("docs/dev/app.md", "src/app/README.md"):
            path = ROOT / filename
            if not path.exists():
                continue
            for command in re.findall(
                r"\bnpm run ([A-Za-z0-9:_-]+)", path.read_text(encoding="utf-8")
            ):
                if command not in scripts:
                    complain(f"{filename}: unknown npm script {command}")

    # These source files serve as low-cost tripwires for major UI renames.
    anchors = {
        "src/app/src/App.tsx": (
            "Chat",
            "Models",
            "Backends",
            "Apps",
            "Monitor",
            "Settings",
        ),
        "src/app/src/components/ConnectView.tsx": ("Connect",),
        "src/app/src/components/EffectiveSettingsModal.tsx": ("Save sampling",),
    }
    for filename, expected in anchors.items():
        path = ROOT / filename
        if not path.is_file():
            complain(f"missing App source: {filename}")
            continue
        source = path.read_text(encoding="utf-8")
        for label in expected:
            if label not in source:
                complain(f"{filename}: documented UI anchor missing: {label!r}")

    if ERRORS:
        for message in ERRORS:
            print("FAIL:", message, file=sys.stderr)
        return 1
    print(
        f"PASS: {len(DOCS)} documentation pages, navigation, relative links, scripts and UI anchors"
    )
    print(
        "Note: manual App workflow + real-server smoke tests are still required for behavioral accuracy."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
