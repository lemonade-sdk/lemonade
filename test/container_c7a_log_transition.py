"""Disclose the sealed v2 load-log binding as a new C7a stage."""

import copy
import json
import subprocess

import backend_v2_4e3b_transition as first

REPO = first.REPO
BASE = "265b1cc5627538af2629e50a999489ee0778b596"
SCHEMA = "schemas/backend-definition-v2.schema.json"
MANIFEST = REPO / "test/fixtures/backend-v2/c7a.log.catalog-transition.json"
RECORDS = [
    dict(
        op="add",
        path="/$defs/Launch/properties/builtin_load_log",
        new=dict(enum=["halogen_starting_v1"]),
    )
]


def schema():
    old_bytes = subprocess.check_output(
        ["git", "show", BASE + ":" + SCHEMA], cwd=REPO, timeout=30
    )
    current_bytes = (REPO / SCHEMA).read_bytes()
    expected = first.apply(json.loads(old_bytes), RECORDS)
    assert first.serialized(expected) == first.serialized(
        json.loads(current_bytes)
    ), "ExactC7aLogSchema"
    return old_bytes, current_bytes, expected


def validate(manifest, previous, expected, previous_ids, ids):
    old, current, _ = schema()
    assert set(manifest) == {
        "schema_version",
        "stage",
        "base_commit",
        "runner_service_policy",
        "old_sha256",
        "new_sha256",
        "old_schema_id",
        "new_schema_id",
        "schema_changes",
        "previous_identities",
        "identities",
        "changes",
    }, "ExactC7aLogTransitionFields"
    assert (
        first.serialized(manifest["schema_version"]) == "1"
        and manifest["stage"] == "c7a-builtin-load-log"
        and manifest["base_commit"] == BASE
        and first.serialized(manifest["runner_service_policy"]) == '{"new":12,"old":11}'
    ), "ExactC7aLogTransitionProvenance"
    assert manifest["old_sha256"] == first.sha(old) and manifest[
        "old_schema_id"
    ] == first.schema_id(json.loads(old)), "PinnedC7aLogPreviousSchema"
    assert manifest["new_sha256"] == first.sha(current) and manifest[
        "new_schema_id"
    ] == first.schema_id(json.loads(current)), "DisclosedC7aLogSchemaIdentity"
    assert first.serialized(manifest["schema_changes"]) == first.serialized(
        RECORDS
    ), "ExactC7aLogSchemaRecords"
    assert first.serialized(manifest["previous_identities"]) == first.serialized(
        previous_ids
    ) and first.serialized(manifest["identities"]) == first.serialized(
        ids
    ), "ExactC7aLogStageIdentities"
    records = manifest["changes"]
    assert len({r["path"] for r in records}) == len(
        records
    ), "UniqueC7aLogTransitionRecords"
    assert first.serialized(records) == first.serialized(
        first.diff(previous, expected)
    ), "ExactC7aLogTransitionRecords"
    assert first.serialized(first.apply(previous, records)) == first.serialized(
        expected
    ), "WholeC7aLogTransitionReplay"


def check(args, previous, expected, previous_ids, ids):
    manifest = json.loads(MANIFEST.read_bytes())
    validate(manifest, previous, expected, previous_ids, ids)
    results = []
    for name, assertion, mutate in (
        (
            "missing",
            "ExactC7aLogTransitionRecords",
            lambda m: m["changes"].pop(),
        ),
        (
            "duplicate",
            "UniqueC7aLogTransitionRecords",
            lambda m: m["changes"].append(m["changes"][0]),
        ),
        (
            "policy",
            "ExactC7aLogTransitionProvenance",
            lambda m: m["runner_service_policy"].update(new=11),
        ),
        (
            "identity",
            "ExactC7aLogStageIdentities",
            lambda m: m["identities"].update(catalog="sha256:" + "0" * 64),
        ),
        (
            "schema-identity",
            "DisclosedC7aLogSchemaIdentity",
            lambda m: m.update(new_schema_id="sha256:" + "0" * 64),
        ),
        (
            "missing-schema-record",
            "ExactC7aLogSchemaRecords",
            lambda m: m["schema_changes"].pop(),
        ),
        (
            "schema-domain",
            "ExactC7aLogSchemaRecords",
            lambda m: m["schema_changes"][0]["new"]["enum"].append("auto"),
        ),
    ):
        changed = copy.deepcopy(manifest)
        mutate(changed)
        try:
            validate(changed, previous, expected, previous_ids, ids)
        except AssertionError as error:
            assert str(error) == assertion, (name, str(error))
        else:
            raise AssertionError("UncaughtC7aLogDisclosureMutation:" + name)
        results.append(dict(rule=name, assertion=assertion, caught=True))
    (args.out / "c7a-log-transition-mutations.json").write_text(
        first.serialized(results, True) + "\n"
    )
    (args.out / "c7a-log-snapshot-transition.json").write_bytes(MANIFEST.read_bytes())
    print(
        "C7a load-log transition:",
        len(manifest["changes"]),
        "snapshot records; runner policy 11 -> 12; one exact binding",
    )
