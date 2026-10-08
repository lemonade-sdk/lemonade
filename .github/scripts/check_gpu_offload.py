#!/usr/bin/env python3
"""Fail if any validation record reports a GPU-offload FAIL.

The sd-cpp and llama.cpp validators write per-backend JSON summaries whose records
carry a ``gpu`` field of ``PASS`` / ``FAIL`` / ``N/A``.
This gate reads those summaries and exits non-zero when any record is ``FAIL``.

It feeds ``validation-gate`` (which blocks merge) but is intentionally NOT a
dependency of ``create-pr``, so the weekly auto-bump PR still opens with the
failing rows visible for a reviewer to judge.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys
from pathlib import Path


def _run_url() -> str:
    """Browser URL for this workflow run, or "" when not running in Actions."""
    server = os.environ.get("GITHUB_SERVER_URL")
    repo = os.environ.get("GITHUB_REPOSITORY")
    run_id = os.environ.get("GITHUB_RUN_ID")
    if not (server and repo and run_id):
        return ""
    url = f"{server}/{repo}/actions/runs/{run_id}"
    attempt = os.environ.get("GITHUB_RUN_ATTEMPT")
    if attempt:
        url += f"/attempts/{attempt}"
    return url


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "globs",
        nargs="+",
        help="Glob(s) matching validation JSON summaries, "
        "e.g. 'sdcpp_validation_*.json'",
    )
    args = parser.parse_args()

    paths: list[str] = []
    for pattern in args.globs:
        paths.extend(sorted(glob.glob(pattern)))

    if not paths:
        print(
            f"ERROR: no validation summaries matched {args.globs}; "
            "expected GPU evidence is missing.",
            file=sys.stderr,
        )
        return 1

    failures: list[str] = []
    passed = 0
    not_applicable = 0
    for path in paths:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        if not isinstance(data, list):
            print(f"ERROR: {path} is not a JSON array", file=sys.stderr)
            return 1
        for record in data:
            gpu = str(record.get("gpu", "N/A")).upper()
            label = record.get("label") or record.get("backend") or Path(path).stem
            model = record.get("model", "?")
            size = record.get("size")
            ident = f"{label} / {model}" + (f" / {size}" if size else "")
            if gpu == "FAIL":
                failures.append(ident)
            elif gpu == "PASS":
                passed += 1
            else:
                not_applicable += 1

    print(
        f"Checked {len(paths)} summary file(s): {passed} PASS, "
        f"{len(failures)} FAIL, {not_applicable} N/A."
    )
    if failures:
        print("GPU offload FAILED for:")
        for item in failures:
            print(f"  - {item}")
        print(
            f"\n{len(failures)} model/backend combination(s) fell back to CPU. "
            "This blocks merge."
        )
        print(
            "\nFor the full device banners behind each failure, open the matching "
            "'Validate <backend>' job logs and its server-logs artifact (server-logs-<backend>) for this run:"
        )
        run_url = _run_url()
        if run_url:
            print(f"  {run_url}")
        return 1

    if passed == 0:
        print(
            f"No GPU-enforced records found across {len(paths)} summary file(s); "
            "nothing to confirm."
        )
        return 0

    print(
        f"GPU offload confirmed for every enforced model/backend combination "
        f"({passed} record(s))."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
