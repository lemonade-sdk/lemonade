"""Replay all immutable stages, then compare the disclosed six-container activation."""

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

import backend_v2_4e3b_transition as first
from backend_catalog_http import Server, prepare, request
from backend_v1_snapshot import capture
from container_activation_control import BASE
from container_c8a_transition import census
from upstream_absorption_transition import identities

REPO = first.REPO
MANIFEST = (
    REPO / "test/fixtures/backend-v2/container-activation.catalog-transition.json"
)
ENGINES = {"gufo", "rocmfpx", "halogen", "halobox", "nathanw", "ds4_rocm"}
NEW_FILES = {
    "test/container_activation_control.py",
    "test/container_activation_transition.py",
    "test/cpp/container_activation_expectations.h",
    "src/cpp/server/backends/configured/backend_active_container_policies.cpp",
}


def git(*args):
    return subprocess.check_output(["git", *args], cwd=REPO, timeout=60)


def protected_changes():
    rows = []
    paths = set(git("ls-tree", "-r", "--name-only", BASE).decode().splitlines())
    for name in sorted(paths | NEW_FILES):
        if (
            not name.startswith(("src/cpp/", "test/", "cmake/"))
            and name != "CMakeLists.txt"
        ):
            continue
        before = (
            hashlib.sha256(git("show", BASE + ":" + name)).hexdigest()
            if name in paths
            else None
        )
        path = REPO / name
        after = hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None
        if before != after:
            rows.append(dict(path=name, base_sha256=before, sha256=after))
    return rows


def extra(binary):
    with tempfile.TemporaryDirectory(prefix="activation-v2-") as temporary:
        root = Path(temporary)
        prepare(root)
        with Server(binary, root, "--builtin-backends-only") as server:

            def api(path, **kwargs):
                status, body, headers = request(server.port, path, **kwargs)
                return dict(status=status, body=body, etag=headers.get("ETag"))

            schema = api("/v1/backends/schema?representation=2")
            recipes = [row["recipe"] for row in schema["body"]["recipes"]]
            assert (
                len(recipes) == len(set(recipes)) == 20
            ), "ActivationTwentyPublicRecipes"
            return dict(
                schema2=schema,
                filtered2={
                    r: api("/v1/backends/schema?representation=2&recipe=" + r)
                    for r in recipes
                },
                conditional2=api(
                    "/v1/backends/schema?representation=2",
                    headers={"If-None-Match": schema["etag"]},
                ),
            )


def validate(manifest, previous, actual, old_ids, ids, sources, tests):
    assert set(manifest) == {
        "schema_version",
        "stage",
        "base_commit",
        "active",
        "runner_service_policy",
        "previous_identities",
        "identities",
        "protected_changes",
        "cpp_ci",
        "changes",
    }, "ActivationExactFields"
    assert (
        type(manifest["schema_version"]) is int
        and manifest["schema_version"] == 1
        and manifest["stage"] == "six-container-activation"
        and manifest["base_commit"] == BASE
        and manifest["active"] is True
    ), "ActivationExactProvenance"
    assert manifest["runner_service_policy"] == dict(
        old=20, new=21
    ), "ActivationExactPolicyRevision"
    assert first.serialized(manifest["previous_identities"]) == first.serialized(
        old_ids
    ) and first.serialized(manifest["identities"]) == first.serialized(
        ids
    ), "ActivationExactIdentities"
    assert first.serialized(manifest["protected_changes"]) == first.serialized(
        sources
    ), "ActivationExactSourceDisclosure"
    assert first.serialized(manifest["cpp_ci"]) == first.serialized(
        tests
    ), "ActivationExactCensus"
    assert len({row["path"] for row in manifest["changes"]}) == len(
        manifest["changes"]
    ), "ActivationUniqueRecords"
    assert first.serialized(manifest["changes"]) == first.serialized(
        first.diff(previous, actual)
    ), "ActivationExactRecords"
    assert first.serialized(
        first.apply(previous, manifest["changes"])
    ) == first.serialized(actual), "ActivationWholeSnapshotReplay"


