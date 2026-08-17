#!/usr/bin/env python3
"""
Lemonade nightly backend perf regression runner.

Uses `lemonade bench` against each tracked fork's prebuilt binary.
The fork binary is downloaded, then routed into lemond via
`lemonade config set llamacpp.<backend>_bin <dir>` (done by the workflow
before lemond starts). lemond then serves --backend <backend> from that
binary instead of downloading its own. There is no LEMONADE_*_BIN env var.

Usage:
  python .github/scripts/validate_backend_bench.py --forks src/cpp/resources/benchmark_forks.json --output ci/results
  python .github/scripts/validate_backend_bench.py --fork-filter llamacpp-rocm-nightly --model-filter Qwen3.5-4B-GGUF
  python .github/scripts/validate_backend_bench.py --fork-filter llamacpp-rocm-nightly --dry-run
"""

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from datetime import datetime, timezone
from pathlib import Path

IS_WINDOWS = platform.system() == "Windows"
BACKEND_VERSIONS_PATH = Path("src/cpp/resources/backend_versions.json")
# Long-context scenarios (context-32k/64k/128k) need the model loaded with a
# matching ctx window; bench loads once at 4096 by default, so a 32k prompt gets
# HTTP 400. Until per-scenario ctx is wired up, run only the short scenarios
# that fit the default window. See issue #2858.
SCENARIOS = ["chat-short", "code-short"]
WARMUP_RUNS = 1
MEASUREMENT_RUNS = 5
THRESHOLDS = {
    "tps_warn": -0.05,
    "tps_critical": -0.15,
    "ttft_warn": +0.10,
}


# ---------------------------------------------------------------------------
# Utilities
# ---------------------------------------------------------------------------


def get_therock_ver() -> str:
    """Read therock major.minor from backend_versions.json — mirrors llamacpp_server.cpp."""
    data = json.loads(BACKEND_VERSIONS_PATH.read_text())
    v = data["therock"]["version"].lstrip("v").split(".")
    return ".".join(v[:2])


def gh_api(path: str, token: str | None = None) -> dict:
    url = f"https://api.github.com/{path.lstrip('/')}"
    headers = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())


def resolve_latest_version(repo: str, token: str | None, tag_prefix: str = "") -> str:
    if tag_prefix:
        releases = gh_api(f"repos/{repo}/releases?per_page=10", token)
        for r in releases:
            if r["tag_name"].startswith(tag_prefix):
                return r["tag_name"]
        raise RuntimeError(f"No release with prefix {tag_prefix!r} in {repo}")
    return gh_api(f"repos/{repo}/releases/latest", token)["tag_name"]


def download_file(url: str, dest: Path, token: str | None = None) -> None:
    print(f"  Downloading {url}")
    headers = {}
    parsed = urllib.parse.urlparse(url)
    host = (parsed.hostname or "").lower()
    if token and (host == "github.com" or host.endswith(".github.com")):
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=300) as r, open(dest, "wb") as f:
            shutil.copyfileobj(r, f)
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"Download failed ({e.code}): {url}") from e


def _ensure_executable(root: Path) -> None:
    """chmod +x every regular file under root (Linux/macOS only).

    zipfile.extractall drops the Unix execute bit, so the extracted
    llama-server is not runnable and lemond's exec() fails with EACCES ->
    /load returns HTTP 500. lemond's own installer chmods 0755 after extract
    (backend_utils.cpp); mirror that here.
    """
    if IS_WINDOWS:
        return
    for p in root.rglob("*"):
        if p.is_file():
            p.chmod(p.stat().st_mode | 0o755)


