"""Disclose the closed Halogen model and host contracts as its own C6 stage."""

import copy
import json
import subprocess

import backend_v2_4e3b_transition as first

REPO = first.REPO
BASE = "42ab4c00f49d01e5c0aae14cda44c0c43bb1753f"
SCHEMA = "schemas/backend-definition-v2.schema.json"
MANIFEST = REPO / "test/fixtures/backend-v2/c6b.halogen.catalog-transition.json"
RECORDS = [
    dict(
        op="add",
        path="/$defs/Declaration/properties/model_contract",
        new=dict(enum=["halogen_hgn_v1"]),
    ),
    dict(
        op="add",
        path="/$defs/EnvironmentSet/properties/when",
        new={"$ref": "#/$defs/Expression"},
    ),
    dict(op="add", path="/$defs/MergePayload/properties/model_contract", new={}),
    dict(
        op="replace",
        path="/$defs/Metadata/properties/slot_policy/enum",
        old=["standard", "exclusive_npu", "coexist_by_type", "unmetered"],
        new=[
            "standard",
            "exclusive_npu",
            "coexist_by_type",
            "unmetered",
            "exclusive_host",
        ],
    ),
]


def schema():
    old_bytes = subprocess.check_output(
        ["git", "show", BASE + ":" + SCHEMA], cwd=REPO, timeout=30
    )
    current_bytes = subprocess.check_output(
        ["git", "show", "b6b586059b71864370ae2c092bc0f9eb97970e1f:" + SCHEMA],
        cwd=REPO,
        timeout=30,
    )
    expected = first.apply(json.loads(old_bytes), RECORDS)
    assert first.serialized(expected) == first.serialized(
        json.loads(current_bytes)
    ), "ExactHalogenContractSchema"
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
    }, "ExactHalogenContractTransitionFields"
    assert (
        first.serialized(manifest["schema_version"]) == "1"
        and manifest["stage"] == "c6-container-halogen-contract"
        and manifest["base_commit"] == BASE
        and first.serialized(manifest["runner_service_policy"]) == '{"new":9,"old":8}'
    ), "ExactHalogenContractTransitionProvenance"
    assert manifest["old_sha256"] == first.sha(old) and manifest[
        "old_schema_id"
    ] == first.schema_id(json.loads(old)), "PinnedHalogenContractPreviousSchema"
    assert manifest["new_sha256"] == first.sha(current) and manifest[
        "new_schema_id"
    ] == first.schema_id(json.loads(current)), "DisclosedHalogenContractSchemaIdentity"
    assert first.serialized(manifest["schema_changes"]) == first.serialized(
        RECORDS
    ), "ExactHalogenContractSchemaRecords"
    assert first.serialized(manifest["previous_identities"]) == first.serialized(
        previous_ids
    ) and first.serialized(manifest["identities"]) == first.serialized(
        ids
    ), "ExactHalogenContractStageIdentities"
    records = manifest["changes"]
    assert len({r["path"] for r in records}) == len(
        records
    ), "UniqueHalogenContractTransitionRecords"
    assert first.serialized(records) == first.serialized(
        first.diff(previous, expected)
    ), "ExactHalogenContractTransitionRecords"
    assert first.serialized(first.apply(previous, records)) == first.serialized(
        expected
    ), "WholeHalogenContractTransitionReplay"


def check(args, previous, expected, previous_ids, ids):
    manifest = json.loads(MANIFEST.read_bytes())
    validate(manifest, previous, expected, previous_ids, ids)
    results = []
    for name, assertion, mutate in (
        (
            "missing",
            "ExactHalogenContractTransitionRecords",
            lambda m: m["changes"].pop(),
        ),
        (
            "duplicate",
            "UniqueHalogenContractTransitionRecords",
            lambda m: m["changes"].append(m["changes"][0]),
        ),
        (
            "policy",
            "ExactHalogenContractTransitionProvenance",
            lambda m: m["runner_service_policy"].update(new=8),
        ),
        (
            "identity",
            "ExactHalogenContractStageIdentities",
            lambda m: m["identities"].update(catalog="sha256:" + "0" * 64),
        ),
        (
            "schema-identity",
            "DisclosedHalogenContractSchemaIdentity",
            lambda m: m.update(new_schema_id="sha256:" + "0" * 64),
        ),
        (
            "missing-schema-record",
            "ExactHalogenContractSchemaRecords",
            lambda m: m["schema_changes"].pop(),
        ),
        (
            "schema-domain",
            "ExactHalogenContractSchemaRecords",
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
            raise AssertionError("UncaughtHalogenContractDisclosureMutation:" + name)
        results.append(dict(rule=name, assertion=assertion, caught=True))
    (args.out / "c6-halogen-contract-transition-mutations.json").write_text(
        first.serialized(results, True) + "\n"
    )
    (args.out / "c6-halogen-contract-snapshot-transition.json").write_bytes(
        MANIFEST.read_bytes()
    )
    print(
        "C6 Halogen contract transition:",
        len(manifest["changes"]),
        "snapshot records; runner policy 8 -> 9; four exact schema changes",
    )
