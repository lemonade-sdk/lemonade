"""Disclose the closed container device API record as its own C6 stage."""

import copy
import json
import subprocess

import backend_v2_4e3b_transition as first

REPO = first.REPO
BASE = "02b2573b168911db95e2f79227e1263f5bb2dbd4"
SCHEMA = "schemas/backend-definition-v2.schema.json"
MANIFEST = (
    REPO / "test/fixtures/backend-v2/c6.device-validation.catalog-transition.json"
)
RECORD = dict(
    op="add",
    path="/$defs/ExecutionCase/oneOf/1/properties/device_validation",
    new=dict(
        type="object",
        properties=dict(backend=dict(enum=["rocm", "vulkan"])),
        required=["backend"],
        additionalProperties=False,
    ),
)


def schema():
    old_bytes = subprocess.check_output(
        ["git", "show", BASE + ":" + SCHEMA], cwd=REPO, timeout=30
    )
    current_bytes = (REPO / SCHEMA).read_bytes()
    expected = first.apply(json.loads(old_bytes), [RECORD])
    assert first.serialized(expected) == first.serialized(
        json.loads(current_bytes)
    ), "ExactDeviceValidationSchema"
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
    }, "ExactDeviceValidationTransitionFields"
    assert (
        first.serialized(manifest["schema_version"]) == "1"
        and manifest["stage"] == "c6-container-device-validation"
        and manifest["base_commit"] == BASE
        and first.serialized(manifest["runner_service_policy"]) == '{"new":8,"old":7}'
    ), "ExactDeviceValidationTransitionProvenance"
    assert manifest["old_sha256"] == first.sha(old) and manifest[
        "old_schema_id"
    ] == first.schema_id(json.loads(old)), "PinnedDeviceValidationPreviousSchema"
    assert manifest["new_sha256"] == first.sha(current) and manifest[
        "new_schema_id"
    ] == first.schema_id(json.loads(current)), "DisclosedDeviceValidationSchemaIdentity"
    assert first.serialized(manifest["schema_changes"]) == first.serialized(
        [RECORD]
    ), "ExactDeviceValidationSchemaRecords"
    assert first.serialized(manifest["previous_identities"]) == first.serialized(
        previous_ids
    ) and first.serialized(manifest["identities"]) == first.serialized(
        ids
    ), "ExactDeviceValidationStageIdentities"
    records = manifest["changes"]
    assert len({r["path"] for r in records}) == len(
        records
    ), "UniqueDeviceValidationTransitionRecords"
    assert first.serialized(records) == first.serialized(
        first.diff(previous, expected)
    ), "ExactDeviceValidationTransitionRecords"
    assert first.serialized(first.apply(previous, records)) == first.serialized(
        expected
    ), "WholeDeviceValidationTransitionReplay"


def check(args, previous, expected, previous_ids, ids):
    manifest = json.loads(MANIFEST.read_bytes())
    validate(manifest, previous, expected, previous_ids, ids)
    results = []
    for name, assertion, mutate in (
        (
            "missing",
            "ExactDeviceValidationTransitionRecords",
            lambda m: m["changes"].pop(),
        ),
        (
            "duplicate",
            "UniqueDeviceValidationTransitionRecords",
            lambda m: m["changes"].append(m["changes"][0]),
        ),
        (
            "policy",
            "ExactDeviceValidationTransitionProvenance",
            lambda m: m["runner_service_policy"].update(new=7),
        ),
        (
            "identity",
            "ExactDeviceValidationStageIdentities",
            lambda m: m["identities"].update(catalog="sha256:" + "0" * 64),
        ),
        (
            "schema-identity",
            "DisclosedDeviceValidationSchemaIdentity",
            lambda m: m.update(new_schema_id="sha256:" + "0" * 64),
        ),
        (
            "missing-schema-record",
            "ExactDeviceValidationSchemaRecords",
            lambda m: m["schema_changes"].pop(),
        ),
        (
            "schema-domain",
            "ExactDeviceValidationSchemaRecords",
            lambda m: m["schema_changes"][0]["new"]["properties"]["backend"][
                "enum"
            ].append("auto"),
        ),
    ):
        changed = copy.deepcopy(manifest)
        mutate(changed)
        try:
            validate(changed, previous, expected, previous_ids, ids)
        except AssertionError as error:
            assert str(error) == assertion, (name, str(error))
        else:
            raise AssertionError("UncaughtDeviceValidationDisclosureMutation:" + name)
        results.append(dict(rule=name, assertion=assertion, caught=True))
    (args.out / "c6-device-validation-transition-mutations.json").write_text(
        first.serialized(results, True) + "\n"
    )
    (args.out / "c6-device-validation-snapshot-transition.json").write_bytes(
        MANIFEST.read_bytes()
    )
    print(
        "C6 device validation transition:",
        len(manifest["changes"]),
        "snapshot records; runner policy 7 -> 8; one schema addition",
    )
