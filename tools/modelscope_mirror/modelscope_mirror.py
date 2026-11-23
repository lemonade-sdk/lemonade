#!/usr/bin/env python3
"""Mirror the backend files pinned in backend_versions.json to a ModelScope dataset,
and keep the SHA-256 checksums pinned there in sync.

Dataset layout, with GitHub's file names:
    github/<owner>/<repo>/<tag>/<file>   GitHub release files
    therock/<version>/<file>             ROCm runtime tarballs
    <each folder>/assets.json            name, size and SHA-256 of its files

Examples:
    python modelscope_mirror.py --set trial --dry-run
    python modelscope_mirror.py --set core --group vllm
    python modelscope_mirror.py --set core --lemonade-ref main --lemonade-ref release:latest --verify
    python modelscope_mirror.py --write-checksums
    python modelscope_mirror.py --check-checksums

Environment:
    MODELSCOPE_API_TOKEN   ModelScope access token (uploads only)
    GITHUB_TOKEN/GH_TOKEN  optional, raises the GitHub API rate limit from 60 to 5000 per hour
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
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
from urllib.parse import quote, urlparse

try:
    # Networks that inspect TLS need the OS certificate store.
    import truststore

    truststore.inject_into_ssl()
except ImportError:
    pass


class NoCredentialsAcrossHosts(urllib.request.HTTPRedirectHandler):
    """urllib keeps every header on a redirect; drop credentials when the host changes."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if (
            new is not None
            and urlparse(newurl).hostname != urlparse(req.full_url).hostname
        ):
            for header in ("Authorization", "Cookie"):
                new.remove_header(header)
        return new


urllib.request.install_opener(urllib.request.build_opener(NoCredentialsAcrossHosts))

DEFAULT_REPO = "lemonade-sdk/backends"
LEMONADE_REPO = "lemonade-sdk/lemonade"
VERSIONS_PATH = "src/cpp/resources/backend_versions.json"
THEROCK_URL = "https://stable.repo.amd.com/rocm/core/tarball/"
USER_AGENT = "lemonade-modelscope-mirror"

# Client GPUs only; datacenter targets are not mirrored.
VLLM_FAMILIES = ["gfx110X", "gfx120X", "gfx1150", "gfx1151"]
THEROCK_DATACENTER = {"gfx908", "gfx90a", "gfx94X-dcgpu", "gfx950-dcgpu"}
THEROCK_OS = ["windows", "linux"]

# recipe -> {variant or "*": (repo, group)}, copied from each backend's
# get_install_params(). An unknown recipe stops the script so this gets updated.
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
    # Only with --include-pwilkin.
    "thinksound": {"*": ("pwilkin/thinksound.cpp", "pwilkin")},
    "acestep": {"*": ("pwilkin/acestep.cpp", "pwilkin")},
    "trellis": {"*": ("pwilkin/trellis.cpp", "pwilkin")},
    "openmoss": {"*": ("pwilkin/openmoss", "pwilkin")},
}
NOT_BACKENDS = {
    "comment",
    "checksums",
    "therock",
    "rocm_asset_families",
    "clear_bin_if_lemonade_below",
}

SETS = {
    # Three small backends, about 0.5 GB.
    "trial": {
        "recipes": {"flm", "onnxruntime", "kokoro"},
        "therock": False,
        "lemonade": False,
    },
    "core": {"recipes": None, "therock": True, "lemonade": True},
}

RETRIES = 5


@dataclass
class Item:
    group: str
    folder: str  # folder inside the ModelScope repo
    source: str  # human-readable source, written to assets.json
    files: list[dict] = field(default_factory=list)  # name, size, sha256, url


def log(msg: str) -> None:
    print(msg, flush=True)


def human(n: int) -> str:
    for unit, div in (("GB", 1e9), ("MB", 1e6), ("KB", 1e3)):
        if n >= div:
            return f"{n / div:.2f} {unit}"
    return f"{n} B"


def transient(e: Exception) -> bool:
    if isinstance(e, urllib.error.HTTPError):
        return e.code == 429 or e.code >= 500
    return isinstance(e, (urllib.error.URLError, OSError, http.client.HTTPException))


def with_retries(what: str, fn, any_error: bool = False):
    """any_error: retry every exception (the ModelScope SDK documents none)."""
    for attempt in range(1, RETRIES + 1):
        try:
            return fn()
        except Exception as e:  # pylint: disable=broad-except
            if attempt == RETRIES or not (any_error or transient(e)):
                raise
            wait = min(120, 5 * 2**attempt)
            log(f"    {what} failed ({e}); retry {attempt}/{RETRIES - 1} in {wait}s")
            time.sleep(wait)
    raise AssertionError("unreachable")


