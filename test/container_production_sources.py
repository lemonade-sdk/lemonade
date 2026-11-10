"""Check C8 provenance and all protected source and transition bytes."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

REPO = Path(__file__).resolve().parents[1]
BASE = "3af47274a644571d29b5af9b6b2b780c04864371"


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
            path.startswith("src/cpp/resources/backends/")
            or path.startswith("test/fixtures/backend-v2/")
            or path
            in (
                "src/cpp/server/backends/configured/backend_builtin_authorization.cpp",
                "src/cpp/server/backends/configured/backend_llamacpp_policy.cpp",
                "src/cpp/server/backends/configured/backend_llamacpp_export.cpp",
                "schemas/backend-definition-v1.schema.json",
                "schemas/backend-definition-v2.schema.json",
                "test/cpp/test_backend_definition_v1_compat.cpp",
                "test/fixtures/backend-v1-compat/compiled.json",
                "test/container_definition_transition.py",
                "test/gufo_definition_transition.py",
                "cmake/CxxStringLiterals.cmake",
                "cmake/BackendDefinitionResources.cmake",
                "src/cpp/resources/defaults.json",
                "src/cpp/server/router.cpp",
            )
        ):
            continue
        data = (REPO / path).read_bytes()
        assert data == git(BASE, path), "C8ProtectedSourceBytes:" + path
        protected.append(
            dict(
                path=path,
                bytes=len(data),
                sha256=hashlib.sha256(data).hexdigest(),
                diff_bytes=0,
            )
        )
    rows = []
    for path in sorted(
        (REPO / "test/fixtures/backend-v2").glob("*.production-parity.json")
    ):
        if path.stem.startswith("gufo."):
            continue
        row = json.loads(path.read_bytes())
        source = REPO / f"src/cpp/resources/backends/{row['id']}.json"
        assert (
            json.loads(source.read_bytes()) == row["production_definition"]
        ), "C8FrozenProductionBytes"
        assert (
            len(source.read_text().splitlines()) == row["authored_lines"]
        ), "C8AuthoredLineCount"
        assert (
            row["active"] is False and row["registry_validated"] is False
        ), "C8InactiveUnvalidatedProvenance"
        pins = json.loads(
            git(row["stack_revision"], "src/cpp/resources/backend_versions.json")
        )
        assert (
            pins[row["recipe"]][row["backend"]] == row["pin"]
        ), "C8ExactPinnedStackVersion"
        models = json.loads(
            git(row["stack_revision"], "src/cpp/resources/server_models.json")
        )
        assert {
            name: model
            for name, model in models.items()
            if model["recipe"] == row["recipe"]
        } == row["models"], "C8CompleteStackInventory"
        fixture = REPO / "test/fixtures/backend-v2" / row["c7b_fixture"]
        assert fixture.read_bytes() == git(
            BASE, str(fixture.relative_to(REPO))
        ), "C8UnchangedC7bParitySource"
        assert (
            hashlib.sha256(fixture.read_bytes()).hexdigest()
            == row["c7b_fixture_sha256"]
        ), "C8PinnedC7bFixture"
        for quote in json.loads(fixture.read_bytes())["source_quotations"]:
            text = git(quote["commit"], quote["path"])
            assert quote["text"].encode() in text, "C8ExactSourceQuotation"
        rows.append(
            dict(
                id=row["id"],
                lines=row["authored_lines"],
                models=len(row["models"]),
                pin=row["pin"],
                owner_digest=row["owner_digest"],
            )
        )
    blockers = []
    for name, revision, path in (
        (
            "halobox",
            "23e953915c96d2b2a039a0b3ffdfae46f97301d5",
            "src/cpp/server/backends/llamacpp/llamacpp_server.cpp",
        ),
        (
            "nathanw",
            "d491c946fed2ab079f7387cf02a0eaabb51afce4",
            "src/cpp/server/backends/llamacpp/llamacpp_server.cpp",
        ),
        (
            "ds4",
            "8f8b905a1cf68b60a4d7fdc66a15d846b0cebc78",
            "src/cpp/server/backends/ds4/ds4_server.cpp",
        ),
    ):
        source = git(revision, path).decode()
        if name == "ds4":
            assert (
                "fs::path(checkpoint).is_absolute() && fs::exists(checkpoint)" in source
            ), "C8PinnedDs4AbsoluteFallback"
            runner = (
                REPO
                / "src/cpp/server/backends/configured/backend_definition_runner.cpp"
            ).read_text()
            assert (
                'artifact.records.value("absolute_checkpoint_fallback", false)'
                in runner
                and 'throw std::invalid_argument("Configured v2 artifact consumer is unavailable")'
                in runner
            ), "C8RunnerRejectsRequiredFallback"
        else:
            assert source.index(
                'if (container && model_info.extra<bool>("hf_load", false))'
            ) < source.index(
                "backend_manager_->install_backend(llamacpp::spec()->recipe, llamacpp_backend)"
            ), "C8PinnedHfRefusalBeforeInstall"
        assert (
            not (REPO / f"src/cpp/resources/backends/{name}.json").exists()
            or name == "ds4"
        ), "C8StoppedEngineNotProduction"
        blockers.append(dict(engine=name, revision=revision, path=path))
    (out / "blocker-provenance.json").write_text(json.dumps(blockers, indent=2) + "\n")
    (out / "protected-sources.json").write_text(json.dumps(protected, indent=2) + "\n")
    (out / "inventory.json").write_text(json.dumps(rows, indent=2) + "\n")
    (out / "v1.diff").write_bytes(b"")
    print(
        len(protected),
        "protected sources byte-identical;",
        len(rows),
        "source-pinned inactive productions",
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    check(parser.parse_args().out)
