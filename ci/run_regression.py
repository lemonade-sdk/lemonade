#!/usr/bin/env python3
"""
Lemonade nightly backend perf regression runner.

For each tracked fork in benchmark_forks.json:
  1. Resolve the latest binary from GitHub releases
  2. Download and install the prebuilt binary
  3. Point lemond at the fork binary via LEMONADE_LLAMACPP_<BACKEND>_BIN
  4. Run lemonade bench across all models and scenarios
  5. Compare vs previous run and detect regressions
  6. Write per-run JSON results + aggregate leaderboard

Usage:
  python ci/run_regression.py --forks ci/benchmark_forks.json --output ci/results
  python ci/run_regression.py --forks ci/benchmark_forks.json --output ci/results --fork-filter llamacpp-rocm-nightly
  python ci/run_regression.py --forks ci/benchmark_forks.json --output ci/results --dry-run
"""

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile
from datetime import datetime, timezone
from pathlib import Path
from urllib.request import urlopen, Request
from urllib.error import HTTPError

# ---------------------------------------------------------------------------
# Models to benchmark
# ---------------------------------------------------------------------------
MODELS = [
    # Hot MoE models — Strix Halo / RDNA3 sweet spot
    "Qwen3.6-35B-A3B-GGUF",               # hot, 23 GB MoE, tool-calling
    "Qwen3.6-27B-MTP-GGUF",               # hot, 18.8 GB, MTP variant
    "Qwen3-Coder-30B-A3B-Instruct-GGUF",  # hot, 18.6 GB, coding — stresses data packing path

    # Hot vision/multimodal
    "Gemma-4-26B-A4B-it-GGUF",            # hot, 18.1 GB, vision
    "Gemma-4-31B-it-GGUF",                # hot, 19.5 GB, vision

    # Hot small/fast — baseline reference and quick regression signal
    "Qwen3.5-4B-GGUF",                    # hot, 3.58 GB, fastest signal

    # MXFP4 quant — AMD-specific format, needs its own regression line
    "gpt-oss-20b-mxfp4-GGUF",             # hot, 12.1 GB, MXFP4

    # Large model — tracks kyuz0 120B coverage, multi-GPU
    "gpt-oss-120b-mxfp-GGUF",             # hot, 63 GB
]

# Scenarios from bench_scenarios.json to run (by name or category)
SCENARIOS = ["chat-short", "code-short", "context-32k"]

# Bench config
WARMUP_RUNS = 1
MEASUREMENT_RUNS = 5

# Regression thresholds (fractional change, negative = drop)
THRESHOLDS = {
    "tps_warn":     -0.05,   # TPS drops >5%  → WARN
    "tps_critical": -0.15,   # TPS drops >15% → CRITICAL (CI fails)
    "ttft_warn":    +0.10,   # TTFT grows >10% → WARN
}

IS_WINDOWS = platform.system() == "Windows"
BACKEND_VERSIONS_PATH = Path("src/cpp/resources/backend_versions.json")


def get_therock_ver() -> str:
    """Read therock version from backend_versions.json, trimmed to major.minor.
    Mirrors get_therock_version() in llamacpp_server.cpp."""
    data = json.loads(BACKEND_VERSIONS_PATH.read_text())
    version = data["therock"]["version"]          # e.g. "7.13.0"
    parts = version.lstrip("v").split(".")
    return ".".join(parts[:2])                    # e.g. "7.13"


# ---------------------------------------------------------------------------
# GitHub helpers
# ---------------------------------------------------------------------------

