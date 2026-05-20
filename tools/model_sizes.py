#!/usr/bin/env python3
"""Compute model sizes (and approximate parameter counts) from the HuggingFace API.

Reads server_models.json, queries HF for every checkpoint each model declares
(mirroring the variant matching in src/cpp/server/hf_variants.cpp), and prints
a table of computed vs. current sizes. Never touches the local HF cache.

Defaults to dry-run. Pass --update to write the computed sizes back to
server_models.json (only entries that have a `size` field are written; the
new value is in gigabytes, decimal).

Usage:
  tools/model_sizes.py src/cpp/resources/server_models.json --all
  tools/model_sizes.py src/cpp/resources/server_models.json --models Qwen3.5-4B-GGUF "Ultra Collection"
  tools/model_sizes.py src/cpp/resources/server_models.json --all --update
"""

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

# Quant token regex — ported from src/cpp/server/hf_variants.cpp::quant_regex().
QUANT_RE = re.compile(
    r"(?:^|[-._/])((?:UD[-_])?(?:Q\d+(?:_\d)?(?:_K)?(?:_(?:M|S|L|XL|XXL))?"
    r"|IQ\d+(?:_(?:M|S|L|XS|XXS|NL))?|F(?:16|32)|BF16|MXFP\d+(?:_MOE)?))"
    r"(?=[-._/]|\.gguf$|$)",
    re.IGNORECASE,
)
DIRECT_FILE_SUFFIXES = (
    ".safetensors",
    ".pth",
    ".ckpt",
    ".gguf",
    ".onnx",
    ".bin",
    ".rai",
)
# Matches the `-NNNNN-of-NNNNN` shard suffix on GGUF filenames.
SHARD_RE = re.compile(r"-\d{5}-of-\d{5}(?=\.gguf$|$)", re.IGNORECASE)


def extract_quant(s: str):
    m = QUANT_RE.search(s)
    return m.group(1).upper() if m else None


def fetch_repo(repo_id: str, cache: dict):
    """Return {files: [(name, size)], params: int|None} for a repo, or None on failure."""
    if repo_id in cache:
        return cache[repo_id]
    url = f"https://huggingface.co/api/models/{repo_id}?blobs=true"
    headers = {}
    tok = os.environ.get("HF_TOKEN")
    if tok:
        headers["Authorization"] = f"Bearer {tok}"
    try:
        req = urllib.request.Request(url, headers=headers)
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = json.loads(resp.read())
    except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError) as e:
        print(f"  ! HF API error for {repo_id}: {e}", file=sys.stderr)
        cache[repo_id] = None
        return None
    files = [
        (s["rfilename"], int(s.get("size") or 0))
        for s in data.get("siblings", [])
        if "rfilename" in s
    ]
    params = None
    if isinstance(data.get("gguf"), dict):
        params = data["gguf"].get("total")
    elif isinstance(data.get("safetensors"), dict):
        params = data["safetensors"].get("total")
    cache[repo_id] = {"files": files, "params": params}
    return cache[repo_id]


def files_for_variant(files, variant: str):
    """Return [(name, size)] for the variant — mirrors hf_variants.cpp."""
    if not variant or variant == "*":
        return list(files)
    for name, size in files:
        if name == variant:
            return [(name, size)]
    if variant.lower().endswith(DIRECT_FILE_SUFFIXES):
        for name, size in files:
            if os.path.basename(name) == variant:
                return [(name, size)]
        return []
    target = variant.upper()
    folder, root = [], []
    for name, size in files:
        ln = name.lower()
        if not ln.endswith(".gguf") or "mmproj" in ln:
            continue
        sl = name.find("/")
        if sl > 0:
            folder_q = extract_quant(name[:sl]) or name[:sl]
            if folder_q.upper() == target or name[:sl] == variant:
                folder.append((name, size))
        else:
            q = extract_quant(name)
            if q and q.upper() == target:
                root.append((name, size))
    if folder:
        return folder
    # Some repos publish both a single concat file (e.g. `model-q4_k_m.gguf`)
    # and an equivalent shard set (e.g. `model-q4_k_m-00001-of-00003.gguf`)
    # for the same quant. They represent the same logical variant, so pick
    # whichever set is present — preferring the single file when both exist.
    singles = [(n, s) for n, s in root if not SHARD_RE.search(n)]
    if singles:
        return singles
    return root


def parse_checkpoint(ckpt: str):
    if ":" in ckpt:
        return tuple(ckpt.split(":", 1))
    return ckpt, ""


