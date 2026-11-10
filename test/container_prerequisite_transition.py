"""Disclose additive C6 contracts after replaying every immutable container stage."""

import copy
import json
import subprocess

import backend_v2_4e3b_transition as first
import gufo_definition_transition as gufo

REPO = first.REPO
BASE = "af5adc3f55c5c9c97ad46c087ff784241c3fed37"
SCHEMA = "schemas/backend-definition-v2.schema.json"
RAW_MANIFEST = REPO / "test/fixtures/backend-v2/c6.raw-labels.catalog-transition.json"


def derive(base, evidence, old_ids, schema, expected_snapshot, policy):
    def snapshot(*args):
        return expected_snapshot(*args, runner_service_policy=policy)

    return gufo.derive(base, evidence, old_ids, schema, snapshot)


def validate(manifest, previous, expected, previous_ids, ids):
    assert set(manifest) == {
        "schema_version",
        "stage",
        "base_commit",
        "runner_service_policy",
        "schema_sha256",
        "raw_label_domain",
        "previous_identities",
        "identities",
        "changes",
    }, "ExactRawLabelTransitionFields"
    assert (
        first.serialized(manifest["schema_version"]) == "1"
        and manifest["stage"] == "c6-raw-label-facts"
        and manifest["base_commit"] == BASE
        and first.serialized(manifest["runner_service_policy"]) == '{"new":7,"old":6}'
        and manifest["raw_label_domain"]
        == ["/model/labels/dflash", "/model/labels/mtp"]
    ), "ExactRawLabelTransitionProvenance"
    assert manifest["schema_sha256"] == first.sha(
        subprocess.check_output(
            ["git", "show", BASE + ":" + SCHEMA], cwd=REPO, timeout=30
        )
    ), "PinnedRawLabelSchemaBytes"
    assert first.serialized(manifest["previous_identities"]) == first.serialized(
        previous_ids
    ) and first.serialized(manifest["identities"]) == first.serialized(
        ids
    ), "ExactRawLabelStageIdentities"
    records = manifest["changes"]
    assert len({r["path"] for r in records}) == len(
        records
    ), "UniqueRawLabelTransitionRecords"
    assert first.serialized(records) == first.serialized(
        first.diff(previous, expected)
    ), "ExactRawLabelTransitionRecords"
    assert first.serialized(first.apply(previous, records)) == first.serialized(
        expected
    ), "WholeRawLabelTransitionReplay"


def check(args, previous, expected, previous_ids, ids):
    manifest = json.loads(RAW_MANIFEST.read_bytes())
    validate(manifest, previous, expected, previous_ids, ids)
    results = []
    for name, assertion, mutate in (
        ("missing", "ExactRawLabelTransitionRecords", lambda m: m["changes"].pop()),
        (
            "duplicate",
            "UniqueRawLabelTransitionRecords",
            lambda m: m["changes"].append(m["changes"][0]),
        ),
        (
            "policy",
            "ExactRawLabelTransitionProvenance",
            lambda m: m["runner_service_policy"].update(new=6),
        ),
        (
            "identity",
            "ExactRawLabelStageIdentities",
            lambda m: m["identities"].update(catalog="sha256:" + "0" * 64),
        ),
        (
            "foreign",
            "ExactRawLabelTransitionRecords",
            lambda m: m["changes"].append(dict(op="add", path="/foreign", new=True)),
        ),
    ):
        changed = copy.deepcopy(manifest)
        mutate(changed)
        try:
            validate(changed, previous, expected, previous_ids, ids)
        except AssertionError as error:
            assert str(error) == assertion, (name, str(error))
        else:
            raise AssertionError("UncaughtRawLabelDisclosureMutation:" + name)
        results.append(dict(rule=name, assertion=assertion, caught=True))
    (args.out / "c6-raw-label-transition-mutations.json").write_text(
        first.serialized(results, True) + "\n"
    )
    (args.out / "c6-raw-label-snapshot-transition.json").write_bytes(
        RAW_MANIFEST.read_bytes()
    )
    print(
        "C6 raw label transition:",
        len(manifest["changes"]),
        "snapshot records; runner policy 6 -> 7",
    )
