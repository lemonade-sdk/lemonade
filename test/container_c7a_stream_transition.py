"""Disclose the sealed builtin container streaming contract as a new C7a stage."""

import copy
import json
import subprocess

import backend_v2_4e3b_transition as first

REPO = first.REPO
BASE = "2985766b9368fe3dca5173d51536edfb3db3ed9b"
SCHEMA = "schemas/backend-definition-v2.schema.json"
MANIFEST = REPO / "test/fixtures/backend-v2/c7a.stream.catalog-transition.json"
RECORDS = [
    dict(
        op="replace",
        path="/$defs/Launch/properties/direct_stream_policy/enum",
        old=["strict", "llamacpp_legacy", "bound_passthrough"],
        new=[
            "strict",
            "llamacpp_legacy",
            "bound_passthrough",
            "builtin_container_legacy",
        ],
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
    ), "ExactC7aStreamSchema"
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
    }, "ExactC7aStreamTransitionFields"
    assert (
        first.serialized(manifest["schema_version"]) == "1"
        and manifest["stage"] == "c7a-builtin-container-stream"
        and manifest["base_commit"] == BASE
        and first.serialized(manifest["runner_service_policy"]) == '{"new":11,"old":10}'
    ), "ExactC7aStreamTransitionProvenance"
    assert manifest["old_sha256"] == first.sha(old) and manifest[
        "old_schema_id"
    ] == first.schema_id(json.loads(old)), "PinnedC7aStreamPreviousSchema"
    assert manifest["new_sha256"] == first.sha(current) and manifest[
        "new_schema_id"
    ] == first.schema_id(json.loads(current)), "DisclosedC7aStreamSchemaIdentity"
    assert first.serialized(manifest["schema_changes"]) == first.serialized(
        RECORDS
    ), "ExactC7aStreamSchemaRecords"
    assert first.serialized(manifest["previous_identities"]) == first.serialized(
        previous_ids
    ) and first.serialized(manifest["identities"]) == first.serialized(
        ids
    ), "ExactC7aStreamStageIdentities"
    records = manifest["changes"]
    assert len({r["path"] for r in records}) == len(
        records
    ), "UniqueC7aStreamTransitionRecords"
    assert first.serialized(records) == first.serialized(
        first.diff(previous, expected)
    ), "ExactC7aStreamTransitionRecords"
    assert first.serialized(first.apply(previous, records)) == first.serialized(
        expected
    ), "WholeC7aStreamTransitionReplay"


def check(args, previous, expected, previous_ids, ids):
    manifest = json.loads(MANIFEST.read_bytes())
    validate(manifest, previous, expected, previous_ids, ids)
    results = []
    for name, assertion, mutate in (
        (
            "missing",
            "ExactC7aStreamTransitionRecords",
            lambda m: m["changes"].pop(),
        ),
        (
            "duplicate",
            "UniqueC7aStreamTransitionRecords",
            lambda m: m["changes"].append(m["changes"][0]),
        ),
        (
            "policy",
            "ExactC7aStreamTransitionProvenance",
            lambda m: m["runner_service_policy"].update(new=10),
        ),
        (
            "identity",
            "ExactC7aStreamStageIdentities",
            lambda m: m["identities"].update(catalog="sha256:" + "0" * 64),
        ),
        (
            "schema-identity",
            "DisclosedC7aStreamSchemaIdentity",
            lambda m: m.update(new_schema_id="sha256:" + "0" * 64),
        ),
        (
            "missing-schema-record",
            "ExactC7aStreamSchemaRecords",
            lambda m: m["schema_changes"].pop(),
        ),
        (
            "schema-domain",
            "ExactC7aStreamSchemaRecords",
            lambda m: m["schema_changes"][0]["new"].append("auto"),
        ),
    ):
        changed = copy.deepcopy(manifest)
        mutate(changed)
        try:
            validate(changed, previous, expected, previous_ids, ids)
        except AssertionError as error:
            assert str(error) == assertion, (name, str(error))
        else:
            raise AssertionError("UncaughtC7aStreamDisclosureMutation:" + name)
        results.append(dict(rule=name, assertion=assertion, caught=True))
    (args.out / "c7a-stream-transition-mutations.json").write_text(
        first.serialized(results, True) + "\n"
    )
    (args.out / "c7a-stream-snapshot-transition.json").write_bytes(
        MANIFEST.read_bytes()
    )
    print(
        "C7a stream transition:",
        len(manifest["changes"]),
        "snapshot records; runner policy 10 -> 11; one exact enum extension",
    )
