"""Run the PR #3828 + GUI3 offline contract suites in a reproducible order.

The C++ lemond executable must already be built from the checked-out merge.
See README.md for build, prerequisite, and branch-update instructions.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys

from server_contract import ROOT, find_lemond


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--lemond",
        default="./build/lemond",
        help="Path to lemond built from this test branch",
    )
    parser.add_argument(
        "--skip-ui",
        action="store_true",
        help="Skip Node/GUI3 tests and the real TypeScript bridge",
    )
    parser.add_argument(
        "--repeat-race",
        type=int,
        default=0,
        help="Repeat the concurrency reproducer N extra times",
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="Print commands without executing them"
    )
    args = parser.parse_args()
    if args.repeat_race < 0:
        parser.error("--repeat-race must be zero or greater")

    try:
        binary = find_lemond(args.lemond)
    except FileNotFoundError as error:
        if not args.dry_run:
            parser.error(str(error))
        binary = str((ROOT / args.lemond).resolve())

    app = ROOT / "src" / "app"
    node_modules = app / "node_modules" / "typescript"
    if not args.skip_ui and not args.dry_run:
        if shutil.which("node") is None:
            parser.error("Node.js missing. Install Node.js or use --skip-ui")
        if not node_modules.is_dir():
            parser.error(
                "GUI3 dependencies missing: run 'cd src/app && npm ci' "
                "or use --skip-ui"
            )

    jobs = [
        (
            "Harness self-tests",
            [
                sys.executable,
                "-m",
                "unittest",
                "discover",
                "-s",
                "test/model_compat",
                "-p",
                "test_*.py",
                "-v",
            ],
            ROOT,
        ),
    ]
    if not args.skip_ui:
        jobs.extend(
            [
                (
                    "App regression tests",
                    ["node", "test/app/run-app-regression-tests.cjs"],
                    ROOT,
                ),
                ("GUI3 runtime tests", ["npm", "run", "test:runtime"], app),
            ]
        )
    jobs.extend(
        [
            (
                "Server contracts" + (" + live GUI3 API" if not args.skip_ui else ""),
                [
                    sys.executable,
                    "test/model_compat/server_contract.py",
                    "--lemond",
                    binary,
                    *([] if args.skip_ui else ["--gui3"]),
                    "--verbose",
                ],
                ROOT,
            ),
            (
                "Advanced model contracts",
                [
                    sys.executable,
                    "test/model_compat/advanced_contract.py",
                    "--lemond",
                    binary,
                    "--verbose",
                ],
                ROOT,
            ),
            (
                "Edge-case and concurrent contracts",
                [
                    sys.executable,
                    "test/model_compat/edge_contract.py",
                    "--lemond",
                    binary,
                    "--verbose",
                ],
                ROOT,
            ),
        ]
    )
    for i in range(args.repeat_race):
        jobs.append(
            (
                f"Repeated registration race {i + 1}/{args.repeat_race}",
                [
                    sys.executable,
                    "test/model_compat/advanced_contract.py",
                    "--lemond",
                    binary,
                    "--match",
                    "parallel_registrations",
                    "--verbose",
                ],
                ROOT,
            )
        )

    failures = []
    for title, command, cwd in jobs:
        display = " ".join(map(str, command))
        print(f"\n=== {title} ===\n{display}\n    cwd: {cwd}", flush=True)
        if args.dry_run:
            continue
        result = subprocess.run(command, cwd=cwd, check=False)
        if result.returncode:
            failures.append((title, result.returncode))
            print(f"FAIL: {title} (exit {result.returncode})", flush=True)
        else:
            print(f"PASS: {title}", flush=True)

    if args.dry_run:
        print(f"\nPlanned {len(jobs)} suites; no commands executed.")
        return 0
    print(
        f"\nCompleted {len(jobs)} suites: {len(jobs) - len(failures)} passed, "
        f"{len(failures)} failed."
    )
    for name, status in failures:
        print(f"  FAILED ({status}): {name}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