def compute(name, registry, cache, seen=None):
    """Returns (size_bytes, params, status) where status is 'ok', 'error', or 'empty'.

    Collections recurse; params are taken once per unique HF repo so models
    sharing a repo (mmproj in main repo) aren't double-counted.
    """
    if seen is None:
        seen = set()
    if name in seen:
        return 0, 0, "ok"
    seen.add(name)

    entry = registry.get(name)
    if entry is None:
        return None, None, "error"

    if entry.get("recipe", "").startswith("collection."):
        total_size, total_params = 0, 0
        for comp in entry.get("components", []):
            cs, cp, _ = compute(comp, registry, cache, seen)
            if cs is not None:
                total_size += cs
            if cp is not None:
                total_params += cp
        return total_size, total_params, "ok"

    sources = []
    if entry.get("checkpoint"):
        sources.append(("main", entry["checkpoint"]))
    for k, v in (entry.get("checkpoints") or {}).items():
        if k == "npu_cache":
            continue
        sources.append((k, v))
    if not sources:
        return None, None, "empty"

    total_size = 0
    seen_repos = set()
    total_params = 0
    main_repo = None
    any_match = False
    for label, ckpt in sources:
        repo_id, variant = parse_checkpoint(ckpt)
        if label == "main":
            main_repo = repo_id
        info = fetch_repo(repo_id, cache)
        if info is None:
            return None, None, "error"
        matched = files_for_variant(info["files"], variant)
        if matched:
            any_match = True
        else:
            print(
                f"  ! no files matched in {repo_id} for variant '{variant}'",
                file=sys.stderr,
            )
        for _, sz in matched:
            total_size += sz
        if repo_id not in seen_repos:
            seen_repos.add(repo_id)
            if info["params"]:
                total_params += info["params"]

    mmproj = entry.get("mmproj")
    if mmproj and main_repo:
        info = fetch_repo(main_repo, cache)
        if info:
            for nm, sz in info["files"]:
                if nm == mmproj or os.path.basename(nm) == mmproj:
                    total_size += sz
                    break

    return total_size, (total_params or None), ("ok" if any_match else "empty")


def round_gb(b: int) -> float:
    g = b / 1e9
    if g >= 10:
        return round(g, 1)
    if g >= 1:
        return round(g, 2)
    if g >= 0.1:
        return round(g, 3)
    return round(g, 4)


def fmt_params(p):
    if not p:
        return "—"
    for unit, scale in [("T", 1e12), ("B", 1e9), ("M", 1e6), ("K", 1e3)]:
        if p >= scale:
            return f"{p / scale:.2f}{unit}"
    return str(p)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("models_json", type=Path)
    g = parser.add_mutually_exclusive_group(required=True)
    g.add_argument(
        "--all", action="store_true", help="Process every entry in the registry"
    )
    g.add_argument(
        "--models", nargs="+", metavar="NAME", help="Process the listed models"
    )
    parser.add_argument(
        "--update",
        action="store_true",
        help="Write computed sizes back to JSON (default: dry-run)",
    )
    args = parser.parse_args()

    with open(args.models_json) as f:
        registry = json.load(f)

    if args.all:
        targets = list(registry.keys())
    else:
        unknown = [n for n in args.models if n not in registry]
        if unknown:
            sys.exit(f"Unknown model(s): {', '.join(unknown)}")
        targets = args.models

    cache = {}
    rows = []
    print(f"Querying HF API for {len(targets)} model(s)...\n", file=sys.stderr)
    for i, name in enumerate(targets, 1):
        print(f"[{i}/{len(targets)}] {name}", file=sys.stderr)
        sz, pa, status = compute(name, registry, cache)
        entry = registry[name]
        rows.append(
            {
                "name": name,
                "recipe": entry.get("recipe", "—"),
                "old": entry.get("size"),
                "new_gb": round_gb(sz) if sz else None,
                "params": pa,
                "status": status,
            }
        )

    # Table
    print()
    name_w = max(max(len(r["name"]) for r in rows), len("Model"))
    recipe_w = max(max(len(r["recipe"]) for r in rows), len("Recipe"))
    print(
        f"{'Model':<{name_w}}  {'Recipe':<{recipe_w}}  {'Old (GB)':>10}  {'New (GB)':>10}  {'Δ':>9}  {'~Params':>10}"
    )
    print("-" * (name_w + recipe_w + 4 + 10 + 2 + 10 + 2 + 9 + 2 + 10))
    changes = []
    errors = []
    for r in rows:
        old_s = f"{r['old']:.4g}" if r["old"] is not None else "—"
        new_s = (
            f"{r['new_gb']:.4g}"
            if r["new_gb"] is not None
            else ("ERR" if r["status"] == "error" else "—")
        )
        if (
            r["old"] is not None
            and r["new_gb"] is not None
            and abs(r["new_gb"] - r["old"]) >= 0.005
        ):
            delta_s = f"{r['new_gb'] - r['old']:+.4g}"
            changes.append(r)
        else:
            delta_s = ""
        if r["status"] == "error":
            errors.append(r["name"])
        print(
            f"{r['name']:<{name_w}}  {r['recipe']:<{recipe_w}}  {old_s:>10}  {new_s:>10}  {delta_s:>9}  {fmt_params(r['params']):>10}"
        )

    print()
    print(f"Changes:   {len(changes)}")
    print(f"Errors:    {len(errors)}")
    if errors:
        for n in errors:
            print(f"  ! {n}", file=sys.stderr)

    if args.update:
        if not changes:
            print("\nNo changes to write.")
            return
        for r in changes:
            registry[r["name"]]["size"] = r["new_gb"]
        with open(args.models_json, "w") as f:
            json.dump(registry, f, indent=4)
            f.write("\n")
        print(f"\nWrote {len(changes)} update(s) to {args.models_json}")
    else:
        print(f"\n(dry-run) Re-run with --update to apply {len(changes)} change(s).")


if __name__ == "__main__":
    main()