def http_get(url: str, accept: str | None = None) -> bytes:
    headers = {"User-Agent": USER_AGENT}
    if accept:
        headers["Accept"] = accept
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if urlparse(url).hostname == "api.github.com" and token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(url, headers=headers)

    def get() -> bytes:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.read()

    return with_retries(f"GET {url}", get)


def head_size(url: str) -> int | None:
    req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": USER_AGENT})

    def head() -> int | None:
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return int(r.headers.get("Content-Length", 0)) or None
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            raise

    return with_retries(f"HEAD {url}", head)


def github_release(repo: str, tag: str) -> dict:
    # "newest" includes release candidates; "latest" does not.
    if tag == "newest":
        url = f"https://api.github.com/repos/{repo}/releases?per_page=1"
    elif tag == "latest":
        url = f"https://api.github.com/repos/{repo}/releases/latest"
    else:
        url = f"https://api.github.com/repos/{repo}/releases/tags/{quote(tag)}"
    try:
        body = json.loads(http_get(url, "application/vnd.github+json"))
    except urllib.error.HTTPError as e:
        if e.code == 403:
            sys.exit("GitHub API rate limit hit. Set GITHUB_TOKEN and run again.")
        raise RuntimeError(f"{repo}@{tag}: GitHub returned HTTP {e.code}") from e
    if tag == "newest":
        if not body:
            raise RuntimeError(f"{repo} has no releases")
        return body[0]
    return body


def load_versions(path: str | None, ref: str | None) -> dict:
    if path:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    if ref.startswith("release:"):
        ref = github_release(LEMONADE_REPO, ref.split(":", 1)[1])["tag_name"]
    url = f"https://raw.githubusercontent.com/{LEMONADE_REPO}/{quote(ref)}/{VERSIONS_PATH}"
    log(f"Reading pins from {url}")
    return json.loads(http_get(url))


def is_container_recipe(variants) -> bool:
    """Image pins (<tag>@sha256:<digest>) belong in a registry, not this mirror."""
    pins = [t for v, t in variants.items() if v != "comment"]
    return bool(pins) and all(isinstance(t, str) and "@sha256:" in t for t in pins)


def pinned_releases(
    versions: dict, recipes: set[str] | None, include_pwilkin: bool
) -> dict[tuple[str, str], str]:
    """(repo, tag) -> mirror group for each pinned GitHub release."""
    pins: dict[tuple[str, str], str] = {}
    for recipe, variants in versions.items():
        if (
            recipe in NOT_BACKENDS
            or not isinstance(variants, dict)
            or is_container_recipe(variants)
        ):
            continue
        if recipe not in SOURCES:
            sys.exit(
                f"backend_versions.json has a backend this script does not know: '{recipe}'. "
                f"Add it to SOURCES (see its get_install_params() in Lemonade)."
            )
        if recipes is not None and recipe not in recipes:
            continue
        for variant, tag in variants.items():
            if variant == "comment" or not isinstance(tag, str):
                continue  # comments and vllm.rocm_arch_overrides (datacenter)
            if "@sha256:" in tag:
                continue  # container image pin inside a native recipe
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
    return pins


def release_item(repo: str, tag: str, group: str) -> Item:
    rel = github_release(repo, tag)
    real_tag = rel["tag_name"]
    item = Item(
        group=group,
        folder=f"github/{repo}/{real_tag}",
        source=f"https://github.com/{repo}/releases/tag/{real_tag}",
    )
    for a in rel.get("assets", []):
        digest = a.get("digest") or ""
        item.files.append(
            {
                "name": a["name"],
                "size": a["size"],
                "sha256": (
                    digest.split(":", 1)[1] if digest.startswith("sha256:") else ""
                ),
                "url": a["browser_download_url"],
            }
        )
    if not item.files:
        log(f"  warning: {repo}@{real_tag} has no files")
    return item