def run(args):
    temporary = args.scratch / "temporary"
    for path in (args.out, args.scratch, temporary):
        path.mkdir(parents=True, exist_ok=True)
    os.environ.update(TMPDIR=str(temporary), TMP=str(temporary), TEMP=str(temporary))
    tempfile.tempdir = str(temporary)
    prior = args.out / "prior"
    tree = args.base_build.parent / "source"
    for name in (
        git("ls-tree", "-r", "--name-only", BASE, "test/fixtures/backend-v2")
        .decode()
        .splitlines()
    ):
        assert (REPO / name).read_bytes() == git("show", BASE + ":" + name), (
            "ActivationPriorFixtureBytes:" + name
        )
    subprocess.run(
        [
            "timeout",
            "1000",
            "python3",
            str(tree / "test/upstream_absorption_transition.py"),
            "--build",
            str(args.base_build),
            "--base-build",
            str(REPO / ".synagent/scratch/container-v2/absorption/base-control/build"),
            "--out",
            str(prior),
            "--scratch",
            str(args.scratch / "prior"),
        ],
        cwd=tree,
        check=True,
        timeout=1010,
    )
    previous = json.loads((prior / "actual-snapshot.json").read_bytes())
    old_ids = identities(json.loads((prior / "current-identities.json").read_bytes()))
    evidence_path = args.out / "current-identities.json"
    result = subprocess.run(
        ["timeout", "120", str(args.build / "test_llamacpp_v2_production_switch")],
        env=dict(os.environ, LEMON_SWITCH_OUT=str(evidence_path)),
        capture_output=True,
        text=True,
        timeout=130,
    )
    (args.out / "current-production-switch.log").write_text(
        result.stdout + result.stderr
    )
    assert result.returncode == 0, "ActivationProductionSwitchSuite"
    evidence = json.loads(evidence_path.read_bytes())
    assert (
        set(evidence["container_leaves"]) == ENGINES
    ), "ActivationExactlySixOwnedLeaves"
    ids = identities(evidence)
    assert (
        ids["exports"] == old_ids["exports"]
    ), "ActivationDoesNotExpandExternalExports"
    actual = dict(
        v1=capture(args.build / "lemond", args.build / "lemonade", REPO),
        v2=extra(args.build / "lemond"),
    )
    for path in sorted((REPO / "src/cpp/resources/container_inventory").glob("*.json")):
        actual["v1"]["file_sha256"][str(path.relative_to(REPO))] = first.sha(
            path.read_bytes()
        )
    baseline, current = census(args.base_build), census(args.build)
    assert baseline == current and len(current) == 203, "Activation203TestsNoDrops"
    tests = dict(count=203, added=[], removed=[])
    sources = protected_changes()
    manifest = dict(
        schema_version=1,
        stage="six-container-activation",
        base_commit=BASE,
        active=True,
        runner_service_policy=dict(old=20, new=21),
        previous_identities=old_ids,
        identities=ids,
        protected_changes=sources,
        cpp_ci=tests,
        changes=first.diff(previous, actual),
    )
    if args.author:
        assert not MANIFEST.exists(), "ActivationNeverRegeneratesManifest"
        MANIFEST.write_text(first.serialized(manifest, True) + "\n")
    manifest = json.loads(MANIFEST.read_bytes())
    validate(manifest, previous, actual, old_ids, ids, sources, tests)
    mutations = []
    for name, label in (
        ("missing", "ActivationExactRecords"),
        ("duplicate", "ActivationUniqueRecords"),
        ("identity", "ActivationExactIdentities"),
        ("source", "ActivationExactSourceDisclosure"),
        ("census", "ActivationExactCensus"),
        ("policy", "ActivationExactPolicyRevision"),
        ("base", "ActivationExactProvenance"),
    ):
        bad = copy.deepcopy(manifest)
        if name == "missing":
            bad["changes"].pop()
        elif name == "duplicate":
            bad["changes"].append(bad["changes"][0])
        elif name == "identity":
            bad["identities"]["catalog"] = "sha256:" + "0" * 64
        elif name == "source":
            bad["protected_changes"].pop()
        elif name == "census":
            bad["cpp_ci"]["count"] -= 1
        elif name == "policy":
            bad["runner_service_policy"]["new"] -= 1
        else:
            bad["base_commit"] = "0" * 40
        try:
            validate(bad, previous, actual, old_ids, ids, sources, tests)
        except AssertionError as error:
            assert str(error) == label, (name, str(error))
        else:
            raise AssertionError("ActivationUncaughtMutation:" + name)
        mutations.append(dict(rule=name, assertion=label, caught=True))
    for name, value in (
        ("previous-snapshot", previous),
        ("actual-snapshot", actual),
        ("disclosure-mutations", mutations),
        ("protected-changes", sources),
    ):
        (args.out / (name + ".json")).write_text(first.serialized(value, True) + "\n")
    print(
        "All 22 prior stages replay unchanged;",
        len(manifest["changes"]),
        "activation records; 203 tests retained; exact whole snapshot replay",
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("build", "base-build", "out", "scratch"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--author", action="store_true")
    args = parser.parse_args()
    for name in ("build", "base_build", "out", "scratch"):
        setattr(args, name, getattr(args, name).resolve())
    run(args)
