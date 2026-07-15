#!/usr/bin/env python3
"""Update Lemonade's pinned stable-diffusion.cpp backend versions.

The stable-diffusion.cpp release tags used by Lemonade look like
``master-684-138da14``. This updater is intentionally narrow: it updates only
existing sd-cpp backend keys in backend_versions.json, rejects nightly aliases,
and does not create new keys.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Iterable

SDCPP_RELEASE_RE = re.compile(r"^master-[0-9]+-[0-9a-f]{7,40}$")
DEFAULT_BACKENDS = ("cpu", "vulkan", "rocm-stable", "metal", "cuda")
FORBIDDEN_BACKENDS = {"rocm-nightly"}


def parse_csv(value: str) -> list[str]:
    items = [item.strip() for item in value.split(",") if item.strip()]
    if not items:
        raise argparse.ArgumentTypeError("backend list must not be empty")
    return items


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--release",
        required=True,
        help="stable-diffusion.cpp release tag, e.g. master-684-138da14",
    )
    parser.add_argument(
        "--path",
        default="src/cpp/resources/backend_versions.json",
        help="Path to backend_versions.json",
    )
    parser.add_argument(
        "--backends",
        type=parse_csv,
        default=list(DEFAULT_BACKENDS),
        help=(
            "Comma-separated existing sd-cpp backend keys to update. "
            "Default: cpu,vulkan,rocm-stable,metal,cuda. rocm-nightly is rejected."
        ),
    )
    return parser.parse_args()


def validate_backends(
    requested: Iterable[str], section: dict[str, object]
) -> list[str]:
    keys: list[str] = []
    seen: set[str] = set()
    for key in requested:
        if key in seen:
            continue
        seen.add(key)
        if key in FORBIDDEN_BACKENDS:
            raise SystemExit("Refusing to update or create sd-cpp.rocm-nightly")
        if key not in section:
            raise SystemExit(
                f"Refusing to create missing sd-cpp.{key}; only existing backend keys may be updated."
            )
        value = section[key]
        if not isinstance(value, str):
            raise SystemExit(f"sd-cpp.{key} must be a string in backend_versions.json")
        keys.append(key)
    return keys


def main() -> int:
    args = parse_args()
    release = args.release.strip()
    if not SDCPP_RELEASE_RE.match(release):
        raise SystemExit(
            f"Invalid stable-diffusion.cpp release tag '{release}'. "
            "Expected format like master-684-138da14."
        )

    path = Path(args.path)
    data = json.loads(path.read_text(encoding="utf-8"))
    if "sd-cpp" not in data or not isinstance(data["sd-cpp"], dict):
        raise SystemExit("backend_versions.json is missing an sd-cpp object")

    section: dict[str, object] = data["sd-cpp"]
    backends = validate_backends(args.backends, section)

    old = {key: section[key] for key in backends}
    for key in backends:
        section[key] = release

    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

    print("Updated sd-cpp backend versions:")
    for key in backends:
        print(f"  sd-cpp.{key}: {old[key]} -> {release}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
