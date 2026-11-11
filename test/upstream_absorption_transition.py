"""Replay every immutable stage through C9 in the pinned tree, then disclose the upstream absorption stage."""

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

import backend_v2_4e3b_transition as first
from backend_v1_snapshot import capture
from container_c8a_transition import census
from upstream_absorption_control import BASE

REPO = first.REPO
MANIFEST = REPO / "test/fixtures/backend-v2/upstream-absorption.catalog-transition.json"
UPSTREAM = "d718189de3cf263b2983ef6a8696c26fa5ab1a87"
STAGE = "upstream-absorption"
PROTECTED = (
    "src/cpp/server/backends/configured/backend_builtin_authorization.cpp",
    "src/cpp/server/backends/configured/backend_llamacpp_policy.cpp",
    "src/cpp/server/backends/configured/backend_llamacpp_export.cpp",
    "schemas/backend-definition-v1.schema.json",
    "schemas/backend-definition-v2.schema.json",
    "cmake/CxxStringLiterals.cmake",
    "cmake/BackendDefinitionResources.cmake",
    "src/cpp/resources/defaults.json",
    "src/cpp/resources/server_models.json",
)
IDENTITIES = (
    "admission",
    "execution",
    "catalog",
    "catalog_v2",
    "exports",
    "builtin_registry",
)
REVISIONS = dict(
    compiler=dict(old=5, new=6), runner_service_policy=dict(old=19, new=20)
)


def git(*arguments):
    return subprocess.check_output(["git", *arguments], cwd=REPO, timeout=30)


def protected_changes():
    rows = []
    for path in git("ls-tree", "-r", "--name-only", BASE).decode().splitlines():
        if not (
            path.startswith("src/cpp/resources/backends/")
            or path.startswith("test/fixtures/")
            or path.startswith("test/container_")
            or path in PROTECTED
        ):
            continue
        before = hashlib.sha256(git("show", BASE + ":" + path)).hexdigest()
        current = REPO / path
        after = (
            hashlib.sha256(current.read_bytes()).hexdigest()
            if current.is_file()
            else None
        )
        if after != before:
            rows.append(dict(path=path, base_sha256=before, sha256=after))
    return rows


def identities(evidence):
    result = {name: evidence[name] for name in IDENTITIES}
    result["hrx_execution"] = evidence["hrx"]["execution"]
    return result


def validate(manifest, previous, actual, old_ids, ids, changed, tests):
    assert set(manifest) == {
        "schema_version",
        "stage",
        "base_commit",
        "upstream_commit",
        "revisions",
        "protected_changes",
        "previous_identities",
        "identities",
        "cpp_ci",
        "changes",
    }, "AbsorptionExactTransitionFields"
    assert (
        type(manifest["schema_version"]) is int
        and manifest["schema_version"] == 1
        and manifest["stage"] == STAGE
        and manifest["base_commit"] == BASE
        and manifest["upstream_commit"] == UPSTREAM
    ), "AbsorptionExactTransitionProvenance"
    assert first.serialized(manifest["revisions"]) == first.serialized(
        REVISIONS
    ), "AbsorptionExactRevisionDisclosure"
    assert len({row["path"] for row in manifest["protected_changes"]}) == len(
        manifest["protected_changes"]
    ) and first.serialized(manifest["protected_changes"]) == first.serialized(
        changed
    ), "AbsorptionExactProtectedChanges"
    assert first.serialized(manifest["previous_identities"]) == first.serialized(
        old_ids
    ) and first.serialized(manifest["identities"]) == first.serialized(
        ids
    ), "AbsorptionExactStageIdentities"
    assert first.serialized(manifest["cpp_ci"]) == first.serialized(
        tests
    ), "AbsorptionExactCppCiCensus"
    assert len({row["path"] for row in manifest["changes"]}) == len(
        manifest["changes"]
    ), "AbsorptionUniqueTransitionRecords"
    assert first.serialized(manifest["changes"]) == first.serialized(
        first.diff(previous, actual)
    ), "AbsorptionExactTransitionRecords"
    assert first.serialized(
        first.apply(previous, manifest["changes"])
    ) == first.serialized(actual), "AbsorptionWholeTransitionReplay"


MUTATIONS = {
    "missing": "AbsorptionExactTransitionRecords",
    "duplicate": "AbsorptionUniqueTransitionRecords",
    "identity": "AbsorptionExactStageIdentities",
    "base": "AbsorptionExactTransitionProvenance",
    "version-type": "AbsorptionExactTransitionProvenance",
    "revision": "AbsorptionExactRevisionDisclosure",
    "undisclosed-source": "AbsorptionExactProtectedChanges",
    "source-bytes": "AbsorptionExactProtectedChanges",
    "census": "AbsorptionExactCppCiCensus",
    "field": "AbsorptionExactTransitionFields",
}


