#!/usr/bin/env python3
"""Validate and synchronize Lemonade's test-owned inference definitions.

.github/inference-tests.json is the source of truth. ``sync`` regenerates the
EXE/DEB matrix blocks and the consolidated DMG inference steps. ``check``
validates the manifest, backend coverage, required-gate integration, and that
the workflow matches the generated form.

This is intentionally not a migration utility. The migration has already
happened; ongoing edits belong in the manifest followed by ``sync``.
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
import sys
from collections import OrderedDict
from pathlib import Path
from typing import Any, Mapping, Sequence

SCHEMA_VERSION = 1
WORKFLOW_REL = Path(".github/workflows/cpp_server_build_test_release.yml")
MANIFEST_REL = Path(".github/inference-tests.json")

MATRIX_JOBS = OrderedDict(
    (
        ("exe", "test-exe-inference"),
        ("deb", "test-deb-inference"),
    )
)
DMG_JOB = "test-dmg-inference"
CHECK_JOB_ID = "check-inference-matrix"
REQUIRED_GATE_JOB = "backends-gate"

BACKEND_ROOTS = (
    Path("src/cpp/server/backends"),
    Path("src/cpp/include/lemon/backends"),
)

BEGIN_MARKER = "# BEGIN GENERATED INFERENCE MATRIX: {platform}"
END_MARKER = "# END GENERATED INFERENCE MATRIX: {platform}"
DMG_BEGIN_MARKER = "# BEGIN GENERATED DMG INFERENCE TESTS"
DMG_END_MARKER = "# END GENERATED DMG INFERENCE TESTS"

TEST_FIELDS = {"script", "extra_args", "impact"}
IMPACT_FIELDS = {"direct", "consumes", "note"}
COMMON_PLATFORM_FIELDS = {"test", "backends", "runner", "note"}
DMG_PLATFORM_FIELDS = COMMON_PLATFORM_FIELDS | {"step_name"}
EXCEPTION_FIELDS = {"reason", "covered_by"}

SAFE_PLAIN_YAML = re.compile(r"^[A-Za-z0-9_./${}:+-]+$")
YAML_RESERVED = {
    "null",
    "~",
    "true",
    "false",
    "yes",
    "no",
    "on",
    "off",
}
TEST_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
BACKEND_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")


class MatrixError(RuntimeError):
    """Invalid manifest or generated workflow state."""


def fail(message: str) -> "NoReturn":  # type: ignore[name-defined]
    raise MatrixError(message)


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        fail(f"required file does not exist: {path}")


def _load_manifest(path: Path) -> OrderedDict[str, Any]:
    try:
        with path.open(encoding="utf-8") as handle:
            return json.load(handle, object_pairs_hook=OrderedDict)
    except FileNotFoundError:
        fail(f"manifest does not exist: {path}")
    except json.JSONDecodeError as error:
        fail(f"could not parse {path}: {error}")


def discover_backend_dirs(repo: Path) -> set[str]:
    found: set[str] = set()
    existing_roots = 0
    for relative_root in BACKEND_ROOTS:
        root = repo / relative_root
        if not root.exists():
            continue
        existing_roots += 1
        found.update(path.name for path in root.iterdir() if path.is_dir())
    if existing_roots == 0:
        fail(
            "could not find either backend root: "
            + ", ".join(path.as_posix() for path in BACKEND_ROOTS)
        )
    if not found:
        fail("backend roots exist but contain no backend directories")
    return found


def _unknown_fields(
    problems: list[str],
    *,
    context: str,
    value: Mapping[str, Any],
    allowed: set[str],
    note_hint: bool = False,
) -> None:
    unknown = sorted(set(value) - allowed)
    if not unknown:
        return
    message = (
        f"{context} has unsupported fields {unknown}; "
        f"allowed fields are {sorted(allowed)}"
    )
    if note_hint:
        message += ". Use 'note' for a generated YAML comment."
    problems.append(message)


def validate_manifest(repo: Path, manifest: Mapping[str, Any]) -> None:
    problems: list[str] = []
    if manifest.get("schema_version") != SCHEMA_VERSION:
        problems.append(
            f"schema_version must be {SCHEMA_VERSION}, got {manifest.get('schema_version')!r}"
        )

    tests = manifest.get("tests")
    platforms = manifest.get("platforms")
    exceptions = manifest.get("coverage_exceptions", {})
    if not isinstance(tests, Mapping) or not tests:
        problems.append("tests must be a non-empty object")
        tests = {}
    if not isinstance(platforms, Mapping):
        problems.append("platforms must be an object")
        platforms = {}
    if not isinstance(exceptions, Mapping):
        problems.append("coverage_exceptions must be an object")
        exceptions = {}

    backend_dirs = discover_backend_dirs(repo)
    observed: set[str] = set()

    for test_id, definition in tests.items():
        if not isinstance(test_id, str) or not TEST_ID_RE.fullmatch(test_id):
            problems.append(f"invalid test id {test_id!r}")
            continue
        if not isinstance(definition, Mapping):
            problems.append(f"test {test_id!r} must be an object")
            continue
        _unknown_fields(
            problems,
            context=f"test {test_id!r}",
            value=definition,
            allowed=TEST_FIELDS,
        )

        script = definition.get("script")
        extra_args = definition.get("extra_args")
        impact = definition.get("impact")
        if not isinstance(script, str) or not script:
            problems.append(f"test {test_id!r} has no script")
        elif not (repo / "test" / script).is_file():
            problems.append(f"test {test_id!r} references missing test/{script}")
        if not isinstance(extra_args, str):
            problems.append(f"test {test_id!r} extra_args must be a string")
        elif "\n" in extra_args or "\r" in extra_args:
            problems.append(f"test {test_id!r} extra_args must be single-line")

        if not isinstance(impact, Mapping):
            problems.append(f"test {test_id!r} has no impact object")
            continue
        _unknown_fields(
            problems,
            context=f"test {test_id!r} impact",
            value=impact,
            allowed=IMPACT_FIELDS,
        )
        note = impact.get("note")
        if note is not None and (not isinstance(note, str) or not note.strip()):
            problems.append(f"test {test_id!r} impact.note must be a non-empty string")

        direct = impact.get("direct")
        consumes = impact.get("consumes")
        if not isinstance(direct, list) or not all(isinstance(x, str) for x in direct):
            problems.append(f"test {test_id!r} impact.direct must be a string list")
            direct = []
        if not isinstance(consumes, list) or not all(
            isinstance(x, str) for x in consumes
        ):
            problems.append(f"test {test_id!r} impact.consumes must be a string list")
            consumes = []
        if len(direct) != len(set(direct)):
            problems.append(f"test {test_id!r} has duplicate direct backends")
        if len(consumes) != len(set(consumes)):
            problems.append(f"test {test_id!r} has duplicate consumed backends")
        overlap = set(direct) & set(consumes)
        if overlap:
            problems.append(
                f"test {test_id!r} lists {sorted(overlap)} as direct and consumed"
            )
        for backend in list(direct) + list(consumes):
            if not BACKEND_ID_RE.fullmatch(backend):
                problems.append(f"test {test_id!r} has invalid backend id {backend!r}")
            elif backend not in backend_dirs:
                problems.append(
                    f"test {test_id!r} references unknown backend {backend!r}"
                )
            observed.add(backend)

    used_tests: set[str] = set()
    for platform in ("exe", "deb", "dmg"):
        entries = platforms.get(platform)
        if not isinstance(entries, list) or not entries:
            problems.append(f"platform {platform!r} must be a non-empty list")
            continue
        seen: set[str] = set()
        allowed = DMG_PLATFORM_FIELDS if platform == "dmg" else COMMON_PLATFORM_FIELDS
        for index, entry in enumerate(entries):
            if not isinstance(entry, Mapping):
                problems.append(f"platform {platform!r} entry {index} is not an object")
                continue
            test_id = entry.get("test")
            context = f"platform {platform!r} test {test_id!r}"
            _unknown_fields(
                problems,
                context=context,
                value=entry,
                allowed=allowed,
                note_hint=True,
            )
            if test_id not in tests:
                problems.append(
                    f"platform {platform!r} entry {index} references unknown test {test_id!r}"
                )
                continue
            used_tests.add(str(test_id))
            if test_id in seen:
                problems.append(
                    f"platform {platform!r} contains duplicate test {test_id!r}"
                )
            seen.add(str(test_id))

            runner = entry.get("runner")
            backends = entry.get("backends")
            note = entry.get("note")
            if (
                not isinstance(runner, list)
                or not runner
                or not all(isinstance(item, str) and item for item in runner)
            ):
                problems.append(f"{context} needs a non-empty runner list")
            if not isinstance(backends, str):
                problems.append(f"{context} backends must be a string")
            if note is not None and (not isinstance(note, str) or not note.strip()):
                problems.append(f"{context} note must be a non-empty string")

            if platform == "dmg":
                if runner != ["macos-latest"]:
                    problems.append(f"{context} runner must be ['macos-latest']")
                if isinstance(backends, str) and len(backends.split()) > 1:
                    problems.append(
                        f"{context} supports at most one backend because DMG tests run as steps"
                    )
                step_name = entry.get("step_name")
                if step_name is not None and (
                    not isinstance(step_name, str) or not step_name.strip()
                ):
                    problems.append(f"{context} step_name must be a non-empty string")

    unused = set(tests) - used_tests
    if unused:
        problems.append(
            "tests not assigned to any platform: " + ", ".join(sorted(unused))
        )

    for backend, exception in exceptions.items():
        if backend not in backend_dirs:
            problems.append(
                f"coverage exception references unknown backend {backend!r}"
            )
            continue
        if backend in observed:
            problems.append(
                f"backend {backend!r} is observed by matrix tests and must not also be excepted"
            )
        if not isinstance(exception, Mapping):
            problems.append(f"coverage exception {backend!r} must be an object")
            continue
        _unknown_fields(
            problems,
            context=f"coverage exception {backend!r}",
            value=exception,
            allowed=EXCEPTION_FIELDS,
        )
        if not isinstance(exception.get("reason"), str) or not exception.get("reason"):
            problems.append(f"coverage exception {backend!r} needs a reason")
        covered_by = exception.get("covered_by")
        if (
            not isinstance(covered_by, list)
            or not covered_by
            or not all(isinstance(item, str) and item for item in covered_by)
        ):
            problems.append(
                f"coverage exception {backend!r} needs a non-empty covered_by list"
            )
        else:
            for item in covered_by:
                if not (repo / item).exists():
                    problems.append(
                        f"coverage exception {backend!r} references missing path {item!r}"
                    )

    uncovered = backend_dirs - observed - set(exceptions)
    if uncovered:
        problems.append(
            "backend directories without matrix impact or coverage exception: "
            + ", ".join(sorted(uncovered))
        )

    if problems:
        raise MatrixError(
            "inference matrix validation failed:\n  - " + "\n  - ".join(problems)
        )


def _yaml_scalar(value: Any) -> str:
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return str(value)
    if not isinstance(value, str):
        fail(f"cannot render unsupported YAML scalar type: {type(value).__name__}")
    if (
        value
        and SAFE_PLAIN_YAML.fullmatch(value)
        and value.lower() not in YAML_RESERVED
        and not value.startswith(("-", "?", ":"))
    ):
        return value
    return json.dumps(value, ensure_ascii=False)


def _yaml_value(value: Any, *, key: str | None = None) -> str:
    if isinstance(value, list):
        return "[" + ", ".join(_yaml_scalar(item) for item in value) + "]"
    if key in {"extra_args", "backends"} and isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    return _yaml_scalar(value)


def _render_note(note: str | None, indent: str) -> list[str]:
    if not note:
        return []
    return [f"{indent}# {line.strip()}\n" for line in note.splitlines() if line.strip()]


def _expanded_matrix_leg(
    manifest: Mapping[str, Any], entry: Mapping[str, Any]
) -> OrderedDict[str, Any]:
    test_id = entry["test"]
    definition = manifest["tests"][test_id]
    leg: OrderedDict[str, Any] = OrderedDict()
    leg["name"] = test_id
    leg["script"] = definition["script"]
    leg["extra_args"] = definition["extra_args"]
    leg["backends"] = entry["backends"]
    leg["runner"] = entry["runner"]
    return leg


def render_matrix_block(manifest: Mapping[str, Any], platform: str) -> list[str]:
    indent = " " * 8
    item_indent = " " * 10
    field_indent = " " * 12
    lines = [
        f"{indent}{BEGIN_MARKER.format(platform=platform)}\n",
        f"{indent}include:\n",
    ]
    for entry in manifest["platforms"][platform]:
        lines.extend(_render_note(entry.get("note"), item_indent))
        leg = _expanded_matrix_leg(manifest, entry)
        items = list(leg.items())
        first_key, first_value = items[0]
        lines.append(
            f"{item_indent}- {first_key}: {_yaml_value(first_value, key=first_key)}\n"
        )
        for key, value in items[1:]:
            lines.append(f"{field_indent}{key}: {_yaml_value(value, key=key)}\n")
    lines.append(f"{indent}{END_MARKER.format(platform=platform)}\n")
    return lines


def render_dmg_block(manifest: Mapping[str, Any]) -> list[str]:
    indent = " " * 6
    body_indent = " " * 8
    lines = [f"{indent}{DMG_BEGIN_MARKER}\n"]
    for entry in manifest["platforms"]["dmg"]:
        test_id = entry["test"]
        definition = manifest["tests"][test_id]
        backend = entry["backends"].strip()
        step_name = entry.get("step_name") or (
            f"Test {test_id}-{backend}" if backend else f"Test {test_id}"
        )
        command_parts = [f".venv/bin/python test/{definition['script']}"]
        extra_args = definition["extra_args"].strip()
        if extra_args:
            command_parts.append(extra_args)
        if backend:
            command_parts.append(f"--backend {backend}")
        command_parts.append('--cli-binary "$CLI_BINARY"')

        lines.extend(_render_note(entry.get("note"), indent))
        lines.append(f"{indent}- name: {step_name}\n")
        lines.append(
            f"{body_indent}if: ${{{{ !cancelled() && steps.setup.outcome == 'success' }}}}\n"
        )
        lines.append(f"{body_indent}run: {' '.join(command_parts)}\n")
        lines.append("\n")
    lines.append(f"{indent}{DMG_END_MARKER}\n")
    return lines


def _replace_marked_block(
    workflow_text: str,
    *,
    begin_marker: str,
    end_marker: str,
    replacement: Sequence[str],
) -> str:
    lines = workflow_text.splitlines(keepends=True)
    begins = [index for index, line in enumerate(lines) if line.strip() == begin_marker]
    ends = [index for index, line in enumerate(lines) if line.strip() == end_marker]
    if len(begins) != 1 or len(ends) != 1 or begins[0] >= ends[0]:
        fail(
            f"expected exactly one generated block {begin_marker!r} .. {end_marker!r}; "
            "the committed workflow must already contain generation markers"
        )
    lines[begins[0] : ends[0] + 1] = replacement
    return "".join(lines)


def sync_workflow_text(workflow_text: str, manifest: Mapping[str, Any]) -> str:
    result = workflow_text
    for platform in MATRIX_JOBS:
        result = _replace_marked_block(
            result,
            begin_marker=BEGIN_MARKER.format(platform=platform),
            end_marker=END_MARKER.format(platform=platform),
            replacement=render_matrix_block(manifest, platform),
        )
    return _replace_marked_block(
        result,
        begin_marker=DMG_BEGIN_MARKER,
        end_marker=DMG_END_MARKER,
        replacement=render_dmg_block(manifest),
    )


def _find_job_block(workflow_text: str, job_id: str) -> str:
    lines = workflow_text.splitlines(keepends=True)
    header = re.compile(rf"^  {re.escape(job_id)}:\s*(?:#.*)?$")
    starts = [
        index for index, line in enumerate(lines) if header.match(line.rstrip("\n"))
    ]
    if len(starts) != 1:
        fail(f"expected exactly one workflow job {job_id!r}, found {len(starts)}")
    start = starts[0]
    next_job = re.compile(r"^  [A-Za-z0-9_.-]+:\s*(?:#.*)?$")
    end = len(lines)
    for index in range(start + 1, len(lines)):
        if next_job.match(lines[index].rstrip("\n")):
            end = index
            break
    return "".join(lines[start:end])


def validate_workflow_integration(workflow_text: str) -> None:
    check_job = _find_job_block(workflow_text, CHECK_JOB_ID)
    expected_command = "python3 .github/scripts/inference_matrix.py check"
    if expected_command not in check_job:
        fail(f"workflow job {CHECK_JOB_ID!r} must run {expected_command!r}")

    gate_job = _find_job_block(workflow_text, REQUIRED_GATE_JOB)
    needs_match = re.search(r"^    needs:\s*\[([^\]]+)\]", gate_job, re.MULTILINE)
    if not needs_match:
        fail(f"workflow job {REQUIRED_GATE_JOB!r} must use an inline needs list")
    needs = {item.strip() for item in needs_match.group(1).split(",") if item.strip()}
    if CHECK_JOB_ID not in needs:
        fail(
            f"workflow job {REQUIRED_GATE_JOB!r} must depend on {CHECK_JOB_ID!r}; "
            "otherwise matrix validation is not part of the required inference gate"
        )


def _workflow_diff(old: str, new: str) -> str:
    return "".join(
        difflib.unified_diff(
            old.splitlines(keepends=True),
            new.splitlines(keepends=True),
            fromfile=f"a/{WORKFLOW_REL.as_posix()}",
            tofile=f"b/{WORKFLOW_REL.as_posix()}",
        )
    )


def command_sync(args: argparse.Namespace) -> None:
    repo = Path(args.repo).resolve()
    workflow_path = repo / WORKFLOW_REL
    manifest = _load_manifest(repo / MANIFEST_REL)
    validate_manifest(repo, manifest)
    old = _read_text(workflow_path)
    validate_workflow_integration(old)
    new = sync_workflow_text(old, manifest)
    if args.dry_run:
        print(
            "Workflow is already synchronized."
            if old == new
            else _workflow_diff(old, new)
        )
        return
    if old != new:
        workflow_path.write_text(new, encoding="utf-8")
        print(f"Updated {WORKFLOW_REL.as_posix()}")
    else:
        print("Workflow is already synchronized.")


def command_check(args: argparse.Namespace) -> None:
    repo = Path(args.repo).resolve()
    workflow_text = _read_text(repo / WORKFLOW_REL)
    manifest = _load_manifest(repo / MANIFEST_REL)
    validate_manifest(repo, manifest)
    validate_workflow_integration(workflow_text)
    expected = sync_workflow_text(workflow_text, manifest)
    if workflow_text != expected:
        fail(
            "generated inference workflow content is out of sync with "
            f"{MANIFEST_REL.as_posix()}; run: python3 .github/scripts/inference_matrix.py sync\n"
            + _workflow_diff(workflow_text, expected)
        )

    print(
        "Inference matrix OK: "
        f"{len(manifest['tests'])} tests, "
        f"{len(manifest['platforms']['exe'])} EXE legs, "
        f"{len(manifest['platforms']['deb'])} DEB legs, "
        f"{len(manifest['platforms']['dmg'])} generated DMG tests."
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".", help="repository root")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="show the workflow diff without writing (sync only)",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("sync", help="regenerate workflow content from the manifest")
    subparsers.add_parser(
        "check", help="validate manifest, workflow sync, and gate wiring"
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "sync":
            command_sync(args)
        else:
            command_check(args)
    except MatrixError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
