"""Disclose the sealed diagnostics and rewrite domain as a new C7b stage."""

import copy
import json
import subprocess

import backend_v2_4e3b_transition as first

REPO = first.REPO
BASE = "957c3fc76ec8ddf35a589121761d69681a09a572"
SCHEMA = "schemas/backend-definition-v2.schema.json"
MANIFEST = REPO / "test/fixtures/backend-v2/c7b.catalog-transition.json"
RECORDS = [
    {
        "path": "/$defs/Diagnostic",
        "op": "add",
        "new": {
            "type": "object",
            "properties": {
                "id": {"$ref": "#/$defs/Identifier", "maxLength": 64},
                "phase": {
                    "enum": [
                        "loading",
                        "backend",
                        "artifacts",
                        "arguments",
                        "custom_args",
                        "starting",
                        "success",
                        "failure",
                    ]
                },
                "level": {"enum": ["debug", "info", "warning", "error"]},
                "category": {"enum": ["RocmFpx", "LlamaCpp", "Halogen", "DS4"]},
                "format": {
                    "enum": [
                        "Loading model: {model:string}",
                        "Per-model settings: {settings:options}",
                        "Using LlamaCpp Backend: {backend:string}",
                        "Using GGUF: {main:path}",
                        "Using backend: {backend:string}\n[LlamaCpp] Use GPU: {gpu:boolean}",
                        "Skipping mmproj argument since GPU mode is not enabled",
                        "Model supports embeddings, adding --embeddings flag",
                        "Model supports reranking, adding --reranking flag",
                        "Adding custom arguments: {custom:string}",
                        "Starting llama-server...",
                        "Starting {model:string} on port {port:integer}",
                        "Starting ds4-server ({image:string}) for {main:path} on port {port:integer}",
                        "Model loaded on port {loaded_port:integer}",
                    ],
                    "maxLength": 256,
                },
                "when": {
                    "enum": [
                        "always",
                        "main_present",
                        "custom_present",
                        "embedding",
                        "reranking",
                        "mmproj_cpu",
                    ]
                },
            },
            "required": ["id", "phase", "level", "category", "format"],
            "additionalProperties": False,
        },
    },
    {
        "path": "/$defs/Launch/properties/diagnostics",
        "op": "add",
        "new": {
            "type": "array",
            "maxItems": 32,
            "items": {"$ref": "#/$defs/Diagnostic"},
        },
    },
    {
        "path": "/$defs/Request/properties/rewrite_domain",
        "op": "add",
        "new": {"enum": ["canonical_path", "chat_completions_substring"]},
    },
]


def schema():
    old_bytes = subprocess.check_output(
        ["git", "show", BASE + ":" + SCHEMA], cwd=REPO, timeout=30
    )
    current_bytes = (REPO / SCHEMA).read_bytes()
    expected = first.apply(json.loads(old_bytes), RECORDS)
    assert first.serialized(expected) == first.serialized(
        json.loads(current_bytes)
    ), "ExactC7bSchema"
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
    }, "ExactC7bTransitionFields"
    assert (
        first.serialized(manifest["schema_version"]) == "1"
        and manifest["stage"] == "c7b-diagnostics-rewrite-domain"
        and manifest["base_commit"] == BASE
        and first.serialized(manifest["runner_service_policy"]) == '{"new":14,"old":13}'
    ), "ExactC7bTransitionProvenance"
    assert manifest["old_sha256"] == first.sha(old) and manifest[
        "old_schema_id"
    ] == first.schema_id(json.loads(old)), "PinnedC7bPreviousSchema"
    assert manifest["new_sha256"] == first.sha(current) and manifest[
        "new_schema_id"
    ] == first.schema_id(json.loads(current)), "DisclosedC7bSchemaIdentity"
    assert first.serialized(manifest["schema_changes"]) == first.serialized(
        RECORDS
    ), "ExactC7bSchemaRecords"
    assert first.serialized(manifest["previous_identities"]) == first.serialized(
        previous_ids
    ) and first.serialized(manifest["identities"]) == first.serialized(
        ids
    ), "ExactC7bStageIdentities"
    records = manifest["changes"]
    assert len({r["path"] for r in records}) == len(
        records
    ), "UniqueC7bTransitionRecords"
    assert first.serialized(records) == first.serialized(
        first.diff(previous, expected)
    ), "ExactC7bTransitionRecords"
    assert first.serialized(first.apply(previous, records)) == first.serialized(
        expected
    ), "WholeC7bTransitionReplay"


def check(args, previous, expected, previous_ids, ids):
    manifest = json.loads(MANIFEST.read_bytes())
    validate(manifest, previous, expected, previous_ids, ids)
    results = []
    for name, assertion, mutate in (
        (
            "missing",
            "ExactC7bTransitionRecords",
            lambda m: m["changes"].pop(),
        ),
        (
            "duplicate",
            "UniqueC7bTransitionRecords",
            lambda m: m["changes"].append(m["changes"][0]),
        ),
        (
            "policy",
            "ExactC7bTransitionProvenance",
            lambda m: m["runner_service_policy"].update(new=13),
        ),
        (
            "identity",
            "ExactC7bStageIdentities",
            lambda m: m["identities"].update(catalog="sha256:" + "0" * 64),
        ),
        (
            "schema-identity",
            "DisclosedC7bSchemaIdentity",
            lambda m: m.update(new_schema_id="sha256:" + "0" * 64),
        ),
        (
            "missing-schema-record",
            "ExactC7bSchemaRecords",
            lambda m: m["schema_changes"].pop(),
        ),
        (
            "schema-domain",
            "ExactC7bSchemaRecords",
            lambda m: m["schema_changes"][0]["new"].update(foreign=True),
        ),
    ):
        changed = copy.deepcopy(manifest)
        mutate(changed)
        try:
            validate(changed, previous, expected, previous_ids, ids)
        except AssertionError as error:
            assert str(error) == assertion, (name, str(error))
        else:
            raise AssertionError("UncaughtC7bDisclosureMutation:" + name)
        results.append(dict(rule=name, assertion=assertion, caught=True))
    (args.out / "c7b-transition-mutations.json").write_text(
        first.serialized(results, True) + "\n"
    )
    (args.out / "c7b-snapshot-transition.json").write_bytes(MANIFEST.read_bytes())
    print(
        "C7b diagnostics/rewrite transition:",
        len(manifest["changes"]),
        "snapshot records; runner policy 13 -> 14; three exact schema additions",
    )
