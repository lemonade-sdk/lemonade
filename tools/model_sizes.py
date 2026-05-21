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
  tools/model_sizes.py src/cpp/resources/server_models.json --models Qwen3.5-4B-MTP-GGUF LMX-Omni-52B-Halo
  tools/model_sizes.py src/cpp/resources/server_models.json --all --update
"""

import argparse
import json
import os
import re
import struct
import sys
import urllib.error
import urllib.parse
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
GGUF_VALUE_SIZES = {
    0: 1,  # UINT8
    1: 1,  # INT8
    2: 2,  # UINT16
    3: 2,  # INT16
    4: 4,  # UINT32
    5: 4,  # INT32
    6: 4,  # FLOAT32
    7: 1,  # BOOL
    10: 8,  # UINT64
    11: 8,  # INT64
    12: 8,  # FLOAT64
}


class NeedMoreData(Exception):
    pass


class BinaryReader:
    def __init__(self, data: bytes):
        self.data = data
        self.offset = 0

    def read(self, n: int) -> bytes:
        if self.offset + n > len(self.data):
            raise NeedMoreData()
        out = self.data[self.offset : self.offset + n]
        self.offset += n
        return out

    def u32(self) -> int:
        return struct.unpack("<I", self.read(4))[0]

    def u64(self) -> int:
        return struct.unpack("<Q", self.read(8))[0]

    def gguf_string(self) -> str:
        n = self.u64()
        return self.read(n).decode("utf-8")


def extract_quant(s: str):
    m = QUANT_RE.search(s)
    return m.group(1).upper() if m else None


def hf_headers(extra=None):
    headers = dict(extra or {})
    tok = os.environ.get("HF_TOKEN")
    if tok:
        headers["Authorization"] = f"Bearer {tok}"
    return headers


def fetch_repo(repo_id: str, cache: dict):
    """Return {files: [(name, size)], params: int|None} for a repo, or None on failure."""
    if repo_id in cache:
        return cache[repo_id]
    url = f"https://huggingface.co/api/models/{repo_id}?blobs=true"
    try:
        req = urllib.request.Request(url, headers=hf_headers())
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


def fetch_range(repo_id: str, filename: str, start: int, end: int):
    quoted_name = urllib.parse.quote(filename)
    url = f"https://huggingface.co/{repo_id}/resolve/main/{quoted_name}"
    req = urllib.request.Request(
        url,
        headers=hf_headers({"Range": f"bytes={start}-{end}"}),
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        return resp.read()


def count_safetensors_params(repo_id: str, filename: str, cache: dict):
    """Count tensors from a safetensors header without downloading the weights."""
    key = ("safetensors", repo_id, filename)
    if key in cache:
        return cache[key]
    try:
        header_len_raw = fetch_range(repo_id, filename, 0, 7)
        if len(header_len_raw) != 8:
            cache[key] = None
            return None
        header_len = int.from_bytes(header_len_raw, "little")
        header_raw = fetch_range(repo_id, filename, 8, 8 + header_len - 1)
        header = json.loads(header_raw.decode("utf-8"))
    except (
        OSError,
        UnicodeDecodeError,
        json.JSONDecodeError,
        urllib.error.HTTPError,
        urllib.error.URLError,
        TimeoutError,
    ) as e:
        print(
            f"  ! could not read safetensors header for {repo_id}:{filename}: {e}",
            file=sys.stderr,
        )
        cache[key] = None
        return None

    total = 0
    for name, meta in header.items():
        if name == "__metadata__":
            continue
        shape = meta.get("shape")
        if shape is None:
            continue
        n = 1
        for dim in shape:
            n *= dim
        total += n
    cache[key] = total or None
    return cache[key]


def skip_gguf_value(reader: BinaryReader, value_type: int):
    if value_type in GGUF_VALUE_SIZES:
        reader.read(GGUF_VALUE_SIZES[value_type])
    elif value_type == 8:  # STRING
        reader.gguf_string()
    elif value_type == 9:  # ARRAY
        item_type = reader.u32()
        count = reader.u64()
        for _ in range(count):
            skip_gguf_value(reader, item_type)
    else:
        raise ValueError(f"unknown GGUF metadata value type {value_type}")


def parse_gguf_params(data: bytes):
    reader = BinaryReader(data)
    if reader.read(4) != b"GGUF":
        return None
    reader.u32()  # version
    tensor_count = reader.u64()
    metadata_count = reader.u64()

    for _ in range(metadata_count):
        reader.gguf_string()
        skip_gguf_value(reader, reader.u32())

    total = 0
    for _ in range(tensor_count):
        reader.gguf_string()
        dims = [reader.u64() for _ in range(reader.u32())]
        reader.u32()  # tensor type
        reader.u64()  # tensor data offset
        n = 1
        for dim in dims:
            n *= dim
        total += n
    return total or None


def count_gguf_params(repo_id: str, filename: str, cache: dict):
    """Count tensors from a GGUF header without downloading tensor data."""
    key = ("gguf", repo_id, filename)
    if key in cache:
        return cache[key]
    size = 1024 * 1024
    while size <= 64 * 1024 * 1024:
        try:
            data = fetch_range(repo_id, filename, 0, size - 1)
            params = parse_gguf_params(data)
            cache[key] = params
            return params
        except NeedMoreData:
            size *= 2
        except (
            OSError,
            UnicodeDecodeError,
            ValueError,
            struct.error,
            urllib.error.HTTPError,
            urllib.error.URLError,
            TimeoutError,
        ) as e:
            print(
                f"  ! could not read GGUF header for {repo_id}:{filename}: {e}",
                file=sys.stderr,
            )
            cache[key] = None
            return None
    print(
        f"  ! GGUF header for {repo_id}:{filename} exceeds 64 MiB",
        file=sys.stderr,
    )
    cache[key] = None
    return None


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
    """Returns (size_bytes, params, params_complete, status).

    Collections recurse; params are taken once per unique HF repo so models
    sharing a repo (mmproj in main repo) aren't double-counted. Some formats
    do not expose parameter counts through HF metadata; in that case the count
    is a lower bound and params_complete is False.
    """
    if seen is None:
        seen = set()
    if name in seen:
        return 0, 0, True, "ok"
    seen.add(name)

    entry = registry.get(name)
    if entry is None:
        return None, None, False, "error"

    if entry.get("recipe", "").startswith("collection."):
        total_size, total_params, params_complete = 0, 0, True
        status = "ok"
        for comp in entry.get("components", []):
            cs, cp, cpc, child_status = compute(comp, registry, cache, seen)
            if cs is not None:
                total_size += cs
            if cp is not None:
                total_params += cp
            if not cpc:
                params_complete = False
            if child_status == "error":
                status = "error"
        return total_size, total_params, params_complete, status

    sources = []
    if entry.get("checkpoint"):
        sources.append(("main", entry["checkpoint"]))
    for k, v in (entry.get("checkpoints") or {}).items():
        if k == "npu_cache":
            continue
        sources.append((k, v))
    if not sources:
        return None, None, False, "empty"

    total_size = 0
    seen_repos = set()
    total_params = 0
    params_complete = True
    main_repo = None
    any_match = False
    for label, ckpt in sources:
        repo_id, variant = parse_checkpoint(ckpt)
        if label == "main":
            main_repo = repo_id
        info = fetch_repo(repo_id, cache)
        if info is None:
            return None, None, False, "error"
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
                continue

        if info["params"]:
            continue

        matched_params = 0
        matched_complete = True
        for filename, _ in matched:
            if filename.lower().endswith(".safetensors"):
                params = count_safetensors_params(repo_id, filename, cache)
            elif filename.lower().endswith(".gguf"):
                params = count_gguf_params(repo_id, filename, cache)
            else:
                params = None
            if params is None:
                matched_complete = False
            else:
                matched_params += params
        total_params += matched_params
        if matched and not matched_complete:
            params_complete = False

    mmproj = entry.get("mmproj")
    if mmproj and main_repo:
        info = fetch_repo(main_repo, cache)
        if info:
            for nm, sz in info["files"]:
                if nm == mmproj or os.path.basename(nm) == mmproj:
                    total_size += sz
                    params = count_gguf_params(main_repo, nm, cache)
                    if params is None:
                        # mmproj counts are not present in the repo-level HF metadata
                        # used above, so the displayed count is a lower bound if header
                        # parsing fails.
                        params_complete = False
                    else:
                        total_params += params
                    break

    status = "ok" if any_match else "empty"
    return total_size, (total_params or None), params_complete, status


def round_gb(b: int) -> float:
    g = b / 1e9
    if g >= 10:
        return round(g, 1)
    if g >= 1:
        return round(g, 2)
    if g >= 0.1:
        return round(g, 3)
    return round(g, 4)


def fmt_params(p, complete=True):
    if not p:
        return "—" if complete else "unknown"
    for unit, scale in [("T", 1e12), ("B", 1e9), ("M", 1e6), ("K", 1e3)]:
        if p >= scale:
            s = f"{p / scale:.2f}{unit}"
            return s if complete else f"≥{s}"
    s = str(p)
    return s if complete else f"≥{s}"


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
        sz, pa, params_complete, status = compute(name, registry, cache)
        entry = registry[name]
        rows.append(
            {
                "name": name,
                "recipe": entry.get("recipe", "—"),
                "old": entry.get("size"),
                "new_gb": round_gb(sz) if sz else None,
                "params": pa,
                "params_complete": params_complete,
                "status": status,
            }
        )

    # Table
    print()
    name_w = max(max(len(r["name"]) for r in rows), len("Model"))
    recipe_w = max(max(len(r["recipe"]) for r in rows), len("Recipe"))
    print(
        f"{'Model':<{name_w}}  {'Recipe':<{recipe_w}}  "
        f"{'Old (GB)':>10}  {'New (GB)':>10}  {'Δ':>9}  {'~Params':>10}"
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
        params_s = fmt_params(r["params"], r["params_complete"])
        print(
            f"{r['name']:<{name_w}}  {r['recipe']:<{recipe_w}}  "
            f"{old_s:>10}  {new_s:>10}  {delta_s:>9}  {params_s:>10}"
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
