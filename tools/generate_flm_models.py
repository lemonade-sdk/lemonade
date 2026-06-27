#!/usr/bin/env python3
"""Generate the Lemonade FLM model registry snapshot from `flm list --json`."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = REPO_ROOT / "src" / "cpp" / "resources" / "flm_models.json"


def normalize_flm_name(name: str) -> str:
    return f"{name.replace(':', '-')}-FLM"


def normalize_models(raw: dict[str, Any], flm_version: str) -> dict[str, Any]:
    raw_models = raw.get("models")
    if not isinstance(raw_models, list):
        raise ValueError("FLM JSON must contain a models array")

    models: dict[str, Any] = {}
    for entry in raw_models:
        if not isinstance(entry, dict):
            continue
        name = entry.get("name")
        if not isinstance(name, str) or not name:
            continue

        labels = entry.get("label", [])
        if not isinstance(labels, list):
            labels = []

        model: dict[str, Any] = {
            "checkpoint": name,
            "recipe": "flm",
            "suggested": True,
            "size": entry.get("footprint", 0.0),
            "labels": [label for label in labels if isinstance(label, str)],
        }
        models[normalize_flm_name(name)] = model

    return {
        "_metadata": {
            "source": "flm list --json",
            "flm_version": flm_version,
            "model_count": len(models),
        },
        "models": dict(sorted(models.items())),
    }


def run_json(command: list[str]) -> dict[str, Any]:
    result = subprocess.run(
        command,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return json.loads(result.stdout)


def get_flm_version(flm_binary: str) -> str:
    data = run_json([flm_binary, "version", "--json"])
    version = data.get("version", "")
    if not isinstance(version, str) or not version:
        raise ValueError("FLM version JSON did not contain a version string")
    return version if version.startswith("v") else f"v{version}"


def generate(
    flm_binary: str,
    input_path: Path | None = None,
    version: str | None = None,
) -> dict[str, Any]:
    if input_path:
        raw = json.loads(input_path.read_text(encoding="utf-8"))
        flm_version = version or "unknown"
    else:
        raw = run_json([flm_binary, "list", "--json"])
        flm_version = version or get_flm_version(flm_binary)
    return normalize_models(raw, flm_version)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flm", default="flm", help="Path to the FLM executable")
    parser.add_argument(
        "--input",
        type=Path,
        help="Read raw flm list JSON from a file instead of running flm",
    )
    parser.add_argument("--version", help="FLM version to record when using --input")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    data = generate(args.flm, args.input, args.version)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"Wrote {args.output} ({data['_metadata']['model_count']} models)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
