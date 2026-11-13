"""Replay immutable container history, then disclose each inactive C8 inventory stage."""

import argparse
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile

import backend_v2_4e3b_transition as first
import container_definition_transition as history
from backend_v1_snapshot import capture

REPO = first.REPO
BASE = "3af47274a644571d29b5af9b6b2b780c04864371"
ORDER = ["rocmfpx", "halogen"]


def pinned(path):
    return subprocess.check_output(
        ["git", "show", BASE + ":" + path], cwd=REPO, timeout=30
    )


def context(evidence, names, pins):
    defaults = json.loads(pinned("src/cpp/resources/defaults.json"))
    ids = set(first.RECIPES) | set(evidence["v1_ids"]) | {"gufo"} | set(names)
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
        "previous_identities",
        "identities",
        "changes",
    }, "C8ExactTransitionFields"
    assert (
        manifest["schema_version"] == 1
        and manifest["active"] is False
        and manifest["stage"] == name + "-inactive-inventory"
        and manifest["base_commit"] == BASE
    ), "C8ExactTransitionProvenance"
    assert manifest["source_sha256"] == source_sha, "C8PinnedSourceBytes"
    assert first.serialized(manifest["previous_identities"]) == first.serialized(
        old_ids
    ) and first.serialized(manifest["identities"]) == first.serialized(
        ids
    ), "C8ExactStageIdentities"
    assert len({r["path"] for r in manifest["changes"]}) == len(
        manifest["changes"]
    ), "C8UniqueTransitionRecords"
    assert first.serialized(manifest["changes"]) == first.serialized(
        first.diff(previous, expected)
    ), "C8ExactTransitionRecords"
    assert first.serialized(
        first.apply(previous, manifest["changes"])
    ) == first.serialized(expected), "C8WholeTransitionReplay"


def run(args):
    for path in (args.out, args.scratch, args.scratch / "temporary"):
        path.mkdir(parents=True, exist_ok=True)
    os.environ.update(
        TMPDIR=str(args.scratch / "temporary"),
        TMP=str(args.scratch / "temporary"),
        TEMP=str(args.scratch / "temporary"),
    )
    tempfile.tempdir = str(args.scratch / "temporary")
    tree = args.scratch / "base-source"
    if not tree.exists():
        tree.mkdir()
        archive = subprocess.check_output(
            ["git", "archive", BASE], cwd=REPO, timeout=60
        )
        with tarfile.open(fileobj=io.BytesIO(archive)) as source:
            source.extractall(tree, filter="data")
        (tree / ".synagent").symlink_to(REPO / ".synagent", target_is_directory=True)
    prior = args.out / "prior"
    prior.mkdir(exist_ok=True)
    replay = "import sys,runpy; from pathlib import Path; sys.path.insert(0,sys.argv[1]); import llamacpp_frozen_control as f; f.ROOT=Path(sys.argv[2]); sys.argv=sys.argv[3:]; runpy.run_path(sys.argv[0],run_name='__main__')"
    replay_prefix = [
        "python3",
        "-c",
        replay,
        str(tree / "test"),
        str(REPO / ".synagent/scratch/backend-v2-4e3b/frozen-control"),
    ]
    replay_env = dict(os.environ, GIT_DIR=str(REPO / ".git"))
    if not (prior / "actual-snapshot.json").exists():
        subprocess.run(
            [
                "timeout",
                "180",
                *replay_prefix,
                str(tree / "test/container_definition_transition.py"),
                "run",
                "--build",
                str(args.base_build),
                "--out",
                str(prior),
                "--scratch",
                str(args.scratch / "prior"),
            ],
            cwd=tree,
            env=replay_env,
            check=True,
            timeout=190,
        )
    else:
        subprocess.run(
            [
                "timeout",
                "180",
                *replay_prefix,
                str(tree / "test/container_definition_transition.py"),
                "check",
                "--build",
                str(args.base_build),
                "--out",
                str(prior),
                "--scratch",
                str(args.scratch / "prior"),
            ],
            cwd=tree,
            env=replay_env,
            check=True,
            timeout=190,
        )
    previous = json.loads((prior / "actual-snapshot.json").read_bytes())
    base = copy.deepcopy(previous)
    old_ids = json.loads(
        pinned("test/fixtures/backend-v2/c7b.catalog-transition.json")
    )["identities"]
    evidence = json.loads((prior / "identities.json").read_bytes())
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
            runner_service_policy=14,
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
            REPO / f"test/fixtures/backend-v2/c8.{name}.catalog-transition.json"
        )
        manifest = dict(
            schema_version=1,
            stage=name + "-inactive-inventory",
            base_commit=BASE,
            active=False,
            source_sha256=first.sha(source.read_bytes()),
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
                raise AssertionError("C8UncaughtDisclosureMutation:" + mutation)
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
    ), "C8ExactPinAdditions"
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
    assert result.returncode == 0, "C8ActualProductionSwitchSuite"
    expected_evidence = copy.deepcopy(evidence)
    for field in ("admission", "execution", "catalog", "catalog_v2"):
        expected_evidence[field] = old_ids[field]
    expected_evidence["hrx"]["execution"] = old_ids["hrx_execution"]
    assert first.serialized(expected_evidence) == first.serialized(
        json.loads(evidence_path.read_bytes())
    ), "C8OnlyDisclosedIdentityEvidenceChanges"
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
    ), "C8ExactWholeSnapshot"
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

    def census(build):
        data = json.loads(
            subprocess.check_output(
                ["ctest", "--test-dir", str(build), "--show-only=json-v1"], timeout=30
            )
        )
        return {
            t["name"]
            for t in data["tests"]
            if any(
                p["name"] == "LABELS" and "cpp-ci" in p["value"]
                for p in t["properties"]
            )
        }

    baseline, current = census(args.base_build), census(args.build)
    assert len(baseline) == 158 and current == baseline | {
        "ContainerProductionParity_" + n for n in names
    } | {
        "ContainerProductionBlocked_" + p.name.split(".")[1]
        for p in (REPO / "test/fixtures/backend-v2").glob("c8.*.blocked.json")
    }, "C8ExactCppCiCensusNoDrops"
    (args.out / "census.json").write_text(
        first.serialized(dict(baseline=sorted(baseline), current=sorted(current)), True)
        + "\n"
    )
    print(
        "Fourteen unchanged stages and",
        len(stages),
        "C8 stages replay; complete snapshot cmp passed",
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
