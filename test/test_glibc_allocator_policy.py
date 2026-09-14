#!/usr/bin/env python3
"""Exercise lemond's glibc allocator override policy through mallopt interposition."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import platform
import subprocess
import tempfile


def run_case(
    lemond: Path,
    probe: Path,
    name: str,
    overrides: dict[str, str],
    expect_mallopt: bool,
) -> None:
    with tempfile.TemporaryDirectory(prefix="lemond-mallopt-test-") as directory:
        log_path = Path(directory) / "mallopt.log"
        environment = os.environ.copy()
        environment.pop("MALLOC_MMAP_THRESHOLD_", None)
        environment.pop("GLIBC_TUNABLES", None)
        environment.update(overrides)
        environment["LEMONADE_MALLOPT_PROBE_PATH"] = str(log_path)
        existing_preload = environment.get("LD_PRELOAD")
        environment["LD_PRELOAD"] = (
            f"{probe}{os.pathsep}{existing_preload}" if existing_preload else str(probe)
        )

        result = subprocess.run(
            [str(lemond), "--version"],
            check=False,
            capture_output=True,
            env=environment,
            text=True,
            timeout=15,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"{name}: lemond --version returned {result.returncode}\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            )

        calls = (
            log_path.read_text(encoding="utf-8").splitlines()
            if log_path.exists()
            else []
        )
        expected_call = "1048576 threads=1"
        expected_calls = [expected_call] if expect_mallopt else []
        if calls != expected_calls:
            expectation = (
                "a single-threaded 1 MiB mallopt call"
                if expect_mallopt
                else "no mallopt call"
            )
            raise RuntimeError(f"{name}: expected {expectation}, observed {calls}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lemond", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    args = parser.parse_args()

    if platform.libc_ver()[0] != "glibc":
        print("SKIP: glibc allocator policy is not compiled on this platform")
        return 77

    cases = (
        ("default", {}, True),
        ("legacy override", {"MALLOC_MMAP_THRESHOLD_": "2097152"}, False),
        ("empty legacy override", {"MALLOC_MMAP_THRESHOLD_": ""}, False),
        (
            "exact tunable",
            {"GLIBC_TUNABLES": "glibc.malloc.mmap_threshold=2097152"},
            False,
        ),
        (
            "tunable among entries",
            {
                "GLIBC_TUNABLES": (
                    "glibc.malloc.trim_threshold=131072:"
                    "glibc.malloc.mmap_threshold=2097152:glibc.malloc.arena_max=4"
                )
            },
            False,
        ),
        (
            "empty tunable value",
            {"GLIBC_TUNABLES": "glibc.malloc.mmap_threshold="},
            False,
        ),
        (
            "unrelated tunable",
            {"GLIBC_TUNABLES": "glibc.malloc.trim_threshold=131072"},
            True,
        ),
        (
            "prefixed name",
            {"GLIBC_TUNABLES": "xglibc.malloc.mmap_threshold=2097152"},
            True,
        ),
        (
            "suffixed name",
            {"GLIBC_TUNABLES": "glibc.malloc.mmap_threshold_extra=2097152"},
            True,
        ),
        (
            "missing assignment",
            {"GLIBC_TUNABLES": "glibc.malloc.mmap_threshold"},
            True,
        ),
    )

    for name, overrides, expect_mallopt in cases:
        run_case(args.lemond, args.probe, name, overrides, expect_mallopt)
        print(f"PASS: {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
