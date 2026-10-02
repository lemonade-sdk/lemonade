#!/usr/bin/env python3
"""Pin llamacpp backend versions in backend_versions.json to verified releases.

Reads the target release tags and the comma-separated list of backends whose
release assets were verified complete from environment variables, and updates
only those backends' pins. Shared by the Windows build, Linux build, and
create-pr jobs in validate_llamacpp.yml.
"""
import json
import os
import re
from pathlib import Path

RELEASE_RE = re.compile(r"^b[0-9]+$")


def parse_csv(value):
    return [item.strip() for item in (value or "").split(",") if item.strip()]


def update(section, release, backends):
    if not backends:
        return []
    if not RELEASE_RE.match(release):
        raise SystemExit(f"Invalid llama.cpp release tag: {release}")
    changed = []
    for key in backends:
        if key not in section:
            raise SystemExit(f"Refusing to create missing llamacpp.{key}")
        if not isinstance(section[key], str):
            raise SystemExit(f"llamacpp.{key} must be a string")
        old = section[key]
        section[key] = release
        changed.append((key, old, release))
    return changed


def main():
    path = Path("src/cpp/resources/backend_versions.json")
    data = json.loads(path.read_text(encoding="utf-8"))
    section = data.get("llamacpp")
    if not isinstance(section, dict):
        raise SystemExit("backend_versions.json is missing a llamacpp object")

    changes = []
    changes += update(
        section,
        os.environ["LLAMACPP_RELEASE"],
        parse_csv(os.environ.get("LLAMACPP_GGML_UPDATE_BACKENDS", "")),
    )
    changes += update(
        section,
        os.environ["LLAMACPP_ROCM_RELEASE"],
        parse_csv(os.environ.get("LLAMACPP_ROCM_UPDATE_BACKENDS", "")),
    )
    changes += update(
        section,
        os.environ["LLAMACPP_LEMONADE_RELEASE"],
        parse_csv(os.environ.get("LLAMACPP_LEMONADE_UPDATE_BACKENDS", "")),
    )

    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

    print("Updated llamacpp backend versions:")
    if changes:
        for key, old, new in changes:
            print(f"  llamacpp.{key}: {old} -> {new}")
    else:
        print("  none")


if __name__ == "__main__":
    main()
