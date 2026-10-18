"""Optional strict-query regression probes for PR #3771.

Not part of the default suite: known source-level parsing gaps currently make
some of these fail. They should become green after the HTTP parser is fixed.
"""

from __future__ import annotations

import argparse

from harness import IsolatedLemond, find_lemond

CASES = (
    ("/api/v1/models?show_all=invalid-boolean", 400),
    ("/internal/models/sync/status?sync_id=12.5", 400),
    ("/internal/models/sync/status?sync_id=-1", 400),
    ("/internal/models/sync/status?sync_id=not-a-number", 400),
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lemond", required=True, help="Binary built from this merge")
    args = parser.parse_args()
    errors = 0
    with IsolatedLemond(find_lemond(args.lemond)) as daemon:
        for path, expected in CASES:
            reply = daemon.api.get(path)
            passed = reply.status == expected
            print(
                f"{'PASS' if passed else 'FAIL'}: GET {path} -> {reply.status} (expected {expected})"
            )
            if not passed:
                print("    " + reply.text[:300])
                errors += 1
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
