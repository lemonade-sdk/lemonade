"""Replay eighteen immutable stages, then disclose each inactive C9 inventory stage."""

import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile

import backend_v2_4e3b_transition as first
import container_definition_transition as history
from backend_v1_snapshot import capture

REPO = first.REPO
from container_c9_control import BASE
from container_c9_sources import check as check_sources
from container_c8a_transition import census

ORDER = ["halobox", "nathanw", "ds4_rocm"]


def pinned(path):
    return subprocess.check_output(
        ["git", "show", BASE + ":" + path], cwd=REPO, timeout=30
    )


def context(evidence, names, pins):
    defaults = json.loads(pinned("src/cpp/resources/defaults.json"))
    ids = (
        set(first.RECIPES)
        | set(evidence["v1_ids"])
        | {"gufo", "rocmfpx", "halogen"}
        | set(names)
    )
    sections = set(defaults) | {"no_broadcast"}
    options = sections | set(evidence["common_defaults"])
    persisted = {}
    for row in evidence["descriptors"]:
        section = row["config_section"] or row["recipe"]
        ids.add(section)
        sections.add(section)
        options.update(o["name"] for o in row["options"])
        if row["options"]:
            persisted[section] = sorted({first.key(o["name"]) for o in row["options"]})
    for pair in evidence["spec_aliases"]:
        ids.update(x for x in pair if x)
    return first.identity(
        "lemonade.backend-builtin-context.v1\n",
        dict(
            ids=sorted(ids),
            recipes=sorted(first.RECIPES),
            sections=sorted(sections),
            options=sorted(options),
            persisted=persisted,
            defaults=defaults,
            pins=pins,
            exports=evidence["exports"],
        ),
    )


def validate(manifest, previous, expected, old_ids, ids, name, source_sha):
    assert set(manifest) == {
        "schema_version",
        "stage",
        "base_commit",
        "active",
        "source_sha256",
        "runner_service_policy",
        "previous_identities",
        "identities",
        "changes",
    }, "C9ExactTransitionFields"
    assert (
        manifest["schema_version"] == 1
        and manifest["active"] is False
        and manifest["stage"] == name + "-inactive-inventory"
        and manifest["base_commit"] == BASE
    ), "C9ExactTransitionProvenance"
    assert manifest["source_sha256"] == source_sha, "C9PinnedSourceBytes"
    index = ORDER.index(name)
    assert manifest["runner_service_policy"] == {
        "old": 16 + min(index, 2),
        "new": 16 + min(index + 1, 2),
    }, "C9ExactSelectorPolicyDisclosure"
    assert first.serialized(manifest["previous_identities"]) == first.serialized(
        old_ids
    ) and first.serialized(manifest["identities"]) == first.serialized(
        ids
    ), "C9ExactStageIdentities"
    assert len({r["path"] for r in manifest["changes"]}) == len(
        manifest["changes"]
    ), "C9UniqueTransitionRecords"
    assert first.serialized(manifest["changes"]) == first.serialized(
        first.diff(previous, expected)
    ), "C9ExactTransitionRecords"
    assert first.serialized(
        first.apply(previous, manifest["changes"])
    ) == first.serialized(expected), "C9WholeTransitionReplay"


