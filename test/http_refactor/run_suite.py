"""Run HTTP refactor + GUI3 contract suites in a reproducible order."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys

from harness import ROOT, find_lemond

HERE = Path(__file__).resolve().parent


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--lemond", default="./build/lemond", help="Built lemond from this checkout"
    )
    parser.add_argument(
        "--skip-gui3", action="store_true", help="Skip the real GUI3 TypeScript bridge"
    )
    parser.add_argument(
        "--self-test-only", action="store_true", help="No lemond required"
    )
    parser.add_argument(
        "--with-ui", action="store_true", help="Run existing GUI3 runtime tests last"
    )
    parser.add_argument(
        "--playwright",
        action="store_true",
        help="Run GUI3 Playwright tests at the very end",
    )
    parser.add_argument(
        "--baseline-lemond",
        help="Compare against a separately built GUI3_squashed server",
    )
    parser.add_argument(
        "--docs-check",
        action="store_true",
        help="Check generated API docs and recorded examples",
    )
    parser.add_argument(
        "--ctest", action="store_true", help="Run existing configured CTest suite"
    )
    parser.add_argument(
        "--repeat-race", type=int, default=0, help="Additional alias concurrency runs"
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="Show commands without running them"
    )
    args = parser.parse_args()
    if args.repeat_race < 0:
        parser.error("--repeat-race cannot be negative")
    if args.self_test_only and (
        args.with_ui
        or args.playwright
        or args.baseline_lemond
        or args.docs_check
        or args.ctest
        or args.repeat_race
    ):
        parser.error("--self-test-only cannot be combined with other suites")

    path = Path(args.lemond).expanduser()
    binary = (ROOT / path).resolve()
    if not args.self_test_only and not args.dry_run:
        try:
            binary = find_lemond(str(binary))
        except FileNotFoundError as error:
            parser.error(str(error))
    app = ROOT / "src" / "app"
    if not args.self_test_only and not args.skip_gui3 and not args.dry_run:
        if not (app / "node_modules" / "typescript").is_dir():
            parser.error("GUI3 requires (cd src/app && npm ci), or use --skip-gui3")
        if not shutil.which("node"):
            parser.error("Node.js missing; use --skip-gui3")
    if (args.with_ui or args.playwright) and not args.dry_run:
        if not (app / "node_modules" / "@playwright" / "test").is_dir():
            parser.error("UI tests require (cd src/app && npm ci)")

    commands = [
        (
            "Harness and manifest self-tests",
            [
                sys.executable,
                "-m",
                "unittest",
                "discover",
                "-s",
                "test/http_refactor",
                "-p",
                "test_*.py",
                "-v",
            ],
            ROOT,
        )
    ]
    if not args.self_test_only:
        commands.append(
            (
                "Isolated lemond contracts" + (" + GUI3" if not args.skip_gui3 else ""),
                [
                    sys.executable,
                    str(HERE / "server_contract.py"),
                    "--lemond",
                    str(binary),
                    *([] if args.skip_gui3 else ["--gui3"]),
                ],
                ROOT,
            )
        )
        for i in range(args.repeat_race):
            commands.append(
                (
                    f"Concurrent alias writes {i + 1}/{args.repeat_race}",
                    [
                        sys.executable,
                        str(HERE / "server_contract.py"),
                        "--lemond",
                        str(binary),
                        "--match",
                        "concurrent_alias_updates",
                    ],
                    ROOT,
                )
            )
        if args.baseline_lemond:
            old_binary = Path(args.baseline_lemond).expanduser().resolve()
            if not args.dry_run:
                try:
                    old_binary = find_lemond(str(old_binary))
                except FileNotFoundError as error:
                    parser.error(str(error))
            commands.append(
                (
                    "GUI3 baseline differential contracts",
                    [
                        sys.executable,
                        str(HERE / "compare_baseline.py"),
                        "--baseline-lemond",
                        str(old_binary),
                        "--candidate-lemond",
                        str(binary),
                    ],
                    ROOT,
                )
            )
        if args.docs_check:
            commands.append(
                (
                    "Generated documentation drift",
                    [
                        sys.executable,
                        "docs/tools/gen_api_boilerplate.py",
                        "--lemond",
                        str(binary),
                        "--check",
                    ],
                    ROOT,
                )
            )
        if args.ctest:
            commands.append(
                (
                    "C++ CTest",
                    ["ctest", "--test-dir", "build", "--output-on-failure"],
                    ROOT,
                )
            )
        if args.with_ui:
            commands.extend(
                [
                    (
                        "Existing app regressions",
                        ["node", "test/app/run-app-regression-tests.cjs"],
                        ROOT,
                    ),
                    ("GUI3 runtime tests", ["npm", "run", "test:runtime"], app),
                ]
            )
        if args.playwright:
            commands.append(
                ("GUI3 Playwright tests", ["npm", "run", "test:playwright"], app)
            )

    failures = []
    for title, command, cwd in commands:
        print(f"\n=== {title} ===", flush=True)
        print("$ " + " ".join(map(str, command)), flush=True)
        if args.dry_run:
            continue
        result = subprocess.run(command, cwd=cwd, check=False)
        if result.returncode:
            failures.append((title, result.returncode))
            print(f"FAIL: {title} ({result.returncode})", flush=True)
        else:
            print(f"PASS: {title}", flush=True)
    if args.dry_run:
        print(f"Planned {len(commands)} suites; none executed.")
        return 0
    print(f"\n{len(commands) - len(failures)}/{len(commands)} suites passed.")
    for title, status in failures:
        print(f"FAILED ({status}): {title}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
