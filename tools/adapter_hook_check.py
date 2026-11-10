#!/usr/bin/env python3
"""Re-greps every entry of test/cpp/adapter_spec/hook_map.json against the working tree and prints the ones
that no longer match (file gone, pattern gone, or the declaration head text differs from the recorded
signature). Exit 1 when any entry is off. No dependencies.

    python3 tools/adapter_hook_check.py [--map PATH] [--root DIR]
"""

import argparse, json, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def norm(s):
    return re.sub(r"\s+", " ", s).strip()


def heads(text, pat):
    out = []
    for m in re.finditer(pat, text, flags=re.M):
        i = m.start()
        k = re.search(r"[;{]", text[i : i + 600])
        j = i + (k.end() if k else 200)
        out.append(norm(text[i:j]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(ROOT))
    ap.add_argument("--map", default=None)
    a = ap.parse_args()
    root = Path(a.root)
    mp = Path(a.map) if a.map else ROOT / "test/cpp/adapter_spec/hook_map.json"
    hooks = json.loads(mp.read_text())["hooks"]
    bad = 0
    for h in hooks:
        f = root / h["file"]
        if not f.exists():
            why = "file missing"
        else:
            found = heads(f.read_text(encoding="utf-8", errors="replace"), h["pattern"])
            if not found:
                why = "pattern no longer matches"
            elif h["signature"][:300] not in [x[:300] for x in found]:
                why = "signature changed: now " + found[0][:200]
            else:
                continue
        bad += 1
        print(f"NO MATCH  {h['file']}  {h['symbol']}: {why}")
    print(f"{len(hooks) - bad}/{len(hooks)} hooks match")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
