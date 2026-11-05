#!/usr/bin/env python3
"""Generate Lemonade's llama.cpp GGUF architecture and decision-type allowlists, and
the decision-model limits that lemond mirrors."""

from __future__ import annotations

import argparse
import json
import os
import re
import urllib.parse
import urllib.request
from pathlib import Path

ARCHITECTURE_RE = re.compile(r'\{\s*LLM_ARCH_[A-Z0-9_]+,\s*"([^"]+)"\s*\}')
DECISION_TYPE_RE = re.compile(r'\{\s*COMMON_DECISION_TYPE_[A-Z0-9_]+,\s*"([^"]+)"\s*\}')
# A ref's source is these files joined; each pattern finds its own entries.
SOURCE_FILES = (
    "src/llama-arch.cpp",
    "common/common.cpp",
    "tools/server/server-decision.cpp",
    "tools/server/server-decision.h",
)
COMMIT_RE = re.compile(r"Llama\.cpp Commit Hash[^:]*:\s*`?([0-9a-f]+)", re.IGNORECASE)
EXCLUDED_ARCHITECTURES = {"(unknown)", "clip"}
# Both registry decision models have this type; without it the list is wrong.
REQUIRED_DECISION_TYPE = "laya"
# Limits lemond mirrors; each must have one value in every shipped ref.
DECISION_LIMITS = {
    "LLAMACPP_DECISION_H2O_MAX_STATE_TOKENS": re.compile(
        r"DECISION_H2O_MAX_STATE_TOKENS\s*=\s*(\d+)"
    ),
    "LLAMACPP_DECISION_LAYA_MAX_OPTION_TOKENS": re.compile(
        r"size_t\s+max_option_tokens\s*=\s*(\d+)"
    ),
    "LLAMACPP_DECISION_LAYA_QUESTION_RESERVE": re.compile(
        r"n_options_tokens\s*\+\s*(\d+)\s*>\s*max_head_tokens"
    ),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--versions",
        default="src/cpp/resources/backend_versions.json",
    )
    parser.add_argument(
        "--output",
        default="src/cpp/include/lemon/llamacpp_architectures_generated.h",
    )
    parser.add_argument(
        "--source",
        action="append",
        metavar="REF=PATH",
        help="Read one of a pinned ref's source files (llama-arch.cpp, common.cpp, "
        "server-decision.cpp/.h) from a local file (repeatable; files of one ref are joined)",
    )
    return parser.parse_args()


def names_from_sources(pattern: re.Pattern, sources: dict[str, str]) -> set[str]:
    name_sets = [set(pattern.findall(source)) for source in sources.values()]
    if not name_sets:
        return set()
    return set.intersection(*name_sets)


def architectures_from_sources(sources: dict[str, str]) -> list[str]:
    return sorted(names_from_sources(ARCHITECTURE_RE, sources) - EXCLUDED_ARCHITECTURES)


def decision_types_from_sources(sources: dict[str, str]) -> list[str]:
    return sorted(names_from_sources(DECISION_TYPE_RE, sources))


def decision_limits_from_sources(sources: dict[str, str]) -> dict[str, int]:
    limits = {}
    for name, pattern in DECISION_LIMITS.items():
        values = {ref: set(pattern.findall(source)) for ref, source in sources.items()}
        missing = sorted(ref for ref, found in values.items() if len(found) != 1)
        if missing:
            raise SystemExit(
                f"{name}: no single value in server-decision.cpp/.h of {missing}; "
                "llama.cpp changed it, so update the pattern and lemond's check"
            )
        distinct = {next(iter(found)) for found in values.values()}
        if len(distinct) != 1:
            raise SystemExit(f"{name}: the shipped refs disagree: {values}")
        limits[name] = int(distinct.pop())
    return limits


def fetch_text(url: str) -> str:
    headers = {"User-Agent": "lemonade-architecture-generator"}
    token = os.environ.get("GH_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    with urllib.request.urlopen(
        urllib.request.Request(url, headers=headers)
    ) as response:
        return response.read().decode("utf-8")


def fetch_upstream_source(ref: str) -> str:
    return "\n".join(
        fetch_text(f"https://raw.githubusercontent.com/ggml-org/llama.cpp/{ref}/{path}")
        for path in SOURCE_FILES
    )


