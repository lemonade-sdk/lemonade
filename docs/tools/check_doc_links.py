#!/usr/bin/env python3
"""Verify that every `#fragment` in a relative Markdown link resolves.

markdown-link-check, which CI already runs, stops at the file: it confirms
`guide.md` exists and never looks at `#a-heading`. So a reworded heading breaks
every inbound link silently, and the reader only finds out by landing at the top
of the wrong page. This covers that gap, over every document rather than the
ones a change happens to touch.
"""

import pathlib
import re
import unicodedata
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SKIP = {"node_modules", "build", ".git", "_deps"}
LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*$")
EXPLICIT_ANCHOR = re.compile(r'<a\s+(?:id|name)="([^"]+)"', re.IGNORECASE)


def slugify(text):
    """Reproduce python-markdown's toc slug for a heading's rendered text."""
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    # Strip HTML tags, but not the <angle brackets> inside a code span.
    spans = text.split("`")
    text = "".join(
        part if index % 2 else re.sub(r"<[^>]+>", "", part)
        for index, part in enumerate(spans)
    )
    text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode("ascii")
    text = re.sub(r"[^\w\s-]", "", text).strip().lower()
    return re.sub(r"[-\s]+", "-", text)


def anchors_of(path):
    """Collect the anchors a page offers, ignoring anything inside a code fence
    so a commented-out `# heading` in a shell example cannot mask a real break."""
    found = set()
    fence = None
    for line in path.read_text(encoding="utf-8").split("\n"):
        marker = re.match(r"\s*(`{3,}|~{3,})", line)
        if marker:
            token = marker.group(1)[0] * 3
            if fence is None:
                fence = token
            elif token == fence:
                fence = None
            continue
        if fence is not None:
            continue
        heading = HEADING.match(line)
        if heading:
            found.add(slugify(heading.group(2)))
        found.update(EXPLICIT_ANCHOR.findall(line))
    return found


def main():
    sources = [
        p for p in ROOT.rglob("*.md") if not SKIP & set(p.relative_to(ROOT).parts)
    ]
    anchors = {p: anchors_of(p) for p in sources}
    broken = []
    checked = 0

    for path in sorted(anchors):
        for target in LINK.findall(path.read_text(encoding="utf-8")):
            if target.startswith(("http://", "https://", "mailto:", "data:", "//")):
                continue
            file_part, _, fragment = target.partition("#")
            dest = path if not file_part else (path.parent / file_part).resolve()
            if not fragment or dest.suffix != ".md":
                continue
            checked += 1
            if dest not in anchors:
                continue  # markdown-link-check owns missing files
            if fragment not in anchors[dest]:
                broken.append(f"{path.relative_to(ROOT)} -> {target}")

    for item in broken:
        print(f"  {item}")
    print(f"{checked} relative links with anchors checked, {len(broken)} broken")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
