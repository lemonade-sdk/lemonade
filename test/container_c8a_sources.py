"""Verify C8a source quotations and every protected C8 input literally."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from container_c8a_control import BASE

REPO = Path(__file__).resolve().parents[1]


def git(revision, path):
    return subprocess.check_output(
        ["git", "show", revision + ":" + path], cwd=REPO, timeout=30
    )


def check(out):
    out.mkdir(parents=True, exist_ok=True)
    protected = []
    for path in subprocess.check_output(
        ["git", "ls-tree", "-r", "--name-only", BASE], cwd=REPO, text=True, timeout=30
    ).splitlines():
        if not (
            path.startswith("src/cpp/resources/")
            or path.startswith("test/fixtures/")
            or path.startswith("test/container_")
            or path
            in (
                "src/cpp/server/backends/configured/backend_builtin_authorization.cpp",
                "src/cpp/server/backends/configured/backend_llamacpp_policy.cpp",
                "src/cpp/server/backends/configured/backend_llamacpp_export.cpp",
                "schemas/backend-definition-v1.schema.json",
                "cmake/CxxStringLiterals.cmake",
                "cmake/BackendDefinitionResources.cmake",
                "src/cpp/server/router.cpp",
            )
        ):
            continue
        data = (REPO / path).read_bytes()
        assert data == git(BASE, path), "C8aProtectedBytes:" + path
        protected.append(
            dict(
                path=path,
                bytes=len(data),
                sha256=hashlib.sha256(data).hexdigest(),
                diff_bytes=0,
            )
        )
    quotations = []
    for path in sorted((REPO / "test/fixtures/backend-v2").glob("c8a.*.json")):
        fixture = json.loads(path.read_bytes())
        if "quotations" not in fixture:
            continue
        for quote in fixture["quotations"]:
            lines = git(fixture["source_revision"], quote["path"]).decode().splitlines()
            start, end = quote["lines"]
            assert (
                "\n".join(lines[start - 1 : end]) == quote["text"]
            ), "C8aExactSourceQuotation:" + str(path)
            quotations.append(
                dict(fixture=path.name, path=quote["path"], lines=quote["lines"])
            )
    assert quotations, "C8aHasSourceQuotations"
    (out / "protected.json").write_text(json.dumps(protected, indent=2) + "\n")
    (out / "quotations.json").write_text(json.dumps(quotations, indent=2) + "\n")
    (out / "v1-and-policies.diff").write_bytes(b"")
    print(
        len(protected),
        "protected inputs byte-identical;",
        len(quotations),
        "exact source quotations",
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    check(parser.parse_args().out.resolve())