def fetch_rocm_nightly_source(ref: str) -> str:
    release = json.loads(
        fetch_text(
            "https://api.github.com/repos/lemonade-sdk/llamacpp-rocm/"
            f"releases/tags/{ref}"
        )
    )
    commit_match = COMMIT_RE.search(release.get("body") or "")
    if not commit_match:
        raise RuntimeError(f"ROCm release {ref} does not identify its llama.cpp commit")
    short_commit = commit_match.group(1)
    query = urllib.parse.urlencode({"until": release["published_at"], "per_page": 100})
    commits = json.loads(
        fetch_text("https://api.github.com/repos/ggml-org/llama.cpp/commits?" + query)
    )
    matches = [
        commit["sha"] for commit in commits if commit["sha"].startswith(short_commit)
    ]
    if len(matches) != 1:
        raise RuntimeError(
            f"Could not uniquely resolve ROCm release {ref} commit {short_commit}"
        )
    return fetch_upstream_source(matches[0])


def render_allowlist(function: str, argument: str, names: list[str]) -> str:
    entries = "\n".join(f'    "{name}",' for name in names)
    initializer = f"{{{{\n{entries}\n    }}}}" if names else "{}"
    return f"""inline bool {function}(
    std::string_view {argument}) {{
    static constexpr std::array<std::string_view, {len(names)}> names = {initializer};
    return std::binary_search(names.begin(), names.end(), {argument});
}}"""


def render(configured_refs: list[str], sources: dict[str, str]) -> str:
    architectures = architectures_from_sources(sources)
    if not architectures:
        raise SystemExit("No architectures found in llama-arch.cpp")
    shipped_refs = ", ".join(configured_refs)
    source_refs = ", ".join(sorted(sources))
    decision_types = decision_types_from_sources(sources)
    if REQUIRED_DECISION_TYPE not in decision_types:
        raise SystemExit(
            f"Decision type '{REQUIRED_DECISION_TYPE}' is missing from common/common.cpp "
            "of at least one shipped ref; lemond would refuse every decision model"
        )
    architecture_check = render_allowlist(
        "llamacpp_architecture_is_supported_by_all_backends",
        "architecture",
        architectures,
    )
    decision_check = render_allowlist(
        "llamacpp_decision_type_is_supported_by_all_backends",
        "decision_type",
        decision_types,
    )
    limits = "\n".join(
        f"inline constexpr size_t {name} = {value};"
        for name, value in decision_limits_from_sources(sources).items()
    )
    return f"""// Generated by .github/scripts/update_llamacpp_architectures.py from
// Common denominator for shipped refs: {shipped_refs}. Sources: {source_refs}.
// Do not edit manually.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace lemon {{

{architecture_check}

{decision_check}

{limits}

}}  // namespace lemon
"""


def main() -> int:
    args = parse_args()
    versions = json.loads(Path(args.versions).read_text(encoding="utf-8"))
    pins = versions["llamacpp"]
    # A container backend's pin is an image digest (<tag>@sha256:...), not a
    # llama.cpp ref.
    configured_refs = sorted({ref for ref in pins.values() if "@" not in ref})
    sources: dict[str, str] = {}
    if args.source:
        for item in args.source:
            try:
                ref, path = item.split("=", 1)
            except ValueError as error:
                raise SystemExit("--source must use REF=PATH") from error
            text = Path(path).read_text(encoding="utf-8")
            sources[ref] = sources[ref] + "\n" + text if ref in sources else text
    else:
        rocm_nightly_ref = pins.get("rocm-nightly")
        for ref in configured_refs:
            try:
                if ref == rocm_nightly_ref:
                    sources[ref] = fetch_rocm_nightly_source(ref)
                else:
                    sources[ref] = fetch_upstream_source(ref)
            except Exception as error:  # pylint: disable=broad-exception-caught
                raise RuntimeError(
                    f"No architecture source found for shipped ref {ref}"
                ) from error
    missing_refs = set(configured_refs) - set(sources)
    unexpected_refs = set(sources) - set(configured_refs)
    if missing_refs or unexpected_refs:
        raise SystemExit(
            f"Architecture sources do not match configured refs: "
            f"missing={sorted(missing_refs)}, unexpected={sorted(unexpected_refs)}"
        )
    Path(args.output).write_text(render(configured_refs, sources), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
