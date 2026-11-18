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
    "docs/guide/app/README.md",
    "docs/guide/app/models.md",
    "docs/guide/app/chat.md",
    "docs/guide/app/audio-and-3d.md",
    "docs/guide/app/operations.md",
)
DOCS = (
    *PAGES,
    "docs/dev/app.md",
    "docs/guide/README.md",
    "src/app/README.md",
    "src/app/ACCESSIBILITY.md",
)
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
        if re.search(r"\bGUI[0-9]+\b|gui[0-9]+\.md", visible_text, flags=re.I):
            complain(
                f"{filename}: contains obsolete generation-specific GUI branding/link"
            )
        inspect_links(path, text)
        if filename in PAGES and filename.removeprefix("docs/") not in nav:
            complain(f"{filename}: page not referenced by mkdocs.yml navigation")

    for legacy_guide in (ROOT / "docs/guide").glob("gui[0-9]*.md"):
        complain(f"obsolete guide still exists: {legacy_guide.relative_to(ROOT)}")
    for legacy_guide in (ROOT / "docs/guide").glob("app*.md"):
        complain(f"obsolete app guide still exists: {legacy_guide.relative_to(ROOT)}")
    if (ROOT / "src/app/docs/audio-and-3d-modalities.md").exists():
        complain("unpublished src/app/docs/audio-and-3d-modalities.md still exists")

    readme_path = ROOT / "src/app/README.md"
    if readme_path.is_file() and not re.search(
        r"\]\(ACCESSIBILITY\.md(?:#[^)]*)?\)",
        readme_path.read_text(encoding="utf-8"),
        flags=re.I,
    ):
        complain("src/app/README.md: missing link to ACCESSIBILITY.md")

    accessibility_path = ROOT / "src/app/ACCESSIBILITY.md"
    if accessibility_path.is_file():
        accessibility_text = accessibility_path.read_text(encoding="utf-8")
        if not re.search(r"\]\(tests/a11y\.spec\.ts(?:#[^)]*)?\)", accessibility_text):
            complain("src/app/ACCESSIBILITY.md: missing link to tests/a11y.spec.ts")
        if "npm run test:a11y" not in accessibility_text:
            complain("src/app/ACCESSIBILITY.md: missing npm run test:a11y command")

    manifest_path = ROOT / "src/app/package.json"
    if manifest_path.is_file():
        scripts = json.loads(manifest_path.read_text(encoding="utf-8")).get(
            "scripts", {}
        )
        for filename in (
            "docs/dev/app.md",
            "src/app/README.md",
            "src/app/ACCESSIBILITY.md",
        ):
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
