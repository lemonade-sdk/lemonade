"""Verify C9 production provenance and preserve every pre-C9 contract byte."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from container_c9_control import BASE

REPO = Path(__file__).resolve().parents[1]


def git(revision, path):
    return subprocess.check_output(
        ["git", "show", revision + ":" + path], cwd=REPO, timeout=30
    )


def check(out):
    out.mkdir(parents=True, exist_ok=True)
    protected = []
    paths = subprocess.check_output(
        ["git", "ls-tree", "-r", "--name-only", BASE], cwd=REPO, text=True, timeout=30
    ).splitlines()
    for path in paths:
        if not (
            path.startswith("src/cpp/resources/backends/")
            or path.startswith("test/fixtures/")
            or path.startswith("test/container_")
            or path
            in (
                "src/cpp/server/backends/configured/backend_builtin_authorization.cpp",
                "src/cpp/server/backends/configured/backend_llamacpp_policy.cpp",
                "src/cpp/server/backends/configured/backend_llamacpp_export.cpp",
                "schemas/backend-definition-v1.schema.json",
                "schemas/backend-definition-v2.schema.json",
                "cmake/CxxStringLiterals.cmake",
                "cmake/BackendDefinitionResources.cmake",
                "src/cpp/resources/defaults.json",
            )
        ):
            continue
        data = (REPO / path).read_bytes()
        assert data == git(BASE, path), "C9ProtectedBytes:" + path
        protected.append(
            dict(
                path=path,
                bytes=len(data),
                sha256=hashlib.sha256(data).hexdigest(),
                diff_bytes=0,
            )
        )
    rows = []
    for name in ("halobox", "nathanw", "ds4_rocm"):
        path = REPO / f"src/cpp/resources/backends/{name}.json"
        if not path.exists():
            continue
        row = json.loads(
            (
                REPO / f"test/fixtures/backend-v2/{name}.production-parity.json"
            ).read_bytes()
        )
        document = json.loads(path.read_bytes())
        assert document == row["production_definition"], "C9FrozenProductionDefinition"
        assert (
            len(path.read_text().splitlines()) == row["authored_lines"]
        ), "C9AuthoredLineCount"
        assert (
            row["active"] is False and row["registry_validated"] is False
        ), "C9InactiveUnvalidated"
        revision, recipe, backend = row["stack_revision"], row["recipe"], row["backend"]
        pins = json.loads(git(revision, "src/cpp/resources/backend_versions.json"))
        assert pins[recipe][backend] == row["pin"], "C9ExactStackPin"
        assert (
            json.loads((REPO / "src/cpp/resources/backend_versions.json").read_bytes())[
                recipe
            ][backend]
            == row["pin"]
        ), "C9StagedPin"
        models = json.loads(git(revision, "src/cpp/resources/server_models.json"))
        assert {k: v for k, v in models.items() if v["recipe"] == recipe} == row[
            "source_models"
        ], "C9CompleteSourceInventory"
        registry = json.loads(
            (REPO / "src/cpp/resources/server_models.json").read_bytes()
        )
        assert {k: v for k, v in registry.items() if v["recipe"] == recipe} == row[
            "models"
        ], "C9ExactRegistryInventory"
        assert (
            json.loads(git(revision, "src/cpp/resources/defaults.json")).get(recipe)
            == row["defaults"]
        ), "C9SourceDefaultsInventory"
        fixture_path = "test/fixtures/backend-v2/" + row["c8a_fixture"]
        fixture_bytes = (REPO / fixture_path).read_bytes()
        assert fixture_bytes == git(BASE, fixture_path), "C9C8aFixtureUnchanged"
        assert (
            hashlib.sha256(fixture_bytes).hexdigest() == row["c8a_fixture_sha256"]
        ), "C9PinnedC8aFixture"
        fixture = json.loads(fixture_bytes)
        for quote in fixture["quotations"]:
            lines = git(fixture["source_revision"], quote["path"]).decode().splitlines()
            start, end = quote["lines"]
            assert (
                "\n".join(lines[start - 1 : end]) == quote["text"]
            ), "C9ExactSourceQuotation"
        if recipe == "llamacpp":
            assert document == fixture["definition"], "C9ExactC8aLlamaProduction"
        else:
            assert (
                document["merge"]["artifacts"]["main"]
                == fixture["definition"]["merge"]["artifacts"]["main"]
            ), "C9ExactC8aCheckpointConsumer"
        rows.append(
            {
                k: row[k]
                for k in (
                    "id",
                    "authored_lines",
                    "pin",
                    "owner_digest",
                    "inventory_scope",
                )
            }
        )
    (out / "protected.json").write_text(json.dumps(protected, indent=2) + "\n")
    (out / "inventory.json").write_text(json.dumps(rows, indent=2) + "\n")
    (out / "v1-and-policies.diff").write_bytes(b"")
    print(
        len(protected),
        "protected inputs byte-identical;",
        len(rows),
        "inactive C9 productions",
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    check(parser.parse_args().out.resolve())
