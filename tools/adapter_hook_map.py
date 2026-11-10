#!/usr/bin/env python3
"""Checks that the Lemonade internals the adapter backend uses have not moved.

The adapter backend (src/cpp/server/backends/adapter/) is the one in-tree piece that out-of-tree engines plug
into. hook_map.json next to it lists every Lemonade declaration it relies on, with a fingerprint of that
declaration's text. When a change here touches one of them, this check names the entry, so the person making the
change can update the adapter backend in the same PR (or say in the PR that it needs a follow-up) and refresh the
fingerprint. Engine adapters themselves live out of tree and never need a Lemonade change.

    python tools/adapter_hook_map.py --check     # CI: exit 1 and name each moved hook
    python tools/adapter_hook_map.py --update    # after adapting: write the new fingerprints
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAP = ROOT / "src/cpp/server/backends/adapter/hook_map.json"


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"(//|#(?!include|pragma|define|if|endif|else)).*", " ", text)


def extract(hook: dict) -> str | None:
    """The declaration text a hook names, or None when its anchor is gone."""
    path = ROOT / hook["file"]
    if not path.exists():
        return None
    text = path.read_text(encoding="utf-8", errors="replace")
    m = re.search(hook["anchor"], text, flags=re.M)
    if not m:
        return None
    start = m.start()
    if (
        hook.get("kind", "decl") == "block"
    ):  # from the anchor to the brace that closes its body
        open_at = text.find("{", m.end())
        if open_at < 0:
            return None
        depth = 0
        for i in range(open_at, len(text)):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    return text[start : i + 1]
        return None
    if (
        hook.get("kind") == "paren"
    ):  # from the anchor to the parenthesis that closes it (CMake)
        open_at = text.find("(", m.start())
        depth = 0
        for i in range(open_at, len(text)):
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
                if depth == 0:
                    return text[start : i + 1]
        return None
    ends = [
        i for i in (text.find(";", m.end()), text.find("{", m.end())) if i >= 0
    ]  # a declaration
    return text[start : min(ends)] if ends else None


def fingerprint(text: str, ignore: str | None = None) -> str:
    body = _strip_comments(text)
    if ignore:  # lines the adapter does not depend on (e.g. the list of
        body = re.sub(
            ignore, " ", body, flags=re.M
        )  # other backends in LEMON_BACKENDS)
    return hashlib.sha256(" ".join(body.split()).encode()).hexdigest()[:16]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--check",
        action="store_true",
        help="exit 1 when a hook moved or is missing (default)",
    )
    ap.add_argument(
        "--update",
        action="store_true",
        help="write the current fingerprints into hook_map.json",
    )
    a = ap.parse_args()
    data = json.loads(MAP.read_text(encoding="utf-8"))
    moved, missing = [], []
    for h in data["hooks"]:
        text = extract(h)
        if text is None:
            missing.append(h)
            continue
        fp = fingerprint(text, h.get("ignore"))
        if fp != h.get("fingerprint"):
            moved.append((h, fp))
            if a.update:
                h["fingerprint"] = fp
    if a.update:
        if missing:
            for h in missing:
                print(
                    f"MISSING {h['id']}: {h['file']} has no match for {h['anchor']!r}; fix the anchor first",
                    file=sys.stderr,
                )
            return 1
        MAP.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        print(
            f"hook_map.json: {len(moved)} fingerprint(s) updated, {len(data['hooks'])} hooks"
        )
        return 0
    for h in missing:
        print(f"MISSING {h['id']} ({h['file']}): {h['used_for']}")
    for h, fp in moved:
        print(f"MOVED   {h['id']} ({h['file']}): {h['used_for']}")
    if moved or missing:
        print(
            "\nThe adapter backend depends on the hooks above. Adapt src/cpp/server/backends/adapter/ if the change "
            "affects it, then run: python tools/adapter_hook_map.py --update"
        )
        return 1
    print(f"adapter hooks unchanged ({len(data['hooks'])})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
