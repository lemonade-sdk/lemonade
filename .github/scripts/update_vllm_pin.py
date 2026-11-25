#!/usr/bin/env python3
"""Find AMD's newest vLLM device-all bundle, or pin one in backend_versions.json.

    update_vllm_pin.py discover            prints stem=... and sha256=... lines
    update_vllm_pin.py write STEM SHA256   sets vllm.rocm and checksums.vllm.rocm

Only dated files are considered: the -latest alias changes every night, so it
can be neither pinned nor hash-checked.
"""

from __future__ import annotations

import json
import re
import sys
import urllib.request

INDEX_URL = (
    "https://rocm.frameworks-nightlies.amd.com/whl-multi-arch-staging/vllm/tarball/"
)
VERSIONS_PATH = "src/cpp/resources/backend_versions.json"
BUNDLE_RE = re.compile(
    r'href="(vllm-portable-rocm-device-all-[^"/]*?\.d(\d{8})\.[^"/]*-py314-x64)\.tar\.gz"'
)
SHA256_RE = re.compile(r"[0-9a-f]{64}")


def fetch(url: str) -> str:
    with urllib.request.urlopen(url, timeout=60) as response:
        return response.read().decode("utf-8")


def discover() -> tuple[str, str]:
    bundles = BUNDLE_RE.findall(fetch(INDEX_URL))
    if not bundles:
        raise SystemExit(f"No dated device-all py314 bundle listed at {INDEX_URL}")
    stem = max(bundles, key=lambda b: (b[1], b[0]))[0]
    sidecar = fetch(f"{INDEX_URL}{stem}.tar.gz.sha256").split()
    if (
        len(sidecar) != 2
        or not SHA256_RE.fullmatch(sidecar[0])
        or sidecar[1] != f"{stem}.tar.gz"
    ):
        raise SystemExit(f"Unexpected {stem}.tar.gz.sha256 contents: {sidecar}")
    return stem, sidecar[0]


def write(stem: str, sha256: str) -> None:
    if not SHA256_RE.fullmatch(sha256):
        raise SystemExit(f"Not a SHA-256 hex digest: {sha256}")
    with open(VERSIONS_PATH, encoding="utf-8") as f:
        data = json.load(f)
    old = data["vllm"].get("rocm", "")
    data["vllm"]["rocm"] = stem
    data["checksums"].setdefault("vllm", {})["rocm"] = {
        stem: {f"{stem}.tar.gz": f"sha256:{sha256}"}
    }
    with open(VERSIONS_PATH, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
    print(f"vllm.rocm: {old} -> {stem}")


def main() -> None:
    if sys.argv[1:2] == ["discover"] and len(sys.argv) == 2:
        stem, sha256 = discover()
        print(f"stem={stem}")
        print(f"sha256={sha256}")
    elif sys.argv[1:2] == ["write"] and len(sys.argv) == 4:
        write(sys.argv[2], sys.argv[3])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