def extract_archive(archive: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    if archive.suffix == ".zip" or archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            z.extractall(dest)
    elif ".tar" in archive.name:
        with tarfile.open(archive) as t:
            t.extractall(dest)
    else:
        raise RuntimeError(f"Unknown archive format: {archive}")

    _ensure_executable(dest)


def find_binary(install_dir: Path, exe_name: str) -> Path | None:
    for p in install_dir.rglob(exe_name):
        if p.is_file():
            return p
    return None


def find_lemonade_bin() -> str:
    # CI sets LEMONADE_EXE from the locate step
    if os.environ.get("LEMONADE_EXE"):
        return os.environ["LEMONADE_EXE"]
    # CI artifacts (build job path)
    for c in [
        Path("build/Release/lemonade.exe"),
        Path("build/lemonade"),
        Path("build/Release/lemonade"),
    ]:
        if c.exists():
            return str(c)
    # Local dev: lemonade_server install has the C++ CLI
    if IS_WINDOWS:
        local = (
            Path(os.environ.get("LOCALAPPDATA", ""))
            / "lemonade_server/bin/lemonade.exe"
        )
        if local.exists():
            return str(local)
    return "lemonade"


# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------


def get_models_from_registry(base_url: str) -> list[str]:
    """Mirror validate_llamacpp.py:get_hot_llamacpp_models() — reads live registry."""
    try:
        req = urllib.request.Request(f"{base_url}/api/v1/models?show_all=true")
        with urllib.request.urlopen(req, timeout=30) as r:
            data = json.loads(r.read())
        models = [
            m["id"]
            for m in data.get("data", [])
            if m.get("recipe") == "llamacpp" and "hot" in m.get("labels", [])
        ]
        return sorted(models)
    except Exception as e:
        print(f"  [WARN] Could not fetch models from registry: {e}")
        return []


# ---------------------------------------------------------------------------
# Binary acquisition
# ---------------------------------------------------------------------------


def resolve_binary_url(fork: dict, version: str) -> str:
    key = "binary_url_windows" if IS_WINDOWS else "binary_url_linux"
    url = fork.get(key, "")
    if not url:
        return ""
    arch = fork.get("arch")
    if not arch:
        raise RuntimeError(
            f"Fork {fork.get('fork_id')} is missing required 'arch' field in benchmark_forks.json"
        )
    therock_ver = ""
    if "{therock_ver}" in url and BACKEND_VERSIONS_PATH.exists():
        therock_ver = get_therock_ver()
    url = url.replace("{version}", version)
    url = url.replace("{arch}", arch)
    if therock_ver:
        url = url.replace("{therock_ver}", therock_ver)
    else:
        url = url.replace("-{therock_ver}", "").replace("{therock_ver}", "")
    return url


def install_fork_binary(
    fork: dict, version: str, binaries_dir: Path, token: str | None, dry_run: bool
) -> Path | None:
    fork_id = fork["fork_id"]
    exe_name = fork["binary_exe_windows"] if IS_WINDOWS else fork["binary_exe_linux"]
    if not exe_name:
        print(f"  [skip] No binary for this platform")
        return None

    # One slot per fork — overwrite on version change, no disk accumulation
    install_dir = binaries_dir / fork_id
    version_file = install_dir / "version.txt"
    installed_version = (
        version_file.read_text().strip() if version_file.exists() else None
    )

    if installed_version == version:
        binary_path = find_binary(install_dir, exe_name)
        if binary_path and binary_path.exists():
            print(f"  [cache] {version} already installed: {binary_path}")
            # Binaries cached by an earlier run may predate the chmod fix (or
            # were extracted without the exec bit), so lemond's exec() would
            # fail with EACCES -> /load 500. Re-ensure the bit on the cache hit.
            _ensure_executable(install_dir)
            return binary_path

    url = resolve_binary_url(fork, version)
    if not url:
        print(f"  [skip] No binary URL for {fork_id} on this platform")
        return None

    if dry_run:
        print(f"  [dry-run] Would download: {url}")
        return install_dir / exe_name

    if installed_version and installed_version != version:
        print(f"  Replacing {installed_version} → {version}")
        shutil.rmtree(install_dir, ignore_errors=True)

    staging_dir = binaries_dir / f"{fork_id}.staging"
    shutil.rmtree(staging_dir, ignore_errors=True)
    staging_dir.mkdir(parents=True, exist_ok=True)

    archive_path = staging_dir / url.split("/")[-1]
    download_file(url, archive_path, token)
    print(f"  Extracting to {staging_dir}")
    extract_archive(archive_path, staging_dir)
    archive_path.unlink()

    if install_dir.exists():
        shutil.rmtree(install_dir)
    staging_dir.rename(install_dir)
    version_file.write_text(version)

    binary_path = find_binary(install_dir, exe_name)
    if not binary_path:
        raise RuntimeError(f"Binary {exe_name!r} not found in {install_dir}")
    print(f"  Installed: {binary_path}")
    return binary_path


# ---------------------------------------------------------------------------
# Bench runner
# ---------------------------------------------------------------------------


def install_backend(base_url: str, recipe: str, backend: str) -> None:
    """Install backend via POST /install — mirrors validate_llamacpp.py:install_backend()."""
    print(f"  Installing backend {recipe}/{backend} via /install...")
    data = json.dumps({"recipe": recipe, "backend": backend, "stream": False}).encode()
    req = urllib.request.Request(
        f"{base_url}/api/v1/install",
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=1800) as r:
            body = json.loads(r.read())
            print(f"  Install response: {body.get('status', 'ok')}")
    except urllib.error.HTTPError as e:
        body = e.read().decode()
        raise RuntimeError(f"Backend install failed ({e.code}): {body}") from e


def run_bench(
    fork: dict,
    version: str,
    binary_path: Path,
    model: str,
    output_file: Path,
    compare_file: Path | None,
    dry_run: bool,
    lemonade_bin: str,
) -> dict | None:
    backend = fork["backend"]
    # bench_as: the real llamacpp backend key lemond recognises for --backend.
    # llamacpp exposes {system, cuda, vulkan, rocm, cpu} — "rocm-nightly" and
    # "rocm-gfx11" are NOT backend keys (nightly/stable are rocm *channels*), so
    # forks declare install_as to map to the real key. The fork binary itself is
    # routed via `lemonade config set llamacpp.<key>_bin <dir>` (done by the
    # workflow before lemond starts); there is no LEMONADE_*_BIN env var —
    # lemond never reads one.
    bench_as = fork.get("install_as", backend)

    cmd = [
        lemonade_bin,
        "bench",
        model,
        "--backend",
        bench_as,
        "--scenarios",
        *SCENARIOS,
        "--runs",
        str(MEASUREMENT_RUNS),
        "--warmup",
        str(WARMUP_RUNS),
        "--auto-pull",
        "--json",
        "--output",
        str(output_file),
    ]
    if compare_file and compare_file.exists():
        cmd += ["--compare", str(compare_file)]

    env = os.environ.copy()

    print(f"    cmd: {' '.join(cmd)}")
    print(f"    backend key: {bench_as}  (binary routed via llamacpp.{bench_as}_bin)")

    if dry_run:
        print("    [dry-run] skipping execution")
        return None

    output_file.parent.mkdir(parents=True, exist_ok=True)

    # Stream output live so CI logs show progress (model pulls, scenario runs)
    print("    [running — streaming output below]", flush=True)
    proc = subprocess.Popen(
        cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    output_lines = []
    for line in proc.stdout:
        print(f"    {line}", end="", flush=True)
        output_lines.append(line)
    proc.wait()

    if proc.returncode != 0:
        print(f"    [ERROR] lemonade bench failed (exit {proc.returncode})")
        return None

    if not output_file.exists():
        print(f"    [ERROR] output file not created: {output_file}")
        return None

    with open(output_file) as f:
        data = json.load(f)

    data.update(
        {
            "fork_id": fork["fork_id"],
            "fork_label": fork["label"],
            "fork_repo": fork["repo"],
            "fork_version": version,
            "fork_backend": backend,
        }
    )
    # CI provenance so the dashboard can link each number back to its run.
    # These env vars are set by GitHub Actions; absent in local runs (None).
    run_id = os.environ.get("GITHUB_RUN_ID")
    server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
    repo = os.environ.get("GITHUB_REPOSITORY", "")
    if run_id:
        data["run_id"] = run_id
        if repo:
            data["run_url"] = f"{server}/{repo}/actions/runs/{run_id}"
    if os.environ.get("GITHUB_SHA"):
        data["commit"] = os.environ["GITHUB_SHA"]
    if os.environ.get("LEMONADE_VERSION"):
        data["lemonade_version"] = os.environ["LEMONADE_VERSION"]
    with open(output_file, "w") as f:
        json.dump(data, f, indent=2)
    return data


# ---------------------------------------------------------------------------
# Regression detection
# ---------------------------------------------------------------------------


def find_previous_result(results_dir: Path, fork_id: str, model: str) -> Path | None:
    model_dir = results_dir / fork_id / model
    if not model_dir.exists():
        return None
    runs = sorted(
        [p for p in model_dir.glob("run-*.json") if "regression" not in p.name],
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    return runs[1] if len(runs) > 1 else None


def backend_results(data: dict, model: str | None = None) -> list[dict]:
    """Return the backend-results list from a bench output file.

    The bench writes {"models": [{"model": ..., "results": [...]}]}, one entry
    per model. Older/flat files put "results" at the top level. This flattens
    both; when `model` is given, only that model's results are returned.
    """
    entries = data.get("models")
    if isinstance(entries, list):
        out = []
        for me in entries:
            if model is None or me.get("model") == model:
                out.extend(me.get("results", []))
        return out
    return data.get("results", [])


def check_regression(
    current: dict, previous: dict, fork_id: str, model: str
) -> list[dict]:
    alerts = []
    curr_results = {r["backend"]: r for r in backend_results(current, model)}
    prev_results = {r["backend"]: r for r in backend_results(previous, model)}
    for backend, curr in curr_results.items():
        prev = prev_results.get(backend)
        if not prev:
            continue
        for curr_sc in curr.get("scenarios", []):
            sc_name = curr_sc["name"]
            prev_sc = next(
                (s for s in prev.get("scenarios", []) if s["name"] == sc_name), None
            )
            if not prev_sc:
                continue
            curr_tps = curr_sc.get("tps", {}).get("mean", 0)
            prev_tps = prev_sc.get("tps", {}).get("mean", 0)
            curr_ttft = curr_sc.get("ttft_ms", {}).get("mean", 0)
            prev_ttft = prev_sc.get("ttft_ms", {}).get("mean", 0)
            if prev_tps > 0:
                delta = (curr_tps - prev_tps) / prev_tps
                sev = (
                    "CRITICAL"
                    if delta <= THRESHOLDS["tps_critical"]
                    else "WARN" if delta <= THRESHOLDS["tps_warn"] else None
                )
                if sev:
                    alerts.append(
                        {
                            "severity": sev,
                            "fork_id": fork_id,
                            "model": model,
                            "scenario": sc_name,
                            "metric": "tps",
                            "delta_pct": round(delta * 100, 2),
                            "prev": round(prev_tps, 2),
                            "curr": round(curr_tps, 2),
                        }
                    )
            if prev_ttft > 0:
                delta = (curr_ttft - prev_ttft) / prev_ttft
                if delta >= THRESHOLDS["ttft_warn"]:
                    alerts.append(
                        {
                            "severity": "WARN",
                            "fork_id": fork_id,
                            "model": model,
                            "scenario": sc_name,
                            "metric": "ttft_ms",
                            "delta_pct": round(delta * 100, 2),
                            "prev": round(prev_ttft, 2),
                            "curr": round(curr_ttft, 2),
                        }
                    )
    return alerts


def update_leaderboard(
    leaderboard: dict, fork: dict, version: str, model: str, result: dict
) -> None:
    for br in backend_results(result, model):
        for sc in br.get("scenarios", []):
            tps_mean = sc.get("tps", {}).get("mean")
            if tps_mean is None:
                continue
            key = f"{model}|{sc['name']}"
            entry = {
                "model": model,
                "scenario": sc["name"],
                "fork_id": fork["fork_id"],
                "fork_label": fork["label"],
                "fork_repo": fork["repo"],
                "fork_version": version,
                "backend": br.get("backend"),
                "tps_mean": round(tps_mean, 2),
                "tps_p95": round(sc["tps"].get("p95", 0), 2),
                "ttft_ms_mean": round(sc.get("ttft_ms", {}).get("mean", 0), 2),
                "vram_peak_gb": sc.get("vram_peak_gb"),
                "timestamp": result.get("timestamp"),
            }
            leaderboard.setdefault(key, [])
            leaderboard[key] = [
                e for e in leaderboard[key] if e["fork_id"] != fork["fork_id"]
            ]
            leaderboard[key].append(entry)
            leaderboard[key].sort(key=lambda e: e["tps_mean"], reverse=True)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--forks", default="src/cpp/resources/benchmark_forks.json")
    parser.add_argument("--output", default="ci/results")
    parser.add_argument(
        "--binaries-dir",
        default=(
            "C:/lemonade-bench-binaries"
            if IS_WINDOWS
            else "/opt/lemonade-bench-binaries"
        ),
    )
    parser.add_argument("--fork-filter", default="")
    parser.add_argument("--model-filter", default="")
    parser.add_argument(
        "--port", default="", help="lemond port (informational; bench uses UDP beacon)"
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--download-only",
        action="store_true",
        help="Only download fork binaries, do not run bench (use before lemond starts)",
    )
    parser.add_argument("--token", default=os.environ.get("GH_TOKEN"))
    args = parser.parse_args()

    forks_path = Path(args.forks)
    output_dir = Path(args.output)
    binaries_dir = Path(args.binaries_dir)
    fork_filter = [f.strip() for f in args.fork_filter.split(",") if f.strip()]
    model_filter = [m.strip() for m in args.model_filter.split(",") if m.strip()]
    lemonade_bin = find_lemonade_bin()
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")

    print("=== Lemonade Perf Regression Runner ===")
    print(f"Forks    : {forks_path}")
    print(f"Output   : {output_dir}")
    print(f"Lemonade : {lemonade_bin}")
    print(f"Dry run  : {args.dry_run}")
    if args.port:
        print(f"Port     : {args.port} (informational)")
    print()

    with open(forks_path) as f:
        forks_config = json.load(f)

    forks = [
        fork
        for fork in forks_config["forks"]
        if fork.get("tracked", True)
        and (not fork_filter or fork["fork_id"] in fork_filter)
    ]

    print(f"Forks ({len(forks)}): {[f['fork_id'] for f in forks]}")

    # Determine model list — from --model-filter or later from registry (after lemond starts)
    models = model_filter
    if models:
        print(f"Models (from filter): {models}")

    print(f"Scenarios: {SCENARIOS}")
    print()

    leaderboard: dict = {}
    all_alerts: list = []
    run_summary: list = []

    # --download-only: just fetch binaries, exit before bench (used before lemond starts)
    if args.download_only:
        for fork in forks:
            fork_id = fork["fork_id"]
            print(f"Downloading binary for {fork_id}...")
            try:
                tag_prefix = fork.get("version_tag_prefix", "")
                version = resolve_latest_version(fork["repo"], args.token, tag_prefix)
                install_fork_binary(
                    fork, version, binaries_dir, args.token, args.dry_run
                )
            except Exception as e:
                print(f"  [ERROR] {e}")
                return 1
        print("All fork binaries ready.")
        return 0

    for fork in forks:
        fork_id = fork["fork_id"]
        runner_os = fork.get("runner_os", "windows" if IS_WINDOWS else "linux")
        current_os = "windows" if IS_WINDOWS else "linux"
        if runner_os != current_os:
            print(f"[skip] {fork_id} requires {runner_os}, running on {current_os}")
            continue

        print(f"{'='*60}")
        print(f"Fork: {fork_id}  ({fork['label']})")
        print(f"Repo: {fork['repo']}")

        try:
            tag_prefix = fork.get("version_tag_prefix", "")
            version = resolve_latest_version(fork["repo"], args.token, tag_prefix)
            print(f"Version: {version}")
        except Exception as e:
            print(f"  [ERROR] Could not resolve version: {e}")
            continue

        try:
            binary_path = install_fork_binary(
                fork, version, binaries_dir, args.token, args.dry_run
            )
        except Exception as e:
            print(f"  [ERROR] Binary install failed: {e}")
            continue

        if binary_path is None:
            continue

        # Install the fork backend before running any models
        base_url = "http://127.0.0.1:8000"
        if args.port:
            base_url = f"http://127.0.0.1:{args.port}"
        if not args.dry_run:
            # Skip POST /install when fork provides its own prebuilt binary.
            # The binary is already routed via llamacpp.<backend>_bin config —
            # it has ROCm baked in and doesn't need lemond to install anything.
            has_own_binary = bool(
                fork.get("binary_url_linux") or fork.get("binary_url_windows")
            )
            if not has_own_binary:
                try:
                    install_backend_name = fork.get("install_as", fork["backend"])
                    install_backend(base_url, fork["recipe"], install_backend_name)
                except Exception as e:
                    print(f"  [ERROR] Backend install failed: {e}")
                    continue
            else:
                print(
                    f"  Skipping POST /install — fork provides its own prebuilt binary"
                )

        # Fetch model list from registry now that lemond is confirmed running
        run_models = models
        if not run_models and not args.dry_run:
            run_models = get_models_from_registry(base_url)
            if run_models:
                print(f"  Models from registry ({len(run_models)}): {run_models}")
            else:
                print("  [ERROR] No models from registry and no --model-filter set")
                continue

        for model in run_models:
            print(
                f"\n  Model: {model} (--auto-pull enabled, first run may download weights)"
            )
            run_file = output_dir / fork_id / model / f"run-{timestamp}.json"
            prev_result = find_previous_result(output_dir, fork_id, model)

            result = run_bench(
                fork,
                version,
                binary_path,
                model,
                run_file,
                prev_result,
                args.dry_run,
                lemonade_bin,
            )

            if result is None:
                run_summary.append(
                    {"fork_id": fork_id, "model": model, "status": "FAILED"}
                )
                continue

            alerts = []
            if prev_result and prev_result.exists():
                with open(prev_result) as f:
                    previous = json.load(f)
                alerts = check_regression(result, previous, fork_id, model)
                all_alerts.extend(alerts)

            for a in alerts:
                print(
                    f"    [{a['severity']}] {a['scenario']} {a['metric']}: "
                    f"{a['prev']} → {a['curr']} ({a['delta_pct']:+.1f}%)"
                )

            update_leaderboard(leaderboard, fork, version, model, result)
            run_summary.append(
                {
                    "fork_id": fork_id,
                    "model": model,
                    "version": version,
                    "status": (
                        "CRITICAL"
                        if any(a["severity"] == "CRITICAL" for a in alerts)
                        else "WARN" if alerts else "OK"
                    ),
                    "alerts": alerts,
                    "result_file": str(run_file),
                }
            )

    output_dir.mkdir(parents=True, exist_ok=True)

    if not args.dry_run:
        lb_out = output_dir / "leaderboard.json"
        lb_out.write_text(
            json.dumps(
                {
                    "generated_at": datetime.now(timezone.utc).isoformat(),
                    "scenarios": SCENARIOS,
                    "by_model_scenario": leaderboard,
                },
                indent=2,
            )
        )
        print(f"\nLeaderboard: {lb_out}")

        rr_out = output_dir / "regression_report.json"
        rr_out.write_text(
            json.dumps(
                {
                    "generated_at": datetime.now(timezone.utc).isoformat(),
                    "run_timestamp": timestamp,
                    "summary": run_summary,
                    "alerts": all_alerts,
                    "has_critical": any(
                        a["severity"] == "CRITICAL" for a in all_alerts
                    ),
                },
                indent=2,
            )
        )
        print(f"Regression : {rr_out}")

    print(f"\n{'='*60}")
    print("SUMMARY")
    criticals = [a for a in all_alerts if a["severity"] == "CRITICAL"]
    warns = [a for a in all_alerts if a["severity"] == "WARN"]
    print(f"  CRITICAL: {len(criticals)}  WARN: {len(warns)}")
    for a in criticals:
        print(
            f"  !! {a['fork_id']} / {a['model']} / {a['scenario']} "
            f"{a['metric']}: {a['delta_pct']:+.1f}%"
        )

    if criticals:
        print("\nCRITICAL regressions detected — exit 1")
        return 1
    print("\nAll checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
