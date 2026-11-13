"""Verify pinned C7b quotations and unchanged definitions and transition fixtures."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

REPO = Path(__file__).resolve().parents[1]
BASE = "957c3fc76ec8ddf35a589121761d69681a09a572"


def git(*args):
    return subprocess.check_output(["git", *args], cwd=REPO, timeout=30)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    quotations = []
    for engine in ("rocmfpx", "halobox", "nathanw", "halogen", "ds4"):
        fixture = json.loads(
            (REPO / f"test/fixtures/backend-v2/c7b.{engine}.json").read_bytes()
        )
        for quote in fixture["source_quotations"]:
            source = git("show", quote["commit"] + ":" + quote["path"])
            if "first_line" in quote:
                actual = b"".join(
                    source.splitlines(keepends=True)[
                        quote["first_line"] - 1 : quote["last_line"]
                    ]
                )
                assert actual == quote["text"].encode(), "ExactC7bPinnedQuotation"
            else:
                assert quote["text"].encode() in source, "ExactC7bPinnedMethod"
            quotations.append(
                dict(
                    engine=engine,
                    commit=quote["commit"],
                    path=quote["path"],
                    sha256=hashlib.sha256(quote["text"].encode()).hexdigest(),
                )
            )
        load = fixture["source_quotations"][0]["text"]
        for record in fixture["definition"]["merge"]["launch"]["diagnostics"]:
            literal = record["format"].split("{")[0].rstrip()
            assert literal in load, ("SourceContainsDiagnosticLiteral", engine, literal)
        stream = fixture["source_quotations"][1]["text"]
        if engine == "ds4":
            assert 'endpoint.find("chat/completions")' in stream
            assert fixture["rewrite_domain"] == "chat_completions_substring"
        else:
            assert fixture["rewrite_domain"] == "canonical_path"
            if engine != "halogen":
                assert 'endpoint == "/v1/chat/completions"' in stream
    paths = git("ls-tree", "-r", "--name-only", BASE).decode().splitlines()
    protected = []
    for path in paths:
        if not (
            path.startswith("src/cpp/resources/backends/")
            or path.startswith("test/fixtures/backend-v2/")
            or path
            in (
                "schemas/backend-definition-v1.schema.json",
                "src/cpp/resources/backend_versions.json",
                "src/cpp/resources/defaults.json",
                "src/cpp/resources/server_models.json",
                "src/cpp/server/backends/configured/backend_builtin_authorization.cpp",
                "src/cpp/server/backends/configured/backend_llamacpp_policy.cpp",
                "src/cpp/server/backends/configured/backend_llamacpp_export.cpp",
                "src/cpp/server/backends/configured/halogen_model_contract_internal.h",
                "test/cpp/test_backend_definition_v1_compat.cpp",
                "test/fixtures/backend-v1-compat/compiled.json",
                "src/cpp/server/router.cpp",
                "cmake/CxxStringLiterals.cmake",
                "cmake/BackendDefinitionResources.cmake",
            )
        ):
            continue
        old, current = git("show", BASE + ":" + path), (REPO / path).read_bytes()
        assert old == current, ("ProtectedC7bSourceBytes", path)
        protected.append(
            dict(
                path=path,
                bytes=len(current),
                sha256=hashlib.sha256(current).hexdigest(),
                diff_bytes=0,
            )
        )
    for name, value in (("quotations", quotations), ("protected-sources", protected)):
        (args.out / (name + ".json")).write_text(json.dumps(value, indent=2) + "\n")
    (args.out / "v1.diff").write_bytes(b"")
    print(
        len(quotations), "exact source quotations;", len(protected), "unchanged sources"
    )


if __name__ == "__main__":
    main()
