#!/usr/bin/env python3
"""Shared release-tag parser and monotonic Debian version generator."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import subprocess
import sys
from typing import Mapping, Optional


class ReleaseChannelError(RuntimeError):
    pass


CMAKE_VERSION_RE = re.compile(
    r"^[ \t]*project\(lemon_cpp[ \t]+VERSION[ \t]+"
    r"(?P<version>[0-9]+\.[0-9]+\.[0-9]+)",
    re.MULTILINE,
)
TAG_RE = re.compile(
    r"^v(?P<base>(?:0|[1-9][0-9]*)\."
    r"(?:0|[1-9][0-9]*)\."
    r"(?:0|[1-9][0-9]*))"
    r"(?:-(?P<phase>alpha|beta|rc)\."
    r"(?P<number>0|[1-9][0-9]*))?$"
)
POSITIVE_INTEGER_RE = re.compile(r"^[1-9][0-9]*$")
SHA_RE = re.compile(r"^[0-9a-fA-F]{7,64}$")


@dataclass(frozen=True)
class ParsedTag:
    name: str
    base_version: str
    phase: Optional[str]
    phase_number: Optional[int]

    @property
    def prerelease_suffix(self) -> str:
        if self.phase is None:
            return ""
        return f"{self.phase}.{self.phase_number}"

    @property
    def is_stable(self) -> bool:
        return self.phase is None


@dataclass(frozen=True)
class Classification:
    tag_name: str
    base_version: str
    prerelease_suffix: str
    is_release_tag: bool
    is_stable: bool
    is_prerelease: bool
    build_label: str
    deb_version: str

    def outputs(self) -> Mapping[str, str]:
        return {
            "tag_name": self.tag_name,
            "base_version": self.base_version,
            "prerelease_suffix": self.prerelease_suffix,
            "is_release_tag": str(self.is_release_tag).lower(),
            "is_stable": str(self.is_stable).lower(),
            "is_prerelease": str(self.is_prerelease).lower(),
            "build_label": self.build_label,
            "deb_version": self.deb_version,
        }


def run_git(repository: Path, *args: str) -> str:
    completed = subprocess.run(
        ["git", *args],
        cwd=repository,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise ReleaseChannelError(
            f"git {' '.join(args)} failed: {completed.stderr.strip()}"
        )
    return completed.stdout.strip()


def project_version(repository: Path) -> str:
    cmake_path = repository / "CMakeLists.txt"
    try:
        content = cmake_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ReleaseChannelError(f"Cannot read {cmake_path}: {exc}") from exc
    match = CMAKE_VERSION_RE.search(content)
    if not match:
        raise ReleaseChannelError(
            "Could not extract project version from CMakeLists.txt."
        )
    version = match.group("version")
    # The tag parser also enforces the no-leading-zero rule for the project version.
    if not TAG_RE.fullmatch(f"v{version}"):
        raise ReleaseChannelError(
            f"CMake project version '{version}' is not canonical MAJOR.MINOR.PATCH."
        )
    return version


def parse_tag(tag_name: str, expected_base: str) -> ParsedTag:
    match = TAG_RE.fullmatch(tag_name)
    if not match:
        raise ReleaseChannelError(
            f"Invalid tag '{tag_name}'. Expected vMAJOR.MINOR.PATCH or "
            "vMAJOR.MINOR.PATCH-(alpha|beta|rc).N without leading zeroes."
        )
    base = match.group("base")
    if base != expected_base:
        raise ReleaseChannelError(
            f"Tag base version '{base}' does not match CMake version "
            f"'{expected_base}'."
        )
    number_text = match.group("number")
    return ParsedTag(
        name=tag_name,
        base_version=base,
        phase=match.group("phase"),
        phase_number=int(number_text) if number_text is not None else None,
    )


def parse_bool(value: str, name: str) -> bool:
    normalized = value.strip().lower()
    if normalized == "true":
        return True
    if normalized == "false":
        return False
    raise ReleaseChannelError(f"{name} must be 'true' or 'false', got '{value}'.")


def positive_integer(value: str, name: str) -> str:
    if not POSITIVE_INTEGER_RE.fullmatch(value):
        raise ReleaseChannelError(f"{name} must be a positive integer, got '{value}'.")
    return value


def normalized_sha(value: str) -> str:
    if not SHA_RE.fullmatch(value):
        raise ReleaseChannelError(
            f"commit_sha must contain 7-64 hexadecimal characters, got '{value}'."
        )
    return value.lower()[:7]


def verify_tag_at_head(repository: Path, tag_name: str) -> None:
    tag_commit = run_git(repository, "rev-parse", "--verify", f"refs/tags/{tag_name}^{{commit}}")
    head_commit = run_git(repository, "rev-parse", "HEAD")
    if tag_commit != head_commit:
        raise ReleaseChannelError(
            f"Tag '{tag_name}' points to {tag_commit}, but the checkout is {head_commit}."
        )


def derive_debian_version(
    base_version: str,
    parsed_tag: Optional[ParsedTag],
    build_serial: str,
    build_attempt: str,
    commit_sha: str,
) -> tuple[str, str]:
    """Return (deb_version, build_label).

    Stable releases intentionally omit the workflow serial so that the final
    version sorts above every '~0.*' snapshot/prerelease. All packages uploaded
    to bleeding-edge use the serial first, making publication order independent
    from tag reachability, tag type, and release-branch topology.
    """
    if parsed_tag is not None and parsed_tag.is_stable:
        return base_version, base_version

    if not build_serial:
        if parsed_tag is None:
            return "", ""
        suffix = parsed_tag.prerelease_suffix
        return f"{base_version}~0.{suffix}", f"{base_version}-{suffix}"

    serial = positive_integer(build_serial, "build_serial")
    attempt = positive_integer(build_attempt, "build_attempt")

    if parsed_tag is not None:
        suffix = parsed_tag.prerelease_suffix
        return (
            f"{base_version}~0.{serial}.{attempt}.{suffix}",
            f"{base_version}-{suffix}-build.{serial}.{attempt}",
        )

    short_sha = normalized_sha(commit_sha)
    return (
        f"{base_version}~0.{serial}.{attempt}.dev+g{short_sha}",
        f"{base_version}-dev.{serial}.{attempt}-g{short_sha}",
    )


def classify(
    repository: Path,
    tag_name: str,
    require_tag: bool,
    require_prerelease: bool,
    require_stable: bool,
    build_serial: str,
    build_attempt: str,
    commit_sha: str,
) -> Classification:
    if require_stable and require_prerelease:
        raise ReleaseChannelError(
            "require_stable and require_prerelease are mutually exclusive."
        )

    base_version = project_version(repository)
    parsed: Optional[ParsedTag] = None
    if tag_name:
        parsed = parse_tag(tag_name, base_version)
        verify_tag_at_head(repository, tag_name)
    elif require_tag or require_stable or require_prerelease:
        raise ReleaseChannelError(
            "A release tag is required for the requested classification."
        )

    if parsed is not None and require_stable and not parsed.is_stable:
        raise ReleaseChannelError(
            f"This operation requires a stable tag, got '{tag_name}'."
        )
    if parsed is not None and require_prerelease and parsed.is_stable:
        raise ReleaseChannelError(
            f"This operation requires a prerelease tag, got '{tag_name}'."
        )

    if not commit_sha:
        commit_sha = run_git(repository, "rev-parse", "HEAD")

    deb_version, build_label = derive_debian_version(
        base_version=base_version,
        parsed_tag=parsed,
        build_serial=build_serial,
        build_attempt=build_attempt,
        commit_sha=commit_sha,
    )

    return Classification(
        tag_name=tag_name,
        base_version=base_version,
        prerelease_suffix=parsed.prerelease_suffix if parsed else "",
        is_release_tag=parsed is not None,
        is_stable=bool(parsed and parsed.is_stable),
        is_prerelease=bool(parsed and not parsed.is_stable),
        build_label=build_label,
        deb_version=deb_version,
    )


def write_outputs(path: Path, values: Mapping[str, str]) -> None:
    with path.open("a", encoding="utf-8") as handle:
        for key, value in values.items():
            if "\n" in value or "\r" in value:
                raise ReleaseChannelError(f"Output '{key}' contains a newline.")
            handle.write(f"{key}={value}\n")


def command_classify(args: argparse.Namespace) -> int:
    repository = Path(args.repository).resolve()
    result = classify(
        repository=repository,
        tag_name=args.tag or "",
        require_tag=parse_bool(args.require_tag, "require_tag"),
        require_prerelease=parse_bool(
            args.require_prerelease, "require_prerelease"
        ),
        require_stable=parse_bool(args.require_stable, "require_stable"),
        build_serial=args.build_serial or "",
        build_attempt=args.build_attempt,
        commit_sha=args.commit_sha or "",
    )
    if args.output:
        write_outputs(Path(args.output), result.outputs())
    for key, value in result.outputs().items():
        print(f"{key}={value}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    classify_parser = subparsers.add_parser("classify")
    classify_parser.add_argument("--repository", required=True)
    classify_parser.add_argument("--tag", default="")
    classify_parser.add_argument("--require-tag", default="false")
    classify_parser.add_argument("--require-prerelease", default="false")
    classify_parser.add_argument("--require-stable", default="false")
    classify_parser.add_argument("--build-serial", default="")
    classify_parser.add_argument("--build-attempt", default="1")
    classify_parser.add_argument("--commit-sha", default="")
    classify_parser.add_argument("--output", default="")
    classify_parser.set_defaults(handler=command_classify)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        return args.handler(args)
    except ReleaseChannelError as exc:
        print(f"::error::{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