def gh_api(path: str, token: str | None = None) -> dict:
    url = f"https://api.github.com/{path.lstrip('/')}"
    headers = {"Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = Request(url, headers=headers)
    with urlopen(req, timeout=30) as r:
        return json.loads(r.read())


def resolve_latest_version(repo: str, token: str | None = None,
                           tag_prefix: str = "") -> str:
    if tag_prefix:
        # For date-stamped releases, /releases/latest may not match the prefix.
        # List recent releases and pick the first whose tag starts with prefix.
        releases = gh_api(f"repos/{repo}/releases?per_page=10", token)
        for r in releases:
            if r["tag_name"].startswith(tag_prefix):
                return r["tag_name"]
        raise RuntimeError(f"No release found with prefix {tag_prefix!r} in {repo}")
    data = gh_api(f"repos/{repo}/releases/latest", token)
    return data["tag_name"]


def resolve_binary_url(fork: dict, version: str, therock_ver: str = "") -> str:
    key = "binary_url_windows" if IS_WINDOWS else "binary_url_linux"
    url = fork.get(key, "")
    arch = fork.get("hardware", ["gfx1100"])[0]
    url = url.replace("{version}", version)
    url = url.replace("{arch}", arch)
    # therock_ver may be empty for forks that don't need it — strip leftover dashes
    if therock_ver:
        url = url.replace("{therock_ver}", therock_ver)
    else:
        url = url.replace("-{therock_ver}", "").replace("{therock_ver}", "")
    return url


def download_file(url: str, dest: Path, token: str | None = None) -> None:
    print(f"  Downloading {url}")
    headers = {}
    if token and "github.com" in url:
        headers["Authorization"] = f"Bearer {token}"
    req = Request(url, headers=headers)
    try:
        with urlopen(req, timeout=300) as r, open(dest, "wb") as f:
            while chunk := r.read(1 << 16):
                f.write(chunk)
    except HTTPError as e:
        raise RuntimeError(f"Download failed ({e.code}): {url}") from e


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 16):
            h.update(chunk)
    return h.hexdigest()


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


def find_binary(install_dir: Path, exe_name: str) -> Path | None:
    for p in install_dir.rglob(exe_name):
        if p.is_file():
            return p
    return None


# ---------------------------------------------------------------------------
# Binary installation
# ---------------------------------------------------------------------------

def get_installed_version(install_dir: Path) -> str | None:
    """Read the version stored on disk from a previous install."""
    version_file = install_dir / "version.txt"
    if version_file.exists():
        return version_file.read_text().strip()
    return None


def install_fork_binary(fork: dict, version: str, binaries_dir: Path,
                        token: str | None, dry_run: bool, therock_ver: str = "") -> Path | None:
    fork_id = fork["fork_id"]
    exe_name = fork["binary_exe_windows"] if IS_WINDOWS else fork["binary_exe_linux"]

    # Store by fork_id only — one slot per fork, overwritten on version change.
    # This prevents daily-release forks (e.g. AMD-Ecosystem gfx11-rocm-nightly-YYYYMMDD)
    # from accumulating unbounded disk usage.
    install_dir = binaries_dir / fork_id
    installed_version = get_installed_version(install_dir)

    if installed_version == version:
        binary_path = find_binary(install_dir, exe_name)
        if binary_path and binary_path.exists():
            print(f"  [cache] {version} already installed: {binary_path}")
            return binary_path

    url = resolve_binary_url(fork, version, therock_ver)
    if not url:
        print(f"  [skip] No binary URL configured for {fork_id} on this platform")
        return None

    if dry_run:
        print(f"  [dry-run] Would download: {url}")
        print(f"  [dry-run] Would install to: {install_dir / exe_name}")
        return install_dir / exe_name

    if installed_version and installed_version != version:
        print(f"  Version changed {installed_version} → {version}, replacing binary")
        shutil.rmtree(install_dir, ignore_errors=True)

    archive_name = url.split("/")[-1]
    staging_dir = binaries_dir / f"{fork_id}.staging"
    shutil.rmtree(staging_dir, ignore_errors=True)
    staging_dir.mkdir(parents=True, exist_ok=True)

    archive_path = staging_dir / archive_name
    download_file(url, archive_path, token)
    print(f"  Extracting to {staging_dir}")
    extract_archive(archive_path, staging_dir)
    archive_path.unlink()

    # Atomic swap: staging → install
    if install_dir.exists():
        shutil.rmtree(install_dir)
    staging_dir.rename(install_dir)

    # Record version so next run can skip download
    (install_dir / "version.txt").write_text(version)

    binary_path = find_binary(install_dir, exe_name)
    if not binary_path:
        raise RuntimeError(f"Binary {exe_name!r} not found in {install_dir} after extraction")

    print(f"  Installed: {binary_path}")
    return binary_path


# ---------------------------------------------------------------------------
# lemonade bench runner
# ---------------------------------------------------------------------------

