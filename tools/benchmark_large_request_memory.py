#!/usr/bin/env python3
"""Reproduce and attribute lemond large-request memory retention on Linux."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict, dataclass
import hashlib
import http.client
import json
import os
from pathlib import Path
import platform
import socket
import statistics
import subprocess
import tempfile
import time
from typing import Any
from urllib.parse import urlparse

EXPECTED_VARIANTS = {
    "baseline",
    "serialization-only",
    "allocator-only",
    "combined",
}


@dataclass(frozen=True)
class Variant:
    name: str
    binary: Path


@dataclass(frozen=True)
class Target:
    name: str
    base_url: str
    pid: int


def parse_assignment(value: str) -> tuple[str, str]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("expected NAME=VALUE")
    name, assigned = value.split("=", 1)
    if not name or not assigned:
        raise argparse.ArgumentTypeError("expected non-empty NAME=VALUE")
    return name, assigned


def parse_source_commit(value: str) -> tuple[str, str]:
    name, commit = parse_assignment(value)
    if len(commit) != 40 or any(
        character not in "0123456789abcdefABCDEF" for character in commit
    ):
        raise argparse.ArgumentTypeError(
            "source commit must be a full 40-character SHA"
        )
    return name, commit.lower()


def source_commits_by_name(
    assignments: list[tuple[str, str]], expected_names: set[str]
) -> dict[str, str]:
    names = [name for name, _commit in assignments]
    if len(set(names)) != len(names):
        raise RuntimeError("source commit names must be unique")
    actual_names = set(names)
    if actual_names != expected_names:
        missing = sorted(expected_names - actual_names)
        extra = sorted(actual_names - expected_names)
        raise RuntimeError(
            f"source commits must match measured names; missing={missing}, extra={extra}"
        )
    return dict(assignments)


def parse_variant(value: str) -> Variant:
    name, binary = parse_assignment(value)
    path = Path(binary).expanduser().resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise argparse.ArgumentTypeError(f"not an executable file: {path}")
    return Variant(name, path)


def parse_target(value: str) -> Target:
    name, target = parse_assignment(value)
    if "," not in target:
        raise argparse.ArgumentTypeError("expected NAME=URL,PID")
    url, pid_text = target.rsplit(",", 1)
    parsed = urlparse(url)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise argparse.ArgumentTypeError(f"invalid target URL: {url}")
    try:
        pid = int(pid_text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid PID: {pid_text}") from error
    if pid <= 0:
        raise argparse.ArgumentTypeError("PID must be positive")
    return Target(name, url.rstrip("/"), pid)


def command_first_line(command: list[str]) -> str:
    try:
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return "unavailable"
    output = result.stdout.strip() or result.stderr.strip()
    return output.splitlines()[0] if output else "unavailable"


def environment_manifest() -> dict[str, Any]:
    return {
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "hostname": platform.node(),
        "platform": platform.platform(),
        "kernel": platform.release(),
        "machine": platform.machine(),
        "libc": " ".join(part for part in platform.libc_ver() if part),
        "python": platform.python_version(),
        "compiler": command_first_line(["c++", "--version"]),
    }


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def process_memory_kib(pid: int) -> dict[str, int]:
    values: dict[str, int] = {}
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith(("VmRSS:", "VmData:")):
            key, value, _unit = line.split()
            values[key.rstrip(":")] = int(value)
    if set(values) != {"VmRSS", "VmData"}:
        raise RuntimeError(f"could not read memory status for PID {pid}")
    return {"rss_kib": values["VmRSS"], "data_kib": values["VmData"]}


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def lemonade_api_key() -> str | None:
    """Return the credential accepted by both regular and internal routes."""
    return os.environ.get("LEMONADE_ADMIN_API_KEY") or os.environ.get(
        "LEMONADE_API_KEY"
    )


def request_json(
    base_url: str,
    method: str,
    path: str,
    body: bytes | None = None,
    timeout: float = 30,
) -> tuple[int, bytes]:
    parsed = urlparse(base_url)
    connection_class = (
        http.client.HTTPSConnection
        if parsed.scheme == "https"
        else http.client.HTTPConnection
    )
    connection = connection_class(parsed.hostname, parsed.port, timeout=timeout)
    try:
        headers = {"Content-Type": "application/json"} if body is not None else {}
        api_key = lemonade_api_key()
        if api_key:
            headers["Authorization"] = f"Bearer {api_key}"
        connection.request(method, f"{parsed.path.rstrip('/')}{path}", body, headers)
        response = connection.getresponse()
        payload = response.read()
        return response.status, payload
    finally:
        connection.close()


def wait_until_live(
    base_url: str, process: subprocess.Popen[Any], timeout: int
) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"lemond exited with status {process.returncode}")
        try:
            status, _body = request_json(base_url, "GET", "/live", timeout=2)
            if status == 200:
                return
        except OSError:
            pass
        time.sleep(0.2)
    raise RuntimeError(f"lemond did not become live within {timeout} seconds")


def send_many(
    base_url: str,
    endpoint: str,
    body: bytes,
    count: int,
    concurrency: int,
    timeout: float,
) -> float:
    def send_one(_request_number: int) -> None:
        status, response = request_json(
            base_url, "POST", endpoint, body=body, timeout=timeout
        )
        if status != 200:
            excerpt = response[:500].decode("utf-8", errors="replace")
            raise RuntimeError(f"{endpoint} returned HTTP {status}: {excerpt}")

    start = time.monotonic()
    with ThreadPoolExecutor(max_workers=concurrency) as executor:
        list(executor.map(send_one, range(count)))
    return time.monotonic() - start


def summarize_numbers(rows: list[dict[str, Any]], field: str) -> dict[str, float]:
    values = [float(row[field]) for row in rows]
    return {
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
    }


def stop_server(process: subprocess.Popen[Any], base_url: str) -> None:
    try:
        request_json(base_url, "POST", "/internal/shutdown", body=b"{}", timeout=5)
    except OSError:
        pass
    try:
        process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)


def run_stress(args: argparse.Namespace) -> dict[str, Any]:
    if platform.system() != "Linux":
        raise RuntimeError("stress mode requires Linux /proc process metrics")

    variants = args.variant
    names = {variant.name for variant in variants}
    if len(names) != len(variants):
        raise RuntimeError("variant names must be unique")
    if names != EXPECTED_VARIANTS:
        missing = sorted(EXPECTED_VARIANTS - names)
        extra = sorted(names - EXPECTED_VARIANTS)
        raise RuntimeError(
            f"expected the four attribution variants; missing={missing}, extra={extra}"
        )
    source_commits = source_commits_by_name(args.source_commit, names)

    prompt = "x" * (args.body_mib * 1024 * 1024)
    body = json.dumps(
        {"model": "mock", "prompt": prompt, "stream": False},
        separators=(",", ":"),
    ).encode()
    runs: list[dict[str, Any]] = []
    binaries = {
        variant.name: {
            "path": str(variant.binary),
            "sha256": sha256(variant.binary),
            "version": command_first_line([str(variant.binary), "--version"]),
        }
        for variant in variants
    }

    for repetition in range(1, args.repetitions + 1):
        for variant in variants:
            port = free_port()
            base_url = f"http://127.0.0.1:{port}"
            with tempfile.TemporaryDirectory(
                prefix=f"lemond-memory-{variant.name}-"
            ) as cache_dir:
                log_path = Path(cache_dir) / "lemond.log"
                with log_path.open("w", encoding="utf-8") as log:
                    process = subprocess.Popen(
                        [
                            str(variant.binary),
                            cache_dir,
                            "--host",
                            "127.0.0.1",
                            "--port",
                            str(port),
                        ],
                        stdout=log,
                        stderr=subprocess.STDOUT,
                    )
                try:
                    try:
                        wait_until_live(base_url, process, args.startup_timeout)
                    except Exception as error:
                        log_text = log_path.read_text(
                            encoding="utf-8", errors="replace"
                        )
                        raise RuntimeError(
                            f"{error}\nlemond log:\n{log_text[-4000:]}"
                        ) from error

                    if args.warmup_requests:
                        send_many(
                            base_url,
                            args.endpoint,
                            body,
                            args.warmup_requests,
                            args.concurrency,
                            args.request_timeout,
                        )
                    before = process_memory_kib(process.pid)
                    elapsed = send_many(
                        base_url,
                        args.endpoint,
                        body,
                        args.requests,
                        args.concurrency,
                        args.request_timeout,
                    )
                    time.sleep(args.settle_seconds)
                    after = process_memory_kib(process.pid)
                    runs.append(
                        {
                            "variant": variant.name,
                            "repetition": repetition,
                            "pid": process.pid,
                            "elapsed_s": elapsed,
                            "requests_per_s": args.requests / elapsed,
                            "rss_before_kib": before["rss_kib"],
                            "rss_after_kib": after["rss_kib"],
                            "rss_delta_kib": after["rss_kib"] - before["rss_kib"],
                            "data_before_kib": before["data_kib"],
                            "data_after_kib": after["data_kib"],
                            "data_delta_kib": after["data_kib"] - before["data_kib"],
                        }
                    )
                finally:
                    stop_server(process, base_url)

    summary: dict[str, Any] = {}
    for variant in variants:
        selected = [row for row in runs if row["variant"] == variant.name]
        summary[variant.name] = {
            field: summarize_numbers(selected, field)
            for field in (
                "rss_delta_kib",
                "data_delta_kib",
                "elapsed_s",
                "requests_per_s",
            )
        }

    return {
        "mode": "stress",
        "environment": environment_manifest(),
        "configuration": {
            "endpoint": args.endpoint,
            "body_mib": args.body_mib,
            "body_bytes": len(body),
            "requests": args.requests,
            "concurrency": args.concurrency,
            "warmup_requests": args.warmup_requests,
            "repetitions": args.repetitions,
            "settle_seconds": args.settle_seconds,
            "connection_policy": "one fresh TCP connection per request",
            "sampling": "VmRSS and VmData from /proc/PID/status before the measured batch and after the settle delay",
        },
        "source_commits": source_commits,
        "binaries": binaries,
        "runs": runs,
        "summary": summary,
    }


def get_timing(response: dict[str, Any], field: str) -> float | None:
    timings = response.get("timings")
    if isinstance(timings, dict) and isinstance(timings.get(field), (int, float)):
        return float(timings[field])
    return None


def run_real_backend(args: argparse.Namespace) -> dict[str, Any]:
    if platform.system() != "Linux":
        raise RuntimeError("real-backend mode requires Linux /proc process metrics")

    target_names = [target.name for target in args.target]
    if len(set(target_names)) != len(target_names):
        raise RuntimeError("target names must be unique")
    source_commits = source_commits_by_name(args.source_commit, set(target_names))
    binaries: dict[str, dict[str, str]] = {}
    for target in args.target:
        binary = Path(f"/proc/{target.pid}/exe").resolve(strict=True)
        binaries[target.name] = {
            "path": str(binary),
            "sha256": sha256(binary),
            "version": command_first_line([str(binary), "--version"]),
        }

    prompt = args.prompt_file.read_text(encoding="utf-8")
    request = {
        "model": args.model,
        "prompt": prompt,
        "max_tokens": args.max_tokens,
        "temperature": 0,
        "stream": False,
        "cache_prompt": args.cache_prompt,
    }
    body = json.dumps(request, separators=(",", ":")).encode()
    warmup_body = json.dumps(
        {
            "model": args.model,
            "prompt": "warmup",
            "max_tokens": 1,
            "temperature": 0,
            "stream": False,
            "cache_prompt": False,
        },
        separators=(",", ":"),
    ).encode()
    runs: list[dict[str, Any]] = []
    warmed_targets: set[str] = set()

    for repetition in range(1, args.repetitions + 1):
        targets = args.target if repetition % 2 else list(reversed(args.target))
        for target in targets:
            health_status, health_body = request_json(
                target.base_url, "GET", "/api/v1/health", timeout=10
            )
            if health_status != 200:
                raise RuntimeError(
                    f"{target.name} health returned HTTP {health_status}: "
                    f"{health_body[:500].decode('utf-8', errors='replace')}"
                )
            if target.name not in warmed_targets:
                for _warmup in range(args.warmup_requests):
                    warmup_status, warmup_response = request_json(
                        target.base_url,
                        "POST",
                        args.endpoint,
                        body=warmup_body,
                        timeout=args.request_timeout,
                    )
                    if warmup_status != 200:
                        excerpt = warmup_response[:1000].decode(
                            "utf-8", errors="replace"
                        )
                        raise RuntimeError(
                            f"{target.name} warm-up returned HTTP "
                            f"{warmup_status}: {excerpt}"
                        )
                warmed_targets.add(target.name)
            before = process_memory_kib(target.pid)
            start = time.monotonic()
            status, response_body = request_json(
                target.base_url,
                "POST",
                args.endpoint,
                body=body,
                timeout=args.request_timeout,
            )
            elapsed = time.monotonic() - start
            if status != 200:
                excerpt = response_body[:1000].decode("utf-8", errors="replace")
                raise RuntimeError(f"{target.name} returned HTTP {status}: {excerpt}")
            response = json.loads(response_body)
            time.sleep(args.settle_seconds)
            after = process_memory_kib(target.pid)
            timings = response.get("timings", {})
            runs.append(
                {
                    "target": target.name,
                    "repetition": repetition,
                    "wall_s": elapsed,
                    "rss_delta_kib": after["rss_kib"] - before["rss_kib"],
                    "data_delta_kib": after["data_kib"] - before["data_kib"],
                    "prompt_tokens": timings.get("prompt_n"),
                    "generated_tokens": timings.get("predicted_n"),
                    "prompt_tokens_per_s": get_timing(response, "prompt_per_second"),
                    "generation_tokens_per_s": get_timing(
                        response, "predicted_per_second"
                    ),
                }
            )

    summary: dict[str, Any] = {}
    numeric_fields = (
        "wall_s",
        "rss_delta_kib",
        "data_delta_kib",
        "prompt_tokens_per_s",
        "generation_tokens_per_s",
    )
    for target in args.target:
        selected = [row for row in runs if row["target"] == target.name]
        summary[target.name] = {
            field: summarize_numbers(
                [row for row in selected if row[field] is not None], field
            )
            for field in numeric_fields
            if any(row[field] is not None for row in selected)
        }

    return {
        "mode": "real-backend",
        "environment": environment_manifest(),
        "configuration": {
            "targets": [asdict(target) for target in args.target],
            "target_order": "listed order on odd repetitions, reversed on even repetitions",
            "endpoint": args.endpoint,
            "model": args.model,
            "prompt_file": str(args.prompt_file),
            "prompt_characters": len(prompt),
            "body_bytes": len(body),
            "max_tokens": args.max_tokens,
            "cache_prompt": args.cache_prompt,
            "warmup_requests": args.warmup_requests,
            "warmup_prompt": "warmup",
            "warmup_max_tokens": 1,
            "repetitions": args.repetitions,
            "settle_seconds": args.settle_seconds,
        },
        "source_commits": source_commits,
        "binaries": binaries,
        "runs": runs,
        "summary": summary,
    }


def add_common_output(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--output", type=Path, help="write the complete JSON result")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="mode", required=True)

    stress = subparsers.add_parser(
        "stress", help="run four fresh-binary allocator attribution variants"
    )
    stress.add_argument(
        "--variant",
        action="append",
        type=parse_variant,
        required=True,
        metavar="NAME=BINARY",
        help="repeat for baseline, serialization-only, allocator-only, and combined",
    )
    stress.add_argument(
        "--source-commit",
        action="append",
        type=parse_source_commit,
        required=True,
        metavar="NAME=FULL_SHA",
        help="repeat for each measured variant",
    )
    stress.add_argument("--endpoint", default="/api/v1/test")
    stress.add_argument("--body-mib", type=int, default=3)
    stress.add_argument("--requests", type=int, default=170)
    stress.add_argument("--concurrency", type=int, default=1)
    stress.add_argument("--warmup-requests", type=int, default=0)
    stress.add_argument("--repetitions", type=int, default=3)
    stress.add_argument("--settle-seconds", type=float, default=1.0)
    stress.add_argument("--startup-timeout", type=int, default=30)
    stress.add_argument("--request-timeout", type=float, default=30)
    add_common_output(stress)

    real = subparsers.add_parser(
        "real-backend", help="measure one or more already-running inference targets"
    )
    real.add_argument(
        "--target",
        action="append",
        type=parse_target,
        required=True,
        metavar="NAME=URL,PID",
    )
    real.add_argument(
        "--source-commit",
        action="append",
        type=parse_source_commit,
        required=True,
        metavar="NAME=FULL_SHA",
        help="repeat for each measured target",
    )
    real.add_argument("--model", required=True)
    real.add_argument("--prompt-file", type=Path, required=True)
    real.add_argument("--endpoint", default="/api/v1/completions")
    real.add_argument("--max-tokens", type=int, default=128)
    real.add_argument("--cache-prompt", action="store_true")
    real.add_argument("--warmup-requests", type=int, default=1)
    real.add_argument("--repetitions", type=int, default=3)
    real.add_argument("--settle-seconds", type=float, default=1.0)
    real.add_argument("--request-timeout", type=float, default=7200)
    add_common_output(real)
    return parser


def validate_positive(args: argparse.Namespace) -> None:
    for name in ("repetitions", "settle_seconds", "request_timeout"):
        if getattr(args, name) <= 0:
            raise RuntimeError(f"--{name.replace('_', '-')} must be positive")
    if args.mode == "stress":
        for name in ("body_mib", "requests", "concurrency", "startup_timeout"):
            if getattr(args, name) <= 0:
                raise RuntimeError(f"--{name.replace('_', '-')} must be positive")
        if args.warmup_requests < 0:
            raise RuntimeError("--warmup-requests cannot be negative")
    else:
        if args.max_tokens <= 0:
            raise RuntimeError("--max-tokens must be positive")
        if args.warmup_requests < 0:
            raise RuntimeError("--warmup-requests cannot be negative")
        if not args.prompt_file.is_file():
            raise RuntimeError(f"prompt file not found: {args.prompt_file}")


def print_summary(result: dict[str, Any]) -> None:
    print(json.dumps(result["summary"], indent=2, sort_keys=True))


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    try:
        validate_positive(args)
        result = run_stress(args) if args.mode == "stress" else run_real_backend(args)
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print_summary(result)


if __name__ == "__main__":
    main()
