"""Disclose the closed DS4 memory and content contracts as its own C6 stage."""

import copy
import json
import subprocess

import backend_v2_4e3b_transition as first

REPO = first.REPO
BASE = "b6b586059b71864370ae2c092bc0f9eb97970e1f"
SCHEMA = "schemas/backend-definition-v2.schema.json"
MANIFEST = REPO / "test/fixtures/backend-v2/c6b.ds4.catalog-transition.json"
RECORDS = [
    dict(
        op="add",
        path="/$defs/ExecutionCase/oneOf/1/properties/memory_plan",
        new=dict(enum=["ds4_rocm_v1"]),
    ),
    dict(
        op="add",
        path="/$defs/Request/properties/content_transform",
        new=dict(enum=["ds4_no_think_v1"]),
    ),
]


def schema():
    old_bytes = subprocess.check_output(
        ["git", "show", BASE + ":" + SCHEMA], cwd=REPO, timeout=30
    )
    current_bytes = (REPO / SCHEMA).read_bytes()
    expected = first.apply(json.loads(old_bytes), RECORDS)
    assert first.serialized(expected) == first.serialized(
        json.loads(current_bytes)
    ), "ExactDs4ContractSchema"
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
    }, "ExactDs4ContractTransitionFields"
    assert (
        first.serialized(manifest["schema_version"]) == "1"
        and manifest["stage"] == "c6-container-ds4-memory-content"
        and manifest["base_commit"] == BASE
        and first.serialized(manifest["runner_service_policy"]) == '{"new":10,"old":9}'
    ), "ExactDs4ContractTransitionProvenance"
    assert manifest["old_sha256"] == first.sha(old) and manifest[
        "old_schema_id"
    ] == first.schema_id(json.loads(old)), "PinnedDs4ContractPreviousSchema"
    assert manifest["new_sha256"] == first.sha(current) and manifest[
        "new_schema_id"
    ] == first.schema_id(json.loads(current)), "DisclosedDs4ContractSchemaIdentity"
    assert first.serialized(manifest["schema_changes"]) == first.serialized(
        RECORDS
    ), "ExactDs4ContractSchemaRecords"
    assert first.serialized(manifest["previous_identities"]) == first.serialized(
        previous_ids
    ) and first.serialized(manifest["identities"]) == first.serialized(
        ids
    ), "ExactDs4ContractStageIdentities"
    records = manifest["changes"]
    assert len({r["path"] for r in records}) == len(
        records
    ), "UniqueDs4ContractTransitionRecords"
    assert first.serialized(records) == first.serialized(
        first.diff(previous, expected)
    ), "ExactDs4ContractTransitionRecords"
    assert first.serialized(first.apply(previous, records)) == first.serialized(
        expected
    ), "WholeDs4ContractTransitionReplay"


def check(args, previous, expected, previous_ids, ids):
    manifest = json.loads(MANIFEST.read_bytes())
    validate(manifest, previous, expected, previous_ids, ids)
    results = []
    for name, assertion, mutate in (
        (
            "missing",
            "ExactDs4ContractTransitionRecords",
            lambda m: m["changes"].pop(),
        ),
        (
            "duplicate",
            "UniqueDs4ContractTransitionRecords",
            lambda m: m["changes"].append(m["changes"][0]),
        ),
        (
            "policy",
            "ExactDs4ContractTransitionProvenance",
            lambda m: m["runner_service_policy"].update(new=9),
        ),
        (
            "identity",
            "ExactDs4ContractStageIdentities",
            lambda m: m["identities"].update(catalog="sha256:" + "0" * 64),
        ),
        (
            "schema-identity",
            "DisclosedDs4ContractSchemaIdentity",
            lambda m: m.update(new_schema_id="sha256:" + "0" * 64),
        ),
        (
            "missing-schema-record",
            "ExactDs4ContractSchemaRecords",
            lambda m: m["schema_changes"].pop(),
        ),
        (
            "schema-domain",
            "ExactDs4ContractSchemaRecords",
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
            raise AssertionError("UncaughtDs4ContractDisclosureMutation:" + name)
        results.append(dict(rule=name, assertion=assertion, caught=True))
    (args.out / "c6-ds4-memory-content-transition-mutations.json").write_text(
        first.serialized(results, True) + "\n"
    )
    (args.out / "c6-ds4-memory-content-snapshot-transition.json").write_bytes(
        MANIFEST.read_bytes()
    )
    print(
        "C6 DS4 memory and content transition:",
        len(manifest["changes"]),
        "snapshot records; runner policy 9 -> 10; two exact schema additions",
    )
