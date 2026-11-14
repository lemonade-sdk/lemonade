"""Verify the inert transition baseline, source preservation and HRX dependency boundary."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from backend_definition_inventory import V1_NAMES, production_inventory
from llamacpp_frozen_control import BASE, REPO, ROOT, verify

MANIFEST = REPO / "test/fixtures/backend-v2/llamacpp.catalog-transition.json"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def git_bytes(path):
    return subprocess.check_output(
        ["git", "show", f"{BASE}:{path}"], cwd=REPO, timeout=30
    )


def protected_paths():
    paths = subprocess.check_output(
        [
            "git",
            "ls-tree",
            "-r",
            "--name-only",
            BASE,
            "--",
            "schemas",
            "src/cpp/resources",
            "test/fixtures/backend-v1-compat",
            "test/fixtures/llamacpp-oracle",
        ],
        cwd=REPO,
        text=True,
        timeout=30,
    ).splitlines()
    return paths + [
        "test/fixtures/backend-v2/llamacpp.candidate.json",
        "test/fixtures/backend-v2/hook-abi.inventory.json",
        "test/fixtures/backend-v2/llamacpp.deliberate-diffs.json",
    ]


def source_preservation(out):
    hashes = {}
    for path in protected_paths():
        expected = git_bytes(path)
        actual = (REPO / path).read_bytes()
        assert actual == expected, f"ProtectedSource:{path}"
        hashes[path] = sha(actual)
    (out / "source-preservation.json").write_text(json.dumps(hashes, indent=2) + "\n")
    print(f"ProtectedSource: {len(hashes)} unchanged payloads")


def baseline(manifest, snapshot):
    expected_keys = {
        "schema_version",
        "stage",
        "base_commit",
        "base_tree",
        "snapshot_sha256",
        "snapshot_bytes",
        "candidate_sha256",
        "v1_sources",
        "recipes",
        "changes",
    }
    assert set(manifest) == expected_keys, "ExactTransitionManifestFields"
    assert (
        manifest["schema_version"] == 1 and manifest["stage"] == "inert-preparation"
    ), "InertTransitionStage"
    assert manifest["changes"] == [], "InertCatalogTransitionHasNoChanges"
    assert manifest["base_commit"] == BASE, "TransitionBaseCommit"
    tree = subprocess.check_output(
        ["git", "rev-parse", f"{BASE}^{{tree}}"], cwd=REPO, text=True, timeout=30
    ).strip()
    assert manifest["base_tree"] == tree, "TransitionBaseTree"
    assert manifest["snapshot_sha256"] == sha(snapshot) and manifest[
        "snapshot_bytes"
    ] == len(snapshot), "PinnedWholeBaseSnapshot"
    assert manifest["candidate_sha256"] == sha(
        git_bytes("test/fixtures/backend-v2/llamacpp.candidate.json")
    ), "PinnedQualifiedCandidate"
    sources = {
        name: sha(git_bytes(f"src/cpp/resources/backends/{name}.json"))
        for name in sorted(V1_NAMES)
    }
    assert manifest["v1_sources"] == sources, "PinnedEightV1Sources"
    value = json.loads(snapshot)
    assert set(value["definitions"]) == V1_NAMES, "BaselineEightDefinitions"
    assert manifest["recipes"] == [
        row["recipe"] for row in value["schema"]["body"]["recipes"]
    ], "BaselineRecipeOrder"
    assert len(manifest["recipes"]) == 17, "BaselineSeventeenRecipes"


def purity(build, out):
    obj = (
        build
        / "CMakeFiles/lemonade-server-core.dir/src/cpp/server/backends/llamacpp/legacy_llamacpp_artifact_ops.cpp.o"
    )
    symbols = subprocess.check_output(["nm", "-C", str(obj)], text=True, timeout=30)
    (out / "legacy-artifact.symbols").write_text(symbols)
    forbidden = [
        "DefinitionRunner",
        "ConfiguredBackend",
        "runtime::",
        "typed::",
        "installed_backend",
        "registered_",
        "llamacpp::ops()",
        "resolve_llamacpp_runtime_args",
        "system_llamacpp_version",
        "LegacyLlamaCppArtifactOps::resolve_runtime_options",
        "LegacyLlamaCppArtifactOps::resolve_version",
        "LegacyLlamaCppArtifactOps::check_install",
        "all_registrations",
        "create_server",
        "descriptor_for",
    ]
    assert not any(
        name in symbols for name in forbidden
    ), "LegacyArtifactTargetCannotReachV2RuntimeOps"
    for name in (
        "populate_metadata",
        "resolve_checkpoint_path",
        "find_imported_checkpoint",
        "validate_registration_checkpoint",
        "validate_checkpoint_file",
    ):
        assert (
            f"LegacyLlamaCppArtifactOps::{name}" in symbols
        ), f"LegacyFiveArtifactMethods:{name}"
    print(
        "LegacyArtifactTargetCannotReachV2RuntimeOps: five legacy methods, no runner/registry/probe/default dependencies"
    )


def census(build, out):
    def load(folder):
        return json.loads(
            subprocess.check_output(
                [
                    "ctest",
                    "--test-dir",
                    str(folder),
                    "-L",
                    "^cpp-ci$",
                    "--show-only=json-v1",
                ],
                text=True,
                timeout=30,
            )
        )

    frozen, current = load(ROOT / "build"), load(build)
    old = sorted(row["name"] for row in frozen["tests"])
    new = sorted(row["name"] for row in current["tests"])
    assert len(old) == len(new) == 150 and old == new, "CppCi150ExactNamesPreserved"
    for row in current["tests"]:
        if row["name"].startswith(("LlamaCppOracle_", "LlamaCppOracleDescriptor_")):
            assert row["command"][0].startswith(
                str(ROOT / "build") + "/"
            ), "FrozenOracleRegistrationIsolated"
    dependencies = subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "query", "cpp-ci-tests"],
        text=True,
        timeout=30,
    )
    for view in ("linux_x64", "linux_arm64", "windows_x64", "macos_arm64"):
        assert (
            f"test_llamacpp_oracle_{view}" in dependencies.split()
        ), "FrozenOracleAggregateDependency"
    (out / "cpp-ci.json").write_text(json.dumps(current, indent=2) + "\n")
    (out / "census.json").write_text(
        json.dumps(
            {
                "base": BASE,
                "before": len(old),
                "after": len(new),
                "added": sorted(set(new) - set(old)),
                "dropped": sorted(set(old) - set(new)),
                "names": new,
            },
            indent=2,
        )
        + "\n"
    )
    print("CppCi150ExactNamesPreserved: 150 -> 150, no additions, removals or renames")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--actual", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    verify(ROOT / "source", ROOT / "build")
    source_preservation(args.out)
    census(args.build.resolve(), args.out)
    baseline(json.loads(MANIFEST.read_text()), args.baseline.read_bytes())
    assert (
        args.actual.read_bytes() == args.baseline.read_bytes()
    ), "InertWholeCatalogSnapshotUnchanged"
    assert production_inventory() == V1_NAMES, "ProductionV2RemainsAbsent"
    purity(args.build, args.out)
    print(
        "InertWholeCatalogSnapshotUnchanged: complete byte equality against reconstructed base"
    )


if __name__ == "__main__":
    main()
