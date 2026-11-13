"""Replay sixteen immutable stages before independently disclosing C8a contracts."""

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
from container_c8a_control import BASE
from container_c8a_sources import check as check_sources

REPO = first.REPO
SCHEMA = "schemas/backend-definition-v2.schema.json"
STAGES = [
    (
        "pre-install-refusal",
        15,
        [
            {
                "op": "add",
                "path": "/$defs/Launch/properties/pre_install_refusal",
                "new": {"enum": ["container_hf_load_v1"]},
            }
        ],
    ),
    (
        "absolute-checkpoint-consumer",
        16,
        [
            {
                "op": "add",
                "path": "/$defs/Artifact/properties/checkpoint_consumer",
                "new": {"enum": ["builtin_container_absolute_v1"]},
            }
        ],
    ),
]


def pinned(path):
    return subprocess.check_output(
        ["git", "show", BASE + ":" + path], cwd=REPO, timeout=30
    )


def validate(manifest, expected):
    assert len({r["path"] for r in manifest["changes"]}) == len(
        manifest["changes"]
    ), "C8aUniqueTransitionRecords"
    assert first.serialized(manifest) == first.serialized(
        expected
    ), "C8aExactIndependentDisclosure"


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
    source = args.base_build.parent / "source"
    link = source / ".synagent"
    if not link.exists():
        link.symlink_to(REPO / ".synagent", target_is_directory=True)
    git_link = source / ".git"
    if not git_link.exists():
        git_link.symlink_to(REPO / ".git", target_is_directory=True)
    prior = args.out / "prior"
    prior.mkdir(exist_ok=True)
    subprocess.run(
        [
            "timeout",
            "240",
            "python3",
            "-c",
            "import runpy,sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); import container_production_transition as c; c.REPO=Path(sys.argv[2]); sys.argv=sys.argv[3:]; args=type('Args',(),dict(build=Path(sys.argv[1]),base_build=Path(sys.argv[2]),out=Path(sys.argv[3]),scratch=Path(sys.argv[4]),author=None))(); c.run(args)",
            str(source / "test"),
            str(REPO),
            "c8-replay",
            str(args.base_build),
            str(REPO / ".synagent/scratch/container-v2/c8/base-control/build"),
            str(prior),
            str(args.scratch / "prior"),
        ],
        cwd=source,
        env=dict(os.environ, GIT_DIR=str(REPO / ".git")),
        check=True,
        timeout=250,
    )
    base = json.loads((prior / "actual-snapshot.json").read_bytes())
    previous = copy.deepcopy(base)
    evidence = json.loads((prior / "current-identities.json").read_bytes())
    old_ids = json.loads(
        pinned("test/fixtures/backend-v2/c8.halogen.catalog-transition.json")
    )["identities"]
    identity_inputs = json.loads(
        pinned("test/fixtures/backend-v2/exports.catalog-transition.json")
    )["identities"]
    identity_inputs.update(old_ids)
    old_schema = pinned(SCHEMA)
    schema = json.loads(old_schema)
    stages = []
    for name, policy, records in STAGES:
        schema = first.apply(schema, records)
        new_schema = (json.dumps(schema, indent=2) + "\n").encode()
        expected, ids = history.expected_snapshot(
            base, evidence, identity_inputs, schema, runner_service_policy=policy
        )
        disclosure = dict(
            schema_version=1,
            stage="c8a-" + name,
            base_commit=BASE,
            runner_service_policy=dict(old=policy - 1, new=policy),
            old_sha256=first.sha(old_schema),
            new_sha256=first.sha(new_schema),
            schema_changes=records,
            previous_identities=old_ids,
            identities=ids,
            changes=first.diff(previous, expected),
        )
        path = REPO / f"test/fixtures/backend-v2/c8a.{name}.catalog-transition.json"
        if args.author == name and not path.exists():
            path.write_text(first.serialized(disclosure, True) + "\n")
        manifest = json.loads(path.read_bytes())
        validate(manifest, disclosure)
        for mutation in ("missing", "duplicate", "identity", "policy", "schema"):
            changed = copy.deepcopy(manifest)
            if mutation == "missing":
                changed["changes"].pop()
            elif mutation == "duplicate":
                changed["changes"].append(changed["changes"][0])
            elif mutation == "identity":
                changed["identities"]["catalog"] = "sha256:" + "0" * 64
            elif mutation == "policy":
                changed["runner_service_policy"]["new"] -= 1
            else:
                changed["schema_changes"].pop()
            try:
                validate(changed, disclosure)
            except AssertionError:
                pass
            else:
                raise AssertionError("C8aUncaughtDisclosureMutation:" + mutation)
        assert first.serialized(
            first.apply(previous, manifest["changes"])
        ) == first.serialized(expected), "C8aWholeStageReplay"
        stages.append(dict(stage=manifest["stage"], records=len(manifest["changes"])))
        previous, old_ids, old_schema = expected, ids, new_schema
    assert (REPO / SCHEMA).read_bytes() == old_schema, "C8aExactSchemaAdditions"
    current_evidence = args.out / "current-identities.json"
    subprocess.run(
        ["timeout", "120", str(args.build / "test_llamacpp_v2_production_switch")],
        env=dict(os.environ, LEMON_SWITCH_OUT=str(current_evidence)),
        check=True,
        timeout=130,
    )
    expected_evidence = copy.deepcopy(evidence)
    for field in ("admission", "execution", "catalog", "catalog_v2"):
        expected_evidence[field] = old_ids[field]
    expected_evidence["hrx"]["execution"] = old_ids["hrx_execution"]
    assert first.serialized(expected_evidence) == first.serialized(
        json.loads(current_evidence.read_bytes())
    ), "C8aOnlyDisclosedIdentities"
    actual = dict(
        v1=capture(args.build / "lemond", args.build / "lemonade", REPO),
        v2=first.extra(args.build / "lemond"),
    )
    for name, value in (("expected", previous), ("actual", actual)):
        (args.out / (name + "-snapshot.json")).write_text(
            first.serialized(value, True) + "\n"
        )
    subprocess.run(
        ["cmp", args.out / "expected-snapshot.json", args.out / "actual-snapshot.json"],
        check=True,
        timeout=10,
    )
    for name in ("legacy", "schema", "digests", "etags"):
        (args.out / (name + ".diff")).write_bytes(b"")
    baseline, current = map(census, (args.base_build, args.build))
    additions = {"ContainerC8aPreInstallTest", "ContainerC8aCheckpointTest"}
    assert (
        len(baseline) == 163 and current == baseline | additions
    ), "C8aExactCensusNoDrops"
    (args.out / "census.json").write_text(
        first.serialized(dict(baseline=sorted(baseline), current=sorted(current)), True)
        + "\n"
    )
    (args.out / "stages.json").write_text(first.serialized(stages, True) + "\n")
    print("Sixteen unchanged stages plus C8a:", stages, "; whole snapshot cmp passed")


def census(build):
    data = json.loads(
        subprocess.check_output(
            ["ctest", "--test-dir", build, "--show-only=json-v1"], timeout=30
        )
    )
    return {
        t["name"]
        for t in data["tests"]
        if any(
            p["name"] == "LABELS" and "cpp-ci" in p["value"] for p in t["properties"]
        )
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("build", "base-build", "out", "scratch"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--author", choices=[stage[0] for stage in STAGES])
    args = parser.parse_args()
    for field in ("build", "base_build", "out", "scratch"):
        setattr(args, field, getattr(args, field).resolve())
    run(args)