def therock_item(versions: dict) -> Item:
    """Client TheRock tarballs published for the pinned version."""
    ver = versions["therock"]["version"]
    item = Item(group="therock", folder=f"therock/{ver}", source=THEROCK_URL)
    # Same arch -> tarball name mapping Lemonade uses in install_therock().
    mapping = versions["therock"].get("url_mapping", {})
    families = sorted(
        {mapping.get(a, a) for a in versions["therock"].get("architectures", [])}
        | set(mapping.values())
    )
    for os_name in THEROCK_OS:
        for fam in families:
            if fam in THEROCK_DATACENTER:
                continue
            name = f"therock-dist-{os_name}-{fam}-{ver}.tar.gz"
            size = head_size(THEROCK_URL + name)
            if size is None:
                log(f"  skip (not published): {name}")
                continue
            item.files.append(
                {"name": name, "size": size, "sha256": "", "url": THEROCK_URL + name}
            )
    return item


def build_plan(
    all_versions: list[dict],
    set_name: str,
    include_pwilkin: bool,
    lemonade_tag: str,
    only_group: str | None = None,
) -> list[Item]:
    chosen = SETS[set_name]
    pins: dict[tuple[str, str], str] = {}
    for versions in all_versions:
        pins.update(pinned_releases(versions, chosen["recipes"], include_pwilkin))
    if chosen["lemonade"]:
        pins[(LEMONADE_REPO, lemonade_tag)] = "small"

    items: list[Item] = []
    for (repo, tag), group in sorted(pins.items()):
        if only_group and group != only_group:
            continue  # skip the GitHub API call for other groups
        items.append(release_item(repo, tag, group))

    if chosen["therock"] and only_group in (None, "therock"):
        seen = set()
        for versions in all_versions:
            if versions["therock"]["version"] not in seen:
                seen.add(versions["therock"]["version"])
                items.append(therock_item(versions))

    for item in items:
        for f in item.files:
            pinned = pinned_sha256(all_versions, item.folder, f["name"])
            if pinned and f["sha256"] and pinned != f["sha256"]:
                sys.exit(
                    f"{item.folder}/{f['name']}: GitHub now publishes SHA-256 {f['sha256']}, "
                    f"but backend_versions.json pins {pinned}. The upstream file changed "
                    "after it was pinned; find out why before mirroring it."
                )
            f["sha256"] = pinned or f["sha256"]
    return items


def pinned_sha256(all_versions: list[dict], folder: str, name: str) -> str:
    """The pinned SHA-256 for one mirrored file, or ""."""
    kind, _, rest = folder.partition("/")
    found = set()
    for versions in all_versions:
        section = versions.get("checksums", {}).get(kind, {})
        if kind == "github":
            repo, _, tag = rest.rpartition("/")
            value = section.get(repo, {}).get(tag, {}).get(name, "")
        else:
            value = section.get(rest, {}).get(name, "")
        if value:
            found.add(value.removeprefix("sha256:"))
    if len(found) > 1:
        sys.exit(f"{folder}/{name}: the pins disagree on its SHA-256: {sorted(found)}")
    return found.pop() if found else ""


def download(url: str, dest: Path, expect_sha: str, expect_size: int | None) -> str:
    """Download with resume, then check size and SHA-256."""
    dest.unlink(missing_ok=True)

    def fetch() -> None:
        have = dest.stat().st_size if dest.exists() else 0
        headers = {"User-Agent": USER_AGENT}
        if have:
            headers["Range"] = f"bytes={have}-"
        req = urllib.request.Request(url, headers=headers)
        with urllib.request.urlopen(req, timeout=120) as r:
            mode = "ab" if have and r.status == 206 else "wb"
            with open(dest, mode) as f:
                while chunk := r.read(8 << 20):
                    f.write(chunk)
        # A dropped connection ends read() early without raising.
        got = dest.stat().st_size
        if expect_size is not None and got < expect_size:
            raise ConnectionError(f"connection closed at {got} of {expect_size} bytes")

    with_retries(f"download {url}", fetch)
    got = dest.stat().st_size
    if expect_size is not None and got != expect_size:
        raise RuntimeError(
            f"size mismatch for {url}: got {got}, expected {expect_size}"
        )
    h = hashlib.sha256()
    with open(dest, "rb") as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
    sha = h.hexdigest()
    if expect_sha and sha != expect_sha:
        raise RuntimeError(
            f"SHA-256 mismatch for {url}: got {sha}, expected {expect_sha}"
        )
    return sha


