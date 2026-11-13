#!/usr/bin/env python3
"""Mirror the backend files Lemonade downloads into one ModelScope Datasets repo.

Lemonade only downloads the versions pinned in
src/cpp/resources/backend_versions.json, so that file is the list of what to copy.
Every file keeps its GitHub name. The repo is https://modelscope.cn/datasets/<owner>/<name>
and the path inside it is:

    github/<owner>/<repo>/<tag>/<file>      for GitHub release files
    therock/<version>/<file>                for the ROCm runtime from stable.repo.amd.com

Each version folder also gets an assets.json with the name, size and SHA-256 of
every file in it.

Examples:
    python modelscope_mirror.py --set trial --dry-run
    python modelscope_mirror.py --set trial --repo lemonade-sdk/backends --private
    python modelscope_mirror.py --set core --group vllm

Environment:
    MODELSCOPE_API_TOKEN   ModelScope access token (not needed for --dry-run)
    GITHUB_TOKEN           optional, raises the GitHub API rate limit from 60 to 5000 per hour
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import tempfile
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

try:
    # Use the operating system's certificate store. Corporate networks that inspect
    # TLS (Zscaler and similar) otherwise fail with CERTIFICATE_VERIFY_FAILED.
    import truststore
    truststore.inject_into_ssl()
except ImportError:
    pass

DEFAULT_REPO = "lemonade-sdk/backends"
LEMONADE_REPO = "lemonade-sdk/lemonade"
VERSIONS_PATH = "src/cpp/resources/backend_versions.json"
THEROCK_URL = "https://stable.repo.amd.com/rocm/core/tarball/"

# Client GPU families. Datacenter targets (gfx942, gfx950) are left out on purpose.
VLLM_FAMILIES = ["gfx110X", "gfx120X", "gfx1150", "gfx1151"]
THEROCK_FAMILIES = ["gfx103X-all", "gfx110X-all", "gfx1150", "gfx1151", "gfx1152", "gfx1153", "gfx120X-all"]
THEROCK_OS = ["windows", "linux"]

# Where each pinned backend comes from. This copies the repo names hardcoded in
# Lemonade's get_install_params() functions. If backend_versions.json gains a key
# that is not listed here, the script stops so the table gets updated.
#   recipe -> {variant or "*": (github repo, group)}
SOURCES: dict[str, dict[str, tuple[str, str]]] = {
    "llamacpp": {
        "cpu": ("ggml-org/llama.cpp", "llamacpp-upstream"),
        "vulkan": ("ggml-org/llama.cpp", "llamacpp-upstream"),
        "metal": ("ggml-org/llama.cpp", "llamacpp-upstream"),
        "rocm-stable": ("lemonade-sdk/llama.cpp", "llamacpp-lemonade"),
        "cuda": ("lemonade-sdk/llama.cpp", "llamacpp-lemonade"),
        "rocm-nightly": ("lemonade-sdk/llamacpp-rocm", "llamacpp-nightly"),
    },
    "llamacpp-hrx": {"*": ("ROCm/ggml-staging-automation", "small")},
    "whispercpp": {"*": ("lemonade-sdk/whisper.cpp-rocm", "whispercpp")},
    "sd-cpp": {"*": ("lemonade-sdk/stable-diffusion.cpp", "sdcpp")},
    "ryzenai-llm": {"*": ("lemonade-sdk/ryzenai-server", "npu")},
    "flm": {"*": ("ROCm/FastFlowLM", "npu")},
    "kokoro": {"*": ("lemonade-sdk/Kokoros", "small")},
    "vllm": {"rocm": ("lemonade-sdk/vllm-rocm", "vllm")},
    "thenoise": {"*": ("lemonade-sdk/thenoise", "thenoise")},
    "moonshine": {"*": ("lemonade-sdk/moonshine-server-rocm", "small")},
    "onnxruntime": {"*": ("lemonade-sdk/ort-server", "small")},
    # Personal repos of one contributor. Only mirrored with --include-pwilkin.
    "thinksound": {"*": ("pwilkin/thinksound.cpp", "pwilkin")},
    "acestep": {"*": ("pwilkin/acestep.cpp", "pwilkin")},
    "trellis": {"*": ("pwilkin/trellis.cpp", "pwilkin")},
    "openmoss": {"*": ("pwilkin/openmoss", "pwilkin")},
}
# Container backends need a container registry, not file hosting.
CONTAINER_RECIPES = {"rocmfpx", "halogen", "gufo", "ds4"}
NOT_BACKENDS = {"comment", "checksums", "therock", "rocm_asset_families", "clear_bin_if_lemonade_below"}

SETS = {
    # Small and fast: three backends, about 0.5 GB. Use this for the first trial.
    "trial": {"recipes": {"flm", "onnxruntime", "kokoro"}, "therock": False, "lemonade": False},
    "core": {"recipes": None, "therock": True, "lemonade": True},
}


@dataclass
class Item:
    group: str
    folder: str                       # folder inside the ModelScope repo
    source: str                       # human-readable source, written to assets.json
    files: list[dict] = field(default_factory=list)   # name, size, sha256, url


def log(msg: str) -> None:
    print(msg, flush=True)


def human(n: int) -> str:
    for unit, div in (("GB", 1e9), ("MB", 1e6), ("KB", 1e3)):
        if n >= div:
            return f"{n / div:.2f} {unit}"
    return f"{n} B"


def http_get(url: str, accept: str | None = None) -> bytes:
    headers = {"User-Agent": "lemonade-modelscope-mirror"}
    if accept:
        headers["Accept"] = accept
    if "api.github.com" in url and os.environ.get("GITHUB_TOKEN"):
        headers["Authorization"] = f"Bearer {os.environ['GITHUB_TOKEN']}"
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def head_size(url: str) -> int | None:
    req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": "lemonade-modelscope-mirror"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return int(r.headers.get("Content-Length", 0)) or None
    except urllib.error.HTTPError:
        return None


def github_release(repo: str, tag: str) -> dict:
    url = f"https://api.github.com/repos/{repo}/releases/" + ("latest" if tag == "latest" else f"tags/{tag}")
    try:
        return json.loads(http_get(url, "application/vnd.github+json"))
    except urllib.error.HTTPError as e:
        if e.code == 403:
            sys.exit("GitHub API rate limit hit. Set GITHUB_TOKEN and run again.")
        raise RuntimeError(f"{repo}@{tag}: GitHub returned HTTP {e.code}") from e


def load_versions(path: str | None, ref: str) -> dict:
    if path:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    url = f"https://raw.githubusercontent.com/{LEMONADE_REPO}/{ref}/{VERSIONS_PATH}"
    log(f"Reading pins from {url}")
    return json.loads(http_get(url))


def build_plan(versions: dict, set_name: str, include_pwilkin: bool, lemonade_tag: str,
               only_group: str | None = None) -> list[Item]:
    chosen = SETS[set_name]
    pins: dict[tuple[str, str], str] = {}     # (repo, tag) -> group
    for recipe, variants in versions.items():
        if recipe in NOT_BACKENDS or recipe in CONTAINER_RECIPES:
            continue
        if recipe not in SOURCES:
            sys.exit(f"backend_versions.json has a backend this script does not know: '{recipe}'. "
                     f"Add it to SOURCES (see its get_install_params() in Lemonade).")
        if chosen["recipes"] is not None and recipe not in chosen["recipes"]:
            continue
        for variant, tag in variants.items():
            if variant == "comment" or not isinstance(tag, str):
                continue                        # comments and vllm.rocm_arch_overrides (datacenter)
            if "@sha256:" in tag:
                continue                        # container image pin inside a native recipe
            src = SOURCES[recipe].get(variant) or SOURCES[recipe].get("*")
            if src is None:
                sys.exit(f"Unknown variant '{recipe}.{variant}'. Add it to SOURCES.")
            repo, group = src
            if group == "pwilkin" and not include_pwilkin:
                continue
            if recipe == "vllm":
                for fam in VLLM_FAMILIES:
                    pins[(repo, f"{tag}-{fam}")] = group
            else:
                pins[(repo, tag)] = group
    if chosen["lemonade"]:
        pins[(LEMONADE_REPO, lemonade_tag)] = "small"

    items: list[Item] = []
    for (repo, tag), group in sorted(pins.items()):
        if only_group and group != only_group:
            continue                            # skip the GitHub API call for other groups
        rel = github_release(repo, tag)
        real_tag = rel["tag_name"]
        item = Item(group=group, folder=f"github/{repo}/{real_tag}",
                    source=f"https://github.com/{repo}/releases/tag/{real_tag}")
        for a in rel.get("assets", []):
            digest = (a.get("digest") or "")
            item.files.append({"name": a["name"], "size": a["size"],
                               "sha256": digest.split(":", 1)[1] if digest.startswith("sha256:") else "",
                               "url": a["browser_download_url"]})
        if not item.files:
            log(f"  warning: {repo}@{real_tag} has no files")
        items.append(item)

    if chosen["therock"] and only_group in (None, "therock"):
        ver = versions["therock"]["version"]
        item = Item(group="therock", folder=f"therock/{ver}", source=THEROCK_URL)
        for os_name in THEROCK_OS:
            for fam in THEROCK_FAMILIES:
                name = f"therock-dist-{os_name}-{fam}-{ver}.tar.gz"
                size = head_size(THEROCK_URL + name)
                if size is None:
                    log(f"  skip (not published): {name}")
                    continue
                item.files.append({"name": name, "size": size, "sha256": "", "url": THEROCK_URL + name})
        items.append(item)
    return items


def download(url: str, dest: Path, expect_sha: str, expect_size: int) -> str:
    h = hashlib.sha256()
    req = urllib.request.Request(url, headers={"User-Agent": "lemonade-modelscope-mirror"})
    got = 0
    with urllib.request.urlopen(req, timeout=120) as r, open(dest, "wb") as f:
        while chunk := r.read(8 << 20):
            f.write(chunk)
            h.update(chunk)
            got += len(chunk)
    if got != expect_size:
        raise RuntimeError(f"size mismatch for {url}: got {got}, expected {expect_size}")
    sha = h.hexdigest()
    if expect_sha and sha != expect_sha:
        raise RuntimeError(f"SHA-256 mismatch for {url}: got {sha}, GitHub says {expect_sha}")
    return sha


REPO_TYPE = "dataset"   # files live in a ModelScope Datasets repo: modelscope.cn/datasets/<owner>/<name>


def file_url(repo: str, path: str, endpoint: str = "https://modelscope.cn") -> str:
    """Direct download URL for one file in the dataset repo (what Lemonade will call)."""
    from urllib.parse import quote
    return f"{endpoint}/api/v1/datasets/{repo}/repo?Revision=master&FilePath={quote(path)}"


def auth_get(url: str, token: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "lemonade-modelscope-mirror",
                                               "Authorization": f"Bearer {token}",
                                               "Cookie": f"m_session_id={token}"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=DEFAULT_REPO, help="ModelScope dataset repo, owner/name")
    ap.add_argument("--set", choices=sorted(SETS), default="trial")
    ap.add_argument("--group", help="only this group, for example vllm or therock (used by the workflow matrix)")
    ap.add_argument("--include-pwilkin", action="store_true", help="also mirror the pwilkin/* backends")
    ap.add_argument("--versions-file", help="local backend_versions.json instead of fetching it")
    ap.add_argument("--lemonade-ref", default="main", help="git ref to read backend_versions.json from")
    ap.add_argument("--lemonade-release", default="latest", help="Lemonade release whose installers to mirror")
    ap.add_argument("--endpoint", default=os.environ.get("MODELSCOPE_ENDPOINT", "https://modelscope.cn"),
                    help="https://modelscope.cn (mainland China, the real mirror) or https://www.modelscope.ai")
    ap.add_argument("--private", action="store_true", help="create the repo as private")
    ap.add_argument("--readme", help="upload this file as the repo's README.md (dataset card)")
    ap.add_argument("--dry-run", action="store_true", help="list what would be uploaded, change nothing")
    ap.add_argument("--workdir", default=None, help="where to put each file while it uploads")
    args = ap.parse_args()

    versions = load_versions(args.versions_file, args.lemonade_ref)
    items = build_plan(versions, args.set, args.include_pwilkin, args.lemonade_release, args.group)
    if args.group:
        items = [i for i in items if i.group == args.group]
        if not items:
            log(f"Nothing in group '{args.group}' for set '{args.set}'.")
            return 0

    existing: dict[str, int] = {}
    api = None
    token = os.environ.get("MODELSCOPE_API_TOKEN", "")
    if not args.dry_run:
        if not token:
            sys.exit("Set MODELSCOPE_API_TOKEN, or use --dry-run.")
        from modelscope_hub.api import HubApi
        api = HubApi(token=token, endpoint=args.endpoint)
        if not api.repo_exists(args.repo, REPO_TYPE):
            api.create_repo(args.repo, REPO_TYPE, visibility="private" if args.private else "public",
                            description="Official mirror of the backend binaries Lemonade downloads")
            log(f"Created dataset repo {args.repo}")
        existing = {f.path: f.size for f in api.list_repo_files(args.repo, REPO_TYPE, recursive=True)
                    if getattr(f, "type", "blob") != "tree"}
        log(f"{args.repo}: {len(existing)} files already on ModelScope")
        if args.readme:
            api.upload_file(args.repo, REPO_TYPE, args.readme, "README.md",
                            commit_message="Update README.md", disable_tqdm=True)
            log("README.md uploaded")

    total = sum(f["size"] for i in items for f in i.files)
    todo = sum(f["size"] for i in items for f in i.files if existing.get(f"{i.folder}/{f['name']}") != f["size"])
    log(f"\nPlan: {sum(len(i.files) for i in items)} files, {human(total)} pinned, {human(todo)} to upload\n")

    workdir = Path(args.workdir or tempfile.mkdtemp(prefix="msmirror-"))
    workdir.mkdir(parents=True, exist_ok=True)
    uploaded = skipped = 0
    for item in items:
        size = sum(f["size"] for f in item.files)
        log(f"[{item.group}] {item.folder}/  ({len(item.files)} files, {human(size)})")
        changed = False
        for f in item.files:
            path = f"{item.folder}/{f['name']}"
            if existing.get(path) == f["size"]:
                skipped += 1
                continue
            if args.dry_run:
                log(f"    would upload  {f['name']}  {human(f['size'])}")
                continue
            local = workdir / f["name"]
            t0 = time.time()
            f["sha256"] = download(f["url"], local, f["sha256"], f["size"])
            api.upload_file(args.repo, REPO_TYPE, str(local), path,
                            commit_message=f"Mirror {path}", disable_tqdm=True)
            local.unlink(missing_ok=True)
            uploaded += 1
            changed = True
            log(f"    uploaded  {f['name']}  {human(f['size'])}  in {time.time() - t0:.0f}s")
        manifest_path = f"{item.folder}/assets.json"
        if not args.dry_run and (changed or manifest_path not in existing):
            if any(not f["sha256"] for f in item.files) and manifest_path in existing:
                # Files skipped this run with no published SHA-256 (TheRock): keep the
                # value computed when they were first uploaded.
                old = json.loads(auth_get(file_url(args.repo, manifest_path, args.endpoint), token))
                known = {o["name"]: o["sha256"] for o in old.get("files", [])}
                for f in item.files:
                    f["sha256"] = f["sha256"] or known.get(f["name"], "")
            manifest = {"source": item.source,
                        "mirrored_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                        "files": [{k: f[k] for k in ("name", "size", "sha256")} for f in item.files]}
            api.upload_file(args.repo, REPO_TYPE, json.dumps(manifest, indent=2).encode(), manifest_path,
                            commit_message=f"Update {manifest_path}", disable_tqdm=True)

    log(f"\nDone. uploaded={uploaded} skipped={skipped} dry_run={args.dry_run}")
    first = next((i for i in items if i.files), None)
    if first:
        log("Download URL for one file:")
        log("  " + file_url(args.repo, f"{first.folder}/{first.files[0]['name']}", args.endpoint))
    return 0


if __name__ == "__main__":
    sys.exit(main())