def find_lemonade_bin() -> str:
    candidates = [
        Path("build/Release/lemonade.exe"),
        Path("build/lemonade"),
        Path("build/Release/lemonade"),
    ]
    for c in candidates:
        if c.exists():
            return str(c)
    return "lemonade"



def run_bench(fork: dict, version: str, binary_path: Path, model: str,
              output_file: Path, compare_file: Path | None,
              dry_run: bool, lemonade_bin: str) -> dict | None:
    backend = fork["backend"]
    fork_id = fork["fork_id"]

    # Build the env var name: LEMONADE_LLAMACPP_ROCM-NIGHTLY_BIN etc.
    env_var = f"LEMONADE_LLAMACPP_{backend.upper()}_BIN"

    cmd = [
        lemonade_bin, "bench", model,
        "--backend", backend,
        "--scenarios", *SCENARIOS,
        "--runs", str(MEASUREMENT_RUNS),
        "--warmup", str(WARMUP_RUNS),
        "--auto-pull",
        "--json",
        "--output", str(output_file),
    ]
    if compare_file and compare_file.exists():
        cmd += ["--compare", str(compare_file)]

    env = os.environ.copy()
    env[env_var] = str(binary_path)

    print(f"    cmd: {' '.join(cmd)}")
    print(f"    env: {env_var}={binary_path}")

    if dry_run:
        print(f"    [dry-run] skipping execution")
        return None

    output_file.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(cmd, env=env, capture_output=True, text=True)

    if result.returncode != 0:
        print(f"    [ERROR] lemonade bench failed (exit {result.returncode})")
        print(result.stderr[-2000:])
        return None

    if not output_file.exists():
        print(f"    [ERROR] output file not created: {output_file}")
        return None

    with open(output_file) as f:
        data = json.load(f)

    data["fork_id"] = fork_id
    data["fork_label"] = fork["label"]
    data["fork_repo"] = fork["repo"]
    data["fork_version"] = version
    data["fork_backend"] = backend
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
    # Return the most recent run that isn't the current one
    return runs[1] if len(runs) > 1 else None


def check_regression(current: dict, previous: dict, fork_id: str, model: str) -> list[dict]:
    alerts = []
    curr_results = {r["backend"]: r for r in current.get("results", [])}
    prev_results = {r["backend"]: r for r in previous.get("results", [])}

    for backend, curr in curr_results.items():
        prev = prev_results.get(backend)
        if not prev:
            continue
        for curr_sc in curr.get("scenarios", []):
            sc_name = curr_sc["name"]
            prev_sc = next((s for s in prev.get("scenarios", []) if s["name"] == sc_name), None)
            if not prev_sc:
                continue

            curr_tps  = curr_sc.get("tps", {}).get("mean", 0)
            prev_tps  = prev_sc.get("tps", {}).get("mean", 0)
            curr_ttft = curr_sc.get("ttft_ms", {}).get("mean", 0)
            prev_ttft = prev_sc.get("ttft_ms", {}).get("mean", 0)

            if prev_tps > 0:
                delta_tps = (curr_tps - prev_tps) / prev_tps
                if delta_tps <= THRESHOLDS["tps_critical"]:
                    alerts.append({
                        "severity": "CRITICAL",
                        "fork_id": fork_id,
                        "model": model,
                        "scenario": sc_name,
                        "metric": "tps",
                        "delta_pct": round(delta_tps * 100, 2),
                        "prev": round(prev_tps, 2),
                        "curr": round(curr_tps, 2),
                    })
                elif delta_tps <= THRESHOLDS["tps_warn"]:
                    alerts.append({
                        "severity": "WARN",
                        "fork_id": fork_id,
                        "model": model,
                        "scenario": sc_name,
                        "metric": "tps",
                        "delta_pct": round(delta_tps * 100, 2),
                        "prev": round(prev_tps, 2),
                        "curr": round(curr_tps, 2),
                    })

            if prev_ttft > 0:
                delta_ttft = (curr_ttft - prev_ttft) / prev_ttft
                if delta_ttft >= THRESHOLDS["ttft_warn"]:
                    alerts.append({
                        "severity": "WARN",
                        "fork_id": fork_id,
                        "model": model,
                        "scenario": sc_name,
                        "metric": "ttft_ms",
                        "delta_pct": round(delta_ttft * 100, 2),
                        "prev": round(prev_ttft, 2),
                        "curr": round(curr_ttft, 2),
                    })

    return alerts