def checksums_for(
    versions: dict, refresh_github: bool, hash_therock: bool, workdir: Path
) -> dict:
    """The checksums section for what versions pins.

    Pinned hashes win over GitHub's unless refresh_github, so a replaced upstream
    file shows as drift. AMD publishes no checksums: TheRock is hashed by
    download, only for a newly pinned version and only with hash_therock.
    """
    old = versions.get("checksums", {})
    github: dict[str, dict[str, dict[str, str]]] = {}
    for repo, tag in sorted(pinned_releases(versions, None, True)):
        item = release_item(repo, tag, "")
        unpublished = [f["name"] for f in item.files if not f["sha256"]]
        if unpublished:
            sys.exit(f"GitHub publishes no SHA-256 for {repo}@{tag}: {unpublished}")
        entries = {f["name"]: f"sha256:{f['sha256']}" for f in item.files}
        if not refresh_github:
            entries.update(old.get("github", {}).get(repo, {}).get(tag, {}))
        github.setdefault(repo, {})[tag] = dict(sorted(entries.items()))

    ver = versions["therock"]["version"]
    old_therock = old.get("therock", {}).get(ver, {})
    result = {}
    if "comment" in old:
        result["comment"] = old["comment"]
    result["github"] = github

    item = therock_item(versions)
    therock: dict[str, str] = dict(old_therock)  # keep hashes AMD no longer lists
    for f in item.files:
        if f["name"] in therock:
            continue  # already hashed
        if not hash_therock:
            therock[f["name"]] = "(not hashed yet)"
            continue
        log(f"  hashing {f['name']} ({human(f['size'])}) from AMD")
        local = workdir / f["name"]
        therock[f["name"]] = "sha256:" + download(f["url"], local, "", f["size"])
        local.unlink(missing_ok=True)
    result["therock"] = {ver: therock}
    return result


def describe_drift(
    want: dict, have: dict, prefix: str = "checksums"
) -> list[tuple[bool, str]]:
    """(fatal, line) per difference. Fatal: a changed hash, or a pinned release
    without checksums. A single asset added or removed upstream only warns."""
    lines = []
    for key in sorted(set(want) | set(have)):
        path = f"{prefix}.{key}"
        leaf = not isinstance(want.get(key, have.get(key)), dict)
        if key not in have:
            lines.append((not leaf, f"  missing  {path}"))
        elif key not in want:
            lines.append((False, f"  stale    {path}"))
        elif not leaf and isinstance(have[key], dict):
            lines += describe_drift(want[key], have[key], path)
        elif want[key] != have[key]:
            lines.append(
                (True, f"  differs  {path}: pinned {have[key]}, upstream {want[key]}")
            )
    return lines