def run(args):
    for path in (args.out, args.scratch, args.scratch / "temporary"):
        path.mkdir(parents=True, exist_ok=True)
    os.environ.update(
        TMPDIR=str(args.scratch / "temporary"),
        TMP=str(args.scratch / "temporary"),
        TEMP=str(args.scratch / "temporary"),
    )
    tempfile.tempdir = str(args.scratch / "temporary")
    check_sources(args.out / "preservation")
    tree = args.base_build.parent / "source"
    for name, target in ((".synagent", REPO / ".synagent"), (".git", REPO / ".git")):
        if not (tree / name).exists():
            (tree / name).symlink_to(target, target_is_directory=True)
    prior = args.out / "prior"
    prior.mkdir(exist_ok=True)
    subprocess.run(
        [
            "timeout",
            "360",
            "python3",
            str(tree / "test/container_c8a_transition.py"),
            "--build",
            str(args.base_build),
            "--base-build",
            str(REPO / ".synagent/scratch/container-v2/c8a/base-control/build"),
            "--out",
            str(prior),
            "--scratch",
            str(args.scratch / "prior"),
        ],
        cwd=tree,
        env=dict(os.environ, GIT_DIR=str(REPO / ".git")),
        check=True,
        timeout=370,
    )
    previous = json.loads((prior / "actual-snapshot.json").read_bytes())
    base = copy.deepcopy(previous)
    old_ids = json.loads(
        pinned(
            "test/fixtures/backend-v2/c8a.absolute-checkpoint-consumer.catalog-transition.json"
        )
    )["identities"]
    evidence = json.loads((prior / "current-identities.json").read_bytes())
    base_ids = json.loads(
        pinned("test/fixtures/backend-v2/exports.catalog-transition.json")
    )["identities"]
    base_ids.update(old_ids)
    pins = json.loads(pinned("src/cpp/resources/backend_versions.json"))
    names = []
    stages = []
    for name in ORDER:
        source = REPO / f"src/cpp/resources/backends/{name}.json"
        if not source.exists():
            break
        names.append(name)
        inventory = json.loads(
            (
                REPO / f"test/fixtures/backend-v2/{name}.production-parity.json"
            ).read_bytes()
        )
        pins.setdefault(inventory["recipe"], {})[inventory["backend"]] = inventory[
            "pin"
        ]
        ids_input = copy.deepcopy(base_ids)
        ids_input["builtin_context"] = context(evidence, names, pins)
        expected, ids = history.expected_snapshot(
            base,
            evidence,
            ids_input,
            json.loads(pinned("schemas/backend-definition-v2.schema.json")),
            runner_service_policy=16 + min(len(names), 2),
        )
        ids["builtin_context"] = ids_input["builtin_context"]
        for n in names:
            path = f"src/cpp/resources/backends/{n}.json"
            data = (REPO / path).read_bytes()
            expected["v1"]["file_sha256"][path] = first.sha(data)
            expected["v1"]["inactive_definitions"][n] = dict(
                recipe=json.loads(data)["recipe"],
                kind="container",
                source_sha256=first.sha(data),
            )
        manifest_path = (
            REPO / f"test/fixtures/backend-v2/c9.{name}.catalog-transition.json"
        )
        manifest = dict(
            schema_version=1,
            stage=name + "-inactive-inventory",
            base_commit=BASE,
            active=False,
            source_sha256=first.sha(source.read_bytes()),
            runner_service_policy=dict(
                old=16 + min(len(names) - 1, 2), new=16 + min(len(names), 2)
            ),
            previous_identities=old_ids,
            identities=ids,
            changes=first.diff(previous, expected),
        )
        if (
            args.author
            and ORDER.index(name) <= ORDER.index(args.author)
            and not manifest_path.exists()
        ):
            manifest_path.write_text(first.serialized(manifest, True) + "\n")
        manifest = json.loads(manifest_path.read_bytes())
        validate(
            manifest,
            previous,
            expected,
            old_ids,
            ids,
            name,
            first.sha(source.read_bytes()),
        )
        for mutation in ("missing", "duplicate", "identity", "activation", "source"):
            changed = copy.deepcopy(manifest)
            if mutation == "missing":
                changed["changes"].pop()
            elif mutation == "duplicate":
                changed["changes"].append(changed["changes"][0])
            elif mutation == "identity":
                changed["identities"]["catalog"] = "sha256:" + "0" * 64
            elif mutation == "activation":
                changed["active"] = True
            else:
                changed["source_sha256"] = "0" * 64
            try:
                validate(
                    changed,
                    previous,
                    expected,
                    old_ids,
                    ids,
                    name,
                    first.sha(source.read_bytes()),
                )
            except AssertionError:
                pass
            else:
                raise AssertionError("C9UncaughtDisclosureMutation:" + mutation)
        stages.append(
            dict(
                stage=manifest["stage"],
                records=len(manifest["changes"]),
                identities=ids,
            )
        )
        previous, old_ids = expected, ids
    assert first.serialized(pins) == first.serialized(
        json.loads((REPO / "src/cpp/resources/backend_versions.json").read_bytes())
    ), "C9ExactPinAdditions"
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
    assert result.returncode == 0, "C9ActualProductionSwitchSuite"
    expected_evidence = copy.deepcopy(evidence)
    for field in ("admission", "execution", "catalog", "catalog_v2"):
        expected_evidence[field] = old_ids[field]
    expected_evidence["hrx"]["execution"] = old_ids["hrx_execution"]
    assert first.serialized(expected_evidence) == first.serialized(
        json.loads(evidence_path.read_bytes())
    ), "C9OnlyDisclosedIdentityEvidenceChanges"
    actual = dict(
        v1=capture(args.build / "lemond", args.build / "lemonade", REPO),
        v2=first.extra(args.build / "lemond"),
    )
    (args.out / "expected-snapshot.json").write_text(
        first.serialized(previous, True) + "\n"
    )
    (args.out / "actual-snapshot.json").write_text(
        first.serialized(actual, True) + "\n"
    )
    assert first.serialized(previous) == first.serialized(
        actual
    ), "C9ExactWholeSnapshot"
    subprocess.run(
        [
            "cmp",
            str(args.out / "expected-snapshot.json"),
            str(args.out / "actual-snapshot.json"),
        ],
        check=True,
        timeout=10,
    )
    for name in ("legacy", "schema", "digests", "etags"):
        (args.out / (name + ".diff")).write_bytes(b"")
    (args.out / "stages.json").write_text(first.serialized(stages, True) + "\n")

    baseline, current = census(args.base_build), census(args.build)
    assert len(baseline) == 165 and current == baseline | {
        "ContainerProductionParity_" + n for n in names
    } | {
        "ContainerProductionSelector_" + n for n in names if n != "ds4_rocm"
    }, "C9ExactCppCiCensusNoDrops"
    (args.out / "census.json").write_text(
        first.serialized(dict(baseline=sorted(baseline), current=sorted(current)), True)
        + "\n"
    )
    print(
        "Eighteen unchanged stages and",
        len(stages),
        "C9 stages replay; complete snapshot cmp passed",
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--base-build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--author", choices=ORDER)
    args = parser.parse_args()
    for field in ("build", "base_build", "out", "scratch"):
        setattr(args, field, getattr(args, field).resolve())
    run(args)