# ---------------------------------------------------------------------------
# Leaderboard aggregation
# ---------------------------------------------------------------------------

def update_leaderboard(leaderboard: dict, fork: dict, version: str,
                       model: str, result: dict) -> None:
    fork_id = fork["fork_id"]
    for backend_result in result.get("results", []):
        for sc in backend_result.get("scenarios", []):
            sc_name = sc["name"]
            tps_mean  = sc.get("tps", {}).get("mean")
            tps_p95   = sc.get("tps", {}).get("p95")
            ttft_mean = sc.get("ttft_ms", {}).get("mean")
            ttft_p95  = sc.get("ttft_ms", {}).get("p95")
            vram      = sc.get("vram_peak_gb")
            if tps_mean is None:
                continue
            key = f"{model}|{sc_name}"
            entry = {
                "model": model,
                "scenario": sc_name,
                "fork_id": fork_id,
                "fork_label": fork["label"],
                "fork_repo": fork["repo"],
                "fork_version": version,
                "backend": fork["backend"],
                "tps_mean": round(tps_mean, 2),
                "tps_p95": round(tps_p95, 2) if tps_p95 else None,
                "ttft_ms_mean": round(ttft_mean, 2) if ttft_mean else None,
                "ttft_ms_p95": round(ttft_p95, 2) if ttft_p95 else None,
                "vram_peak_gb": round(vram, 2) if vram and vram > 0 else None,
                "timestamp": result.get("timestamp"),
            }
            if key not in leaderboard:
                leaderboard[key] = []
            # Replace existing entry for this fork_id if present
            leaderboard[key] = [e for e in leaderboard[key] if e["fork_id"] != fork_id]
            leaderboard[key].append(entry)
            # Sort by TPS descending
            leaderboard[key].sort(key=lambda e: e["tps_mean"], reverse=True)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Lemonade nightly perf regression runner")
    parser.add_argument("--forks",        default="ci/benchmark_forks.json", help="Path to benchmark_forks.json")
    parser.add_argument("--output",       default="ci/results",              help="Directory for result JSON files")
    parser.add_argument("--binaries-dir", default="ci/binaries",             help="Directory for installed fork binaries")
    parser.add_argument("--fork-filter",  default="",                        help="Comma-separated fork_ids to run (blank = all tracked)")
    parser.add_argument("--model-filter", default="",                        help="Comma-separated model names to run (blank = all)")
    parser.add_argument("--dry-run",      action="store_true",               help="Print what would run without executing")
    parser.add_argument("--token",        default=os.environ.get("GH_TOKEN"), help="GitHub token for API calls")
    args = parser.parse_args()

    forks_path   = Path(args.forks)
    output_dir   = Path(args.output)
    binaries_dir = Path(args.binaries_dir)
    fork_filter  = [f.strip() for f in args.fork_filter.split(",") if f.strip()]
    model_filter = [m.strip() for m in args.model_filter.split(",") if m.strip()]
    models       = [m for m in MODELS if not model_filter or m in model_filter]
    lemonade_bin = find_lemonade_bin()
    timestamp    = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")

    print(f"=== Lemonade Perf Regression Runner ===")
    print(f"Forks config : {forks_path}")
    print(f"Output dir   : {output_dir}")
    print(f"Models       : {models}")
    print(f"Scenarios    : {SCENARIOS}")
    print(f"Lemonade bin : {lemonade_bin}")
    print(f"Dry run      : {args.dry_run}")
    print()

    with open(forks_path) as f:
        forks_config = json.load(f)

    forks = [
        fork for fork in forks_config["forks"]
        if fork.get("tracked", True)
        and (not fork_filter or fork["fork_id"] in fork_filter)
    ]

    print(f"Forks to run ({len(forks)}): {[f['fork_id'] for f in forks]}")
    print()

    leaderboard: dict = {}
    all_alerts:  list = []
    run_summary: list = []

    for fork in forks:
        fork_id = fork["fork_id"]
        print(f"{'='*60}")
        print(f"Fork: {fork_id}  ({fork['label']})")
        print(f"Repo: {fork['repo']}")

        # Skip if this fork requires a different OS than we're running on
        runner_os = fork.get("runner_os", "windows" if IS_WINDOWS else "linux")
        current_os = "windows" if IS_WINDOWS else "linux"
        if runner_os != current_os:
            print(f"  [skip] fork requires {runner_os}, running on {current_os}")
            continue

        # Resolve version
        try:
            tag_prefix = fork.get("version_tag_prefix", "")
            version = resolve_latest_version(fork["repo"], args.token, tag_prefix)
            print(f"Version: {version}")
        except Exception as e:
            print(f"  [ERROR] Could not resolve latest version: {e}")
            continue

        # Install binary
        try:
            therock_ver = get_therock_ver() if BACKEND_VERSIONS_PATH.exists() else ""
            binary_path = install_fork_binary(
                fork, version, binaries_dir, args.token, args.dry_run,
                therock_ver=therock_ver
            )
        except Exception as e:
            print(f"  [ERROR] Binary install failed: {e}")
            continue

        if binary_path is None:
            print(f"  [skip] No binary available for this platform")
            continue

        for model in models:
            print(f"\n  Model: {model}")
            run_file    = output_dir / fork_id / model / f"run-{timestamp}.json"
            prev_result = find_previous_result(output_dir, fork_id, model)

            result = run_bench(
                fork, version, binary_path, model,
                run_file, prev_result, args.dry_run, lemonade_bin
            )

            if result is None:
                run_summary.append({"fork_id": fork_id, "model": model, "status": "FAILED"})
                continue

            # Regression check
            alerts = []
            if prev_result and prev_result.exists():
                with open(prev_result) as f:
                    previous = json.load(f)
                alerts = check_regression(result, previous, fork_id, model)
                all_alerts.extend(alerts)

            for a in alerts:
                print(f"    [{a['severity']}] {a['scenario']} {a['metric']}: "
                      f"{a['prev']} → {a['curr']} ({a['delta_pct']:+.1f}%)")

            update_leaderboard(leaderboard, fork, version, model, result)
            run_summary.append({
                "fork_id": fork_id,
                "model": model,
                "version": version,
                "status": "CRITICAL" if any(a["severity"] == "CRITICAL" for a in alerts)
                          else "WARN" if alerts else "OK",
                "alerts": alerts,
                "result_file": str(run_file),
            })

    # Write leaderboard
    output_dir.mkdir(parents=True, exist_ok=True)
    leaderboard_out = output_dir / "leaderboard.json"
    leaderboard_data = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "scenarios": SCENARIOS,
        "models": models,
        "by_model_scenario": leaderboard,
    }
    if not args.dry_run:
        with open(leaderboard_out, "w") as f:
            json.dump(leaderboard_data, f, indent=2)
        print(f"\nLeaderboard written: {leaderboard_out}")

    # Write regression report
    regression_report = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "run_timestamp": timestamp,
        "summary": run_summary,
        "alerts": all_alerts,
        "has_critical": any(a["severity"] == "CRITICAL" for a in all_alerts),
    }
    report_out = output_dir / "regression_report.json"
    if not args.dry_run:
        with open(report_out, "w") as f:
            json.dump(regression_report, f, indent=2)
        print(f"Regression report:   {report_out}")

    # Print summary
    print(f"\n{'='*60}")
    print("REGRESSION SUMMARY")
    print(f"{'='*60}")
    criticals = [a for a in all_alerts if a["severity"] == "CRITICAL"]
    warns     = [a for a in all_alerts if a["severity"] == "WARN"]
    print(f"  CRITICAL: {len(criticals)}")
    print(f"  WARN:     {len(warns)}")
    for a in criticals:
        print(f"  !! CRITICAL {a['fork_id']} / {a['model']} / {a['scenario']} "
              f"/ {a['metric']}: {a['delta_pct']:+.1f}%")
    for a in warns:
        print(f"  ~  WARN    {a['fork_id']} / {a['model']} / {a['scenario']} "
              f"/ {a['metric']}: {a['delta_pct']:+.1f}%")

    if criticals:
        print("\nCRITICAL regressions detected — exiting with code 1")
        return 1

    print("\nAll checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