def update_checksums(path: Path, check: bool, refresh: bool, workdir: Path) -> int:
    versions = json.loads(path.read_text(encoding="utf-8"))
    want = checksums_for(versions, refresh or check, not check, workdir)
    have = versions.get("checksums", {})
    drift = describe_drift(
        {k: v for k, v in want.items() if k != "comment"},
        {k: v for k, v in have.items() if k != "comment"},
    )
    if check:
        fatal = [line for is_fatal, line in drift if is_fatal]
        notes = [line for is_fatal, line in drift if not is_fatal]
        if notes:
            log(
                f"{path}: upstream changed after pinning (not an error):\n"
                + "\n".join(notes)
            )
        if fatal:
            log(f"{path}: checksums do not match the pins:\n" + "\n".join(fatal))
            log(
                "\nFor a missing entry, run python tools/modelscope_mirror/modelscope_mirror.py "
                "--write-checksums and commit the result. A differing hash means upstream "
                "replaced a pinned file; find out why before pinning the new hash."
            )
            return 1
        log(f"{path}: checksums match every pinned file.")
        return 0
    versions["checksums"] = want
    path.write_text(
        json.dumps(versions, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    log(f"{path}: {len(drift)} checksum change(s) written.")
    return 0


REPO_TYPE = "dataset"  # files live in a ModelScope Datasets repo: modelscope.cn/datasets/<owner>/<name>


def file_url(repo: str, path: str, endpoint: str = "https://modelscope.cn") -> str:
    """The URL lemond downloads one dataset file from."""
    return (
        f"{endpoint}/api/v1/datasets/{repo}/repo?Revision=master&FilePath={quote(path)}"
    )


def manifest_files(item: Item) -> list[dict]:
    return [{k: f[k] for k in ("name", "size", "sha256")} for f in item.files]


def read_manifest(repo: str, folder: str, endpoint: str) -> list[dict] | None:
    """Files listed in a folder's assets.json, or None. Public, so no token."""
    try:
        body = http_get(file_url(repo, f"{folder}/assets.json", endpoint))
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise
    return json.loads(body).get("files", [])


def same_files(a: list[dict] | None, b: list[dict]) -> bool:
    return a is not None and sorted(a, key=lambda f: f["name"]) == sorted(
        b, key=lambda f: f["name"]
    )


def manifest_is_current(item: Item, files: list[dict] | None) -> bool:
    if files is None:
        return False
    listed = {f["name"]: f for f in files}
    return len(listed) == len(item.files) and all(
        f["name"] in listed
        and listed[f["name"]]["size"] == f["size"]
        and (not f["sha256"] or listed[f["name"]]["sha256"] == f["sha256"])
        for f in item.files
    )


def list_existing(api, repo: str) -> dict[str, int]:
    return {
        f.path: f.size
        for f in with_retries(
            "list files",
            lambda: list(api.list_repo_files(repo, REPO_TYPE, recursive=True)),
            any_error=True,
        )
        if getattr(f, "type", "blob") != "tree"
    }


def missing_from(
    items: list[Item], existing: dict[str, int], read_manifests=None
) -> list[str]:
    """Planned files missing from ModelScope; with read_manifests, stale assets.json too."""
    missing = []
    for item in items:
        for f in item.files:
            path = f"{item.folder}/{f['name']}"
            if existing.get(path) != f["size"]:
                missing.append(f"{path} ({human(f['size'])})")
        if not item.files:
            continue
        manifest_path = f"{item.folder}/assets.json"
        if manifest_path not in existing:
            missing.append(manifest_path)
        elif read_manifests and not manifest_is_current(
            item, read_manifests(item.folder)
        ):
            missing.append(f"{manifest_path} (out of date)")
    return missing


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--repo", default=DEFAULT_REPO, help="ModelScope dataset repo, owner/name"
    )
    ap.add_argument("--set", choices=sorted(SETS), default="trial")
    ap.add_argument(
        "--group",
        help="only this group, for example vllm or therock (used by the workflow matrix)",
    )
    ap.add_argument(
        "--include-pwilkin",
        action="store_true",
        help="also mirror the pwilkin/* backends",
    )
    ap.add_argument(
        "--versions-file",
        action="append",
        default=[],
        help="a local backend_versions.json; repeatable, the pins of all are mirrored",
    )
    ap.add_argument(
        "--lemonade-ref",
        action="append",
        default=[],
        help="git ref or commit to read backend_versions.json from; repeatable. "
        "release:latest and release:newest name the newest stable release and the "
        "newest release including candidates. Default: main",
    )
    ap.add_argument(
        "--lemonade-release",
        default="latest",
        help="Lemonade release whose installers to mirror: a tag, 'latest' (newest "
        "stable) or 'newest' (also counts release candidates)",
    )
    ap.add_argument(
        "--endpoint",
        default=os.environ.get("MODELSCOPE_ENDPOINT", "https://modelscope.cn"),
        help="https://modelscope.cn (mainland China, the real mirror) or https://www.modelscope.ai",
    )
    ap.add_argument("--private", action="store_true", help="create the repo as private")
    ap.add_argument(
        "--readme", help="upload this file as the repo's README.md (dataset card)"
    )
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="list what would be uploaded, change nothing",
    )
    ap.add_argument(
        "--verify",
        action="store_true",
        help="upload nothing; fail if any planned file is missing from ModelScope",
    )
    ap.add_argument(
        "--write-checksums",
        action="store_true",
        help="rewrite the checksums section of --versions-file to match its pins",
    )
    ap.add_argument(
        "--check-checksums",
        action="store_true",
        help="fail if the checksums section of --versions-file does not match its pins "
        "and the digests GitHub publishes",
    )
    ap.add_argument(
        "--refresh",
        action="store_true",
        help="with --write-checksums: look up every GitHub entry again, not only new pins",
    )
    ap.add_argument(
        "--workdir", default=None, help="where to put each file while it uploads"
    )
    args = ap.parse_args()

    workdir = Path(args.workdir or tempfile.mkdtemp(prefix="msmirror-"))
    workdir.mkdir(parents=True, exist_ok=True)

    if args.write_checksums or args.check_checksums:
        if len(args.versions_file) > 1 or args.lemonade_ref:
            sys.exit(
                "--write-checksums and --check-checksums take one --versions-file."
            )
        path = Path(args.versions_file[0] if args.versions_file else VERSIONS_PATH)
        return update_checksums(path, args.check_checksums, args.refresh, workdir)

    all_versions = [load_versions(p, None) for p in args.versions_file]
    all_versions += [load_versions(None, r) for r in args.lemonade_ref]
    if not all_versions:
        all_versions = [load_versions(None, "main")]
    items = build_plan(
        all_versions, args.set, args.include_pwilkin, args.lemonade_release, args.group
    )
    if args.group:
        items = [i for i in items if i.group == args.group]
        if not items:
            log(f"Nothing in group '{args.group}' for set '{args.set}'.")
            return 0

    existing: dict[str, int] = {}
    api = None
    token = os.environ.get("MODELSCOPE_API_TOKEN", "")
    if not args.dry_run:
        from modelscope_hub.api import HubApi

        if args.verify:
            api = HubApi(token=token or None, endpoint=args.endpoint)
            missing = missing_from(
                items,
                list_existing(api, args.repo),
                lambda folder: read_manifest(args.repo, folder, args.endpoint),
            )
            if missing:
                log(f"{len(missing)} pinned file(s) missing from {args.repo}:")
                for m in missing:
                    log(f"  {m}")
                return 1
            log(
                f"All {sum(len(i.files) for i in items)} pinned files are on {args.repo}."
            )
            return 0
        if not token:
            sys.exit("Set MODELSCOPE_API_TOKEN, or use --dry-run.")
        api = HubApi(token=token, endpoint=args.endpoint)
        if not api.repo_exists(args.repo, REPO_TYPE):
            api.create_repo(
                args.repo,
                REPO_TYPE,
                visibility="private" if args.private else "public",
                description="Official mirror of the backend binaries Lemonade downloads",
            )
            log(f"Created dataset repo {args.repo}")
        existing = list_existing(api, args.repo)
        log(f"{args.repo}: {len(existing)} files already on ModelScope")
        if args.readme:
            api.upload_file(
                args.repo,
                REPO_TYPE,
                args.readme,
                "README.md",
                commit_message="Update README.md",
                disable_tqdm=True,
            )
            log("README.md uploaded")

    total = sum(f["size"] for i in items for f in i.files)
    todo = sum(
        f["size"]
        for i in items
        for f in i.files
        if existing.get(f"{i.folder}/{f['name']}") != f["size"]
    )
    log(
        f"\nPlan: {sum(len(i.files) for i in items)} files, {human(total)} pinned, {human(todo)} to upload\n"
    )

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
            with_retries(
                f"upload {path}",
                lambda: api.upload_file(
                    args.repo,
                    REPO_TYPE,
                    str(local),
                    path,
                    commit_message=f"Mirror {path}",
                    disable_tqdm=True,
                ),
                any_error=True,
            )
            local.unlink(missing_ok=True)
            uploaded += 1
            changed = True
            log(
                f"    uploaded  {f['name']}  {human(f['size'])}  in {time.time() - t0:.0f}s"
            )
        manifest_path = f"{item.folder}/assets.json"
        if args.dry_run or not item.files:
            continue
        # Checked every run: an interrupted run can leave it stale.
        old = (
            read_manifest(args.repo, item.folder, args.endpoint)
            if manifest_path in existing
            else None
        )
        if old:
            # Files with no known SHA-256 keep the one from their first upload.
            known = {o["name"]: o.get("sha256", "") for o in old}
            for f in item.files:
                f["sha256"] = f["sha256"] or known.get(f["name"], "")
        if not changed and same_files(old, manifest_files(item)):
            continue
        manifest = {
            "source": item.source,
            "mirrored_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "files": manifest_files(item),
        }
        with_retries(
            f"upload {manifest_path}",
            lambda: api.upload_file(
                args.repo,
                REPO_TYPE,
                json.dumps(manifest, indent=2).encode(),
                manifest_path,
                commit_message=f"Update {manifest_path}",
                disable_tqdm=True,
            ),
            any_error=True,
        )
        log("    updated  assets.json")

    log(f"\nDone. uploaded={uploaded} skipped={skipped} dry_run={args.dry_run}")
    if not args.dry_run:
        missing = missing_from(items, list_existing(api, args.repo))
        if missing:
            log(f"Still missing after this run ({len(missing)}):")
            for m in missing:
                log(f"  {m}")
            return 1
        log("Verified: every planned file is on ModelScope.")
    first = next((i for i in items if i.files), None)
    if first:
        log("Download URL for one file:")
        log(
            "  "
            + file_url(
                args.repo, f"{first.folder}/{first.files[0]['name']}", args.endpoint
            )
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