def mutate(manifest, rule):
    changed = copy.deepcopy(manifest)
    if rule == "missing":
        changed["changes"].pop()
    elif rule == "duplicate":
        changed["changes"].append(changed["changes"][0])
    elif rule == "identity":
        changed["identities"]["catalog"] = "sha256:" + "0" * 64
    elif rule == "base":
        changed["base_commit"] = UPSTREAM
    elif rule == "version-type":
        changed["schema_version"] = True
    elif rule == "revision":
        changed["revisions"]["compiler"]["new"] -= 1
    elif rule == "undisclosed-source":
        changed["protected_changes"].pop()
    elif rule == "source-bytes":
        changed["protected_changes"][0]["sha256"] = "0" * 64
    elif rule == "census":
        changed["cpp_ci"]["added"].pop()
    else:
        changed["active"] = False
    return changed


def run(args):
    for path in (args.out, args.scratch, args.scratch / "temporary"):
        path.mkdir(parents=True, exist_ok=True)
    os.environ.update(
        TMPDIR=str(args.scratch / "temporary"),
        TMP=str(args.scratch / "temporary"),
        TEMP=str(args.scratch / "temporary"),
    )
    tempfile.tempdir = str(args.scratch / "temporary")
    tree = args.base_build.parent / "source"
    prior = args.out / "prior"
    prior.mkdir(exist_ok=True)
    subprocess.run(
        [
            "timeout",
            "840",
            "python3",
            str(tree / "test/container_c9_transition.py"),
            "--build",
            str(args.base_build),
            "--base-build",
            str(tree / ".synagent/scratch/container-v2/c9/base-control/build"),
            "--out",
            str(prior),
            "--scratch",
            str(args.scratch / "prior"),
        ],
        cwd=tree,
        env=dict(os.environ, GIT_DIR=str(REPO / ".git")),
        check=True,
        timeout=850,
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
    assert result.returncode == 0, "AbsorptionActualProductionSwitchSuite"
    ids = identities(json.loads(evidence_path.read_bytes()))
    assert all(
        ids[name] != old_ids[name] for name in ids
    ), "AbsorptionEveryIdentityAdvances"

    actual = dict(
        v1=capture(args.build / "lemond", args.build / "lemonade", REPO),
        v2=first.extra(args.build / "lemond"),
    )
    inventory = REPO / "src/cpp/resources/container_inventory"
    if inventory.exists():
        for path in sorted(inventory.glob("*.json")):
            actual["v1"]["file_sha256"][str(path.relative_to(REPO))] = first.sha(
                path.read_bytes()
            )
    baseline, current = census(args.base_build), census(args.build)
    tests = dict(added=sorted(current - baseline), removed=sorted(baseline - current))
    changed = protected_changes()
    manifest = dict(
        schema_version=1,
        stage=STAGE,
        base_commit=BASE,
        upstream_commit=UPSTREAM,
        revisions=REVISIONS,
        protected_changes=changed,
        previous_identities=old_ids,
        identities=ids,
        cpp_ci=tests,
        changes=first.diff(previous, actual),
    )
    if args.author:
        MANIFEST.write_text(first.serialized(manifest, True) + "\n")
    manifest = json.loads(MANIFEST.read_bytes())
    validate(manifest, previous, actual, old_ids, ids, changed, tests)
    caught = []
    for rule, label in MUTATIONS.items():
        try:
            validate(
                mutate(manifest, rule), previous, actual, old_ids, ids, changed, tests
            )
        except AssertionError as error:
            assert str(error) == label, (rule, str(error))
        else:
            raise AssertionError("AbsorptionUncaughtDisclosureMutation:" + rule)
        caught.append(dict(rule=rule, assertion=label, caught=True))
    (args.out / "previous-snapshot.json").write_text(
        first.serialized(previous, True) + "\n"
    )
    (args.out / "actual-snapshot.json").write_text(
        first.serialized(actual, True) + "\n"
    )
    (args.out / "protected-changes.json").write_text(
        first.serialized(changed, True) + "\n"
    )
    (args.out / "census.json").write_text(
        first.serialized(dict(baseline=sorted(baseline), current=sorted(current)), True)
        + "\n"
    )
    (args.out / "disclosure-mutations.json").write_text(
        first.serialized(caught, True) + "\n"
    )
    print(
        "Every stage through C9 replays unchanged in the pinned tree;",
        len(manifest["changes"]),
        "absorption records,",
        len(changed),
        "protected inputs disclosed,",
        len(tests["added"]),
        "tests added,",
        len(tests["removed"]),
        "removed",
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--base-build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--author", action="store_true")
    args = parser.parse_args()
    for field in ("build", "base_build", "out", "scratch"):
        setattr(args, field, getattr(args, field).resolve())
    run(args)
