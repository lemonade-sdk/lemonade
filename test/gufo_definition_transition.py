"""Derive the dormant Gufo inventory stage after the unchanged C4a replay."""

import copy
import json
from pathlib import Path
import subprocess

import backend_v2_4e3b_transition as first

REPO = Path(__file__).resolve().parents[1]
BASE = "e1552e17a403ebd6d358400cf921ce3145f100b7"
SOURCE = "src/cpp/resources/backends/gufo.json"
MANIFEST = REPO / "test/fixtures/backend-v2/gufo.catalog-transition.json"


def derive(base, evidence, old_ids, schema, expected_snapshot):
    defaults = json.loads((REPO / "src/cpp/resources/defaults.json").read_bytes())
    names = set(first.RECIPES) | set(evidence["v1_ids"]) | {"gufo"}
    sections = set(defaults) | {"no_broadcast"}
    options = sections | set(evidence["common_defaults"])
    persisted = {}
    for row in evidence["descriptors"]:
        section = row["config_section"] or row["recipe"]
        names.add(section)
        sections.add(section)
        options.update(o["name"] for o in row["options"])
        if row["options"]:
            persisted[section] = sorted({first.key(o["name"]) for o in row["options"]})
    for pair in evidence["spec_aliases"]:
        names.update(x for x in pair if x)
    pin_path = "src/cpp/resources/backend_versions.json"
    old_pins = json.loads(
        subprocess.check_output(
            ["git", "show", BASE + ":" + pin_path], cwd=REPO, timeout=30
        )
    )
    pins = copy.deepcopy(old_pins)
    pins["gufo"] = {
        "rocm": "rocm-10.0-gufo@sha256:67aa0b040993fedfd4e300242c5cb48f8add5874454a7486993668e74185e7a9"
    }
    assert first.serialized(pins) == first.serialized(
        json.loads((REPO / pin_path).read_bytes())
    ), "ExactGufoOnlyPinAddition"
    context = dict(
        ids=sorted(names),
        recipes=sorted(first.RECIPES),
        sections=sorted(sections),
        options=sorted(options),
        persisted=persisted,
        defaults=defaults,
        pins=pins,
        exports=evidence["exports"],
    )
    ids = copy.deepcopy(old_ids)
    ids["builtin_context"] = first.identity(
        "lemonade.backend-builtin-context.v1\n", context
    )
    expected, result_ids = expected_snapshot(base, evidence, ids, schema)
    result_ids["builtin_context"] = ids["builtin_context"]
    expected["v1"]["file_sha256"][SOURCE] = first.sha((REPO / SOURCE).read_bytes())
    expected["v1"]["inactive_definitions"] = dict(
        gufo=dict(
            recipe="gufo",
            kind="container",
            source_sha256=first.sha((REPO / SOURCE).read_bytes()),
        )
    )
    return expected, result_ids


def validate(manifest, previous, expected, previous_ids, ids):
    assert set(manifest) == {
        "schema_version",
        "stage",
        "base_commit",
        "active",
        "source_sha256",
        "previous_identities",
        "identities",
        "changes",
    }, "ExactGufoTransitionFields"
    assert (
        first.serialized(manifest["schema_version"]) == "1"
        and manifest["stage"] == "gufo-inactive-inventory"
        and manifest["base_commit"] == BASE
        and manifest["active"] is False
    ), "ExactGufoTransitionProvenance"
    assert manifest["source_sha256"] == first.sha(
        (REPO / SOURCE).read_bytes()
    ), "PinnedGufoSourceBytes"
    assert first.serialized(manifest["previous_identities"]) == first.serialized(
        previous_ids
    ) and first.serialized(manifest["identities"]) == first.serialized(
        ids
    ), "ExactGufoStageIdentities"
    records = manifest["changes"]
    assert len({r["path"] for r in records}) == len(
        records
    ), "UniqueGufoTransitionRecords"
    assert first.serialized(records) == first.serialized(
        first.diff(previous, expected)
    ), "ExactGufoTransitionRecords"
    assert first.serialized(first.apply(previous, records)) == first.serialized(
        expected
    ), "WholeGufoTransitionReplay"
    return records


def check(args, previous, expected, previous_ids, ids):
    manifest = json.loads(MANIFEST.read_bytes())
    records = validate(manifest, previous, expected, previous_ids, ids)
    mutations = []
    for name, assertion, mutate in (
        ("missing", "ExactGufoTransitionRecords", lambda m: m["changes"].pop()),
        (
            "duplicate",
            "UniqueGufoTransitionRecords",
            lambda m: m["changes"].append(m["changes"][0]),
        ),
        (
            "activation",
            "ExactGufoTransitionProvenance",
            lambda m: m.update(active=True),
        ),
        ("source", "PinnedGufoSourceBytes", lambda m: m.update(source_sha256="0" * 64)),
        (
            "identity",
            "ExactGufoStageIdentities",
            lambda m: m["identities"].update(admission="sha256:" + "0" * 64),
        ),
    ):
        changed = copy.deepcopy(manifest)
        mutate(changed)
        try:
            validate(changed, previous, expected, previous_ids, ids)
        except AssertionError as error:
            assert str(error) == assertion, (name, str(error))
        else:
            raise AssertionError("UncaughtGufoDisclosureMutation:" + name)
        mutations.append(dict(rule=name, assertion=assertion, caught=True))
    (args.out / "gufo-transition-mutations.json").write_text(
        first.serialized(mutations, True) + "\n"
    )
    (args.out / "gufo-snapshot-transition.json").write_bytes(MANIFEST.read_bytes())
    print("C4b exact inactive Gufo transition:", len(records), "snapshot records")
