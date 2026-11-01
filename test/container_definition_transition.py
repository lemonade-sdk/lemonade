"""Replay the immutable history and disclose the exact C2 schema identity transition."""

import argparse
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
from types import SimpleNamespace

import backend_v2_4e3b_transition as first
from backend_v2_4e_audit import census

REPO = first.REPO
BASE = "1648503d3cf0c93b51219b8141cbf881feff5ec8"
MANIFEST = REPO / "test/fixtures/backend-v2/container.schema-transition.json"
SCHEMA = "schemas/backend-definition-v2.schema.json"
serialized, sha, schema_id, identity = (
    first.serialized,
    first.sha,
    first.schema_id,
    first.identity,
)


def git(path):
    return subprocess.check_output(
        ["git", "show", BASE + ":" + path], cwd=REPO, timeout=30
    )


def historical(args, evidence):
    tree = args.scratch / "base-source"
    if not tree.exists():
        tree.mkdir()
        archive = subprocess.check_output(
            ["git", "archive", BASE], cwd=REPO, timeout=60
        )
        with tarfile.open(fileobj=io.BytesIO(archive)) as source:
            source.extractall(tree, filter="data")
    pinned = json.loads(
        git("test/fixtures/backend-v2/exports.catalog-transition.json")
    )["identities"]
    old_evidence = copy.deepcopy(evidence)
    for field in ("admission", "catalog", "catalog_v2"):
        old_evidence[field] = pinned[field]
    old_evidence["execution"] = pinned["leaves"]["llamacpp"]["execution"]
    old_evidence["hrx"]["execution"] = pinned["leaves"]["hrx"]["execution"]
    (args.out / "historical-evidence.json").write_text(
        serialized(old_evidence, True) + "\n"
    )
    command = r"""
import json, sys
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0, sys.argv[1])
import backend_v2_4h_transition as third
first, second = third.first, third.second
out, scratch = Path(sys.argv[2]), Path(sys.argv[3])
raw_base, raw_extra = (out / "base-snapshot.json").read_bytes(), (out / "base-extra.json").read_bytes()
base = dict(v1=json.loads(raw_base), v2=json.loads(raw_extra))
actual = base
for manifest in (first.MANIFEST, second.MANIFEST, third.MANIFEST):
    actual = first.apply(actual, json.loads(manifest.read_bytes())["changes"])
evidence = json.loads((out / "historical-evidence.json").read_bytes())
values = base, actual, evidence, raw_base, raw_extra
stage1, stage2, evidence1, previous, ids1, old_ids = third.history(values)
manifest = json.loads(third.MANIFEST.read_bytes())
ids, records = third.validate(manifest, stage2, actual, evidence, previous, old_ids)
for name, function, m, values in (
    ("stage1", first.mutations, json.loads(first.MANIFEST.read_bytes()), (base, stage1, evidence1, raw_base, raw_extra)),
    ("stage2", second.mutations, json.loads(second.MANIFEST.read_bytes()), (stage1, stage2, previous, evidence1, ids1)),
    ("stage3", third.mutations, manifest, (stage2, actual, evidence, previous, old_ids)),
):
    o, s = out / name, scratch / name
    o.mkdir(exist_ok=True); s.mkdir(exist_ok=True)
    function(SimpleNamespace(out=o, scratch=s), m, values)
(out / "historical-snapshot.json").write_text(first.serialized(actual, True) + "\n")
(out / "historical-identities.json").write_text(first.serialized(ids, True) + "\n")
print("Unchanged historical stages replay:", 171, 141, len(records))
"""
    subprocess.run(
        [
            "python3",
            "-c",
            command,
            str(tree / "test"),
            str(args.out),
            str(args.scratch),
        ],
        cwd=tree,
        check=True,
        timeout=120,
    )
    return json.loads((args.out / "historical-snapshot.json").read_bytes()), pinned


def expected_snapshot(base, evidence, old_ids, new_schema):
    expected = copy.deepcopy(base)
    manifest = copy.deepcopy(base["v2"]["schema2"]["body"]["admission_manifest"])
    manifest["schema_id"] = schema_id(new_schema)
    defaults = json.loads(git("src/cpp/resources/defaults.json"))
    sections = set(defaults) | {"no_broadcast"}
    options = sections | set(evidence["common_defaults"])
    persisted = {}
    for row in evidence["descriptors"]:
        section = row["config_section"] or row["recipe"]
        sections.add(section)
        options.update(o["name"] for o in row["options"])
        if row["options"]:
            persisted[section] = sorted({first.key(o["name"]) for o in row["options"]})
    admission = identity(
        "lemonade.backend-admitted-catalog.v1\n",
        dict(
            builtins=old_ids["builtin_context"],
            manifest=manifest,
            definitions=[
                dict(
                    builtin="builtin:" + n + "@1",
                    authorization=old_ids["leaves"][n]["authority"],
                )
                for n in ("llamacpp", "hrx")
            ],
            recipes=sorted(first.RECIPES),
            sections=sorted(sections),
            options=sorted(options),
            persisted=persisted,
            aliases=[],
        ),
    )
    executions = [
        identity(
            "lemonade.backend-execution.v1\n",
            dict(
                admission=admission,
                compilation=old_ids["leaves"][n]["compilation"],
                binder=1,
                targets=1,
            ),
        )
        for n in ("llamacpp", "hrx")
    ]
    schema = copy.deepcopy(base["v1"]["schema"]["body"])
    del schema["catalog_id"]
    catalog = schema_id(
        dict(
            export=schema,
            arms=evidence["arms"],
            executions=executions,
            runner_service_policy=6,
        )
    )
    catalog_v2 = schema_id(
        dict(
            domain="lemonade.backend-catalog-v2.v1",
            admission=admission,
            executable=catalog,
        )
    )
    schema["catalog_id"] = catalog

    def response(body):
        return dict(status=200, body=body, etag='"' + schema_id(body) + '"')

    expected["v1"]["schema"] = response(schema)
    expected["v1"]["cli_schema"]["stdout"] = serialized(schema, True) + "\n"
    for case in expected["v1"]["cli_validation"].values():
        for result in case.values():
            result["stdout"] = result["stdout"].replace(old_ids["catalog"], catalog)
    schema2 = copy.deepcopy(base["v2"]["schema2"]["body"])
    schema2.update(
        catalog_id=catalog,
        catalog_v2_id=catalog_v2,
        feature_manifest_id=schema_id(manifest),
        admission_manifest=manifest,
    )
    expected["v2"]["schema2"] = response(schema2)
    for version, main, filtered, conditional in (
        ("v1", "schema", "filtered", "conditional"),
        ("v2", "schema2", "filtered2", "conditional2"),
    ):
        complete = expected[version][main]
        expected[version][conditional]["etag"] = complete["etag"]
        for recipe in first.RECIPES:
            body = copy.deepcopy(complete["body"])
            body["recipes"] = [r for r in body["recipes"] if r["recipe"] == recipe]
            expected[version][filtered][recipe] = response(body)
    ids = dict(
        admission=admission,
        execution=executions[0],
        hrx_execution=executions[1],
        catalog=catalog,
        catalog_v2=catalog_v2,
        schema=schema_id(new_schema),
        manifest=schema_id(manifest),
    )
    return expected, ids


def validate_schema(manifest):
    old = json.loads(git(SCHEMA))
    actual_bytes = (REPO / SCHEMA).read_bytes()
    actual = json.loads(actual_bytes)
    assert set(manifest) == {
        "schema_version",
        "stage",
        "base_commit",
        "old_sha256",
        "new_sha256",
        "old_schema_id",
        "new_schema_id",
        "changes",
    }, "ExactContainerTransitionFields"
    assert (
        serialized(manifest["schema_version"]) == "1"
        and manifest["stage"] == "container-data-only"
        and manifest["base_commit"] == BASE
    ), "ExactContainerTransitionProvenance"
    assert manifest["old_sha256"] == sha(git(SCHEMA)) and manifest[
        "old_schema_id"
    ] == schema_id(old), "PinnedPreviousSchemaIdentity"
    assert manifest["new_sha256"] == sha(actual_bytes) and manifest[
        "new_schema_id"
    ] == schema_id(actual), "DisclosedContainerSchemaIdentity"
    records = manifest["changes"]
    assert len({r["path"] for r in records}) == len(
        records
    ), "UniqueContainerSchemaRecords"
    assert serialized(first.diff(old, actual)) == serialized(
        records
    ), "ExactContainerSchemaChanges"
    assert serialized(first.apply(old, records)) == serialized(
        actual
    ), "WholeContainerSchemaReplay"
    return actual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gate", choices=["run", "check"])
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    args.build, args.out, args.scratch = (
        p.resolve() for p in (args.build, args.out, args.scratch)
    )
    for p in (args.out, args.scratch):
        p.mkdir(parents=True, exist_ok=True)
    temporary = args.scratch / "tmp"
    temporary.mkdir(exist_ok=True)
    os.environ.update(
        TMPDIR=str(temporary),
        TMP=str(temporary),
        TEMP=str(temporary),
        HF_HUB_CACHE=str(args.scratch / "hf-empty"),
    )
    tempfile.tempdir = str(temporary)
    if args.gate == "run":
        first.capture_all(args)
    values = first.load(args)
    actual, evidence = values[1:3]
    base, old_ids = historical(args, evidence)
    schema = validate_schema(json.loads(MANIFEST.read_bytes()))
    expected, ids = expected_snapshot(base, evidence, old_ids, schema)
    old_evidence = json.loads((args.out / "historical-evidence.json").read_bytes())
    expected_evidence = copy.deepcopy(old_evidence)
    for k in ("admission", "execution", "catalog", "catalog_v2"):
        expected_evidence[k] = ids[k]
    expected_evidence["hrx"]["execution"] = ids["hrx_execution"]
    assert serialized(expected_evidence) == serialized(
        evidence
    ), "OnlySchemaDependentEvidenceChanges"
    assert serialized(expected) == serialized(
        actual
    ), "ExactSchemaDependentWholeSnapshot"
    raw = (serialized(expected, True) + "\n").encode()
    (args.out / "expected-snapshot.json").write_bytes(raw)
    (args.out / "actual-snapshot.json").write_text(serialized(actual, True) + "\n")
    subprocess.run(
        [
            "cmp",
            str(args.out / "expected-snapshot.json"),
            str(args.out / "actual-snapshot.json"),
        ],
        check=True,
        timeout=10,
    )
    for name, select in {
        "legacy": lambda v: v["v1"],
        "schema": lambda v: v["v1"]["schema"],
        "digests": lambda v: {k: v["v1"][k] for k in ("definitions", "file_sha256")},
        "etags": lambda v: dict(
            full=v["v1"]["schema"]["etag"],
            filtered={k: r["etag"] for k, r in v["v1"]["filtered"].items()},
            conditional=v["v1"]["conditional"],
        ),
    }.items():
        assert serialized(select(expected)) == serialized(select(actual)), name
        (args.out / (name + ".diff")).write_bytes(b"")
    changes = first.diff(base, expected)
    (args.out / "snapshot-transition.json").write_text(
        serialized(dict(identities=ids, changes=changes), True) + "\n"
    )
    mutant = json.loads(MANIFEST.read_bytes())
    mutant["new_schema_id"] = "sha256:" + "0" * 64
    try:
        validate_schema(mutant)
    except AssertionError as error:
        assert str(error) == "DisclosedContainerSchemaIdentity"
    else:
        raise AssertionError("UncaughtUndisclosedSchemaIdentity")
    mutation_results = [
        dict(
            rule="undisclosed-schema-identity",
            assertion="DisclosedContainerSchemaIdentity",
            caught=True,
        )
    ]
    for name, assertion, change in (
        (
            "duplicate-schema-record",
            "UniqueContainerSchemaRecords",
            lambda m: m["changes"].append(m["changes"][0]),
        ),
        (
            "missing-schema-record",
            "ExactContainerSchemaChanges",
            lambda m: m["changes"].pop(),
        ),
        (
            "foreign-schema-record",
            "ExactContainerSchemaChanges",
            lambda m: m["changes"].append(dict(op="add", path="/foreign", new=True)),
        ),
        (
            "typed-transition-version",
            "ExactContainerTransitionProvenance",
            lambda m: m.update(schema_version=True),
        ),
    ):
        changed = json.loads(MANIFEST.read_bytes())
        change(changed)
        try:
            validate_schema(changed)
        except AssertionError as error:
            assert str(error) == assertion, (name, str(error))
        else:
            raise AssertionError("UncaughtContainerTransitionMutation:" + name)
        mutation_results.append(dict(rule=name, assertion=assertion, caught=True))
    changed_snapshot = copy.deepcopy(actual)
    changed_snapshot["v2"]["schema2"]["body"]["admission_manifest"]["schema_id"] = (
        "sha256:" + "0" * 64
    )
    try:
        assert serialized(expected) == serialized(
            changed_snapshot
        ), "ExactSchemaDependentWholeSnapshot"
    except AssertionError as error:
        assert str(error) == "ExactSchemaDependentWholeSnapshot"
    else:
        raise AssertionError("UncaughtAdvertisedSchemaIdentity")
    mutation_results.append(
        dict(
            rule="undisclosed-advertised-schema-identity",
            assertion="ExactSchemaDependentWholeSnapshot",
            caught=True,
        )
    )
    (args.out / "container-transition-mutations.json").write_text(
        serialized(mutation_results, True) + "\n"
    )
    names = json.loads(
        (REPO / "test/fixtures/backend-v2/hrx.cpp-ci-names.json").read_bytes()
    ) + ["LlamaCppExternalForkTest"]
    baseline = args.out / "base-cpp-ci.json"
    baseline.write_text(
        serialized(
            dict(
                tests=[
                    dict(name=n, properties=[dict(name="LABELS", value=["cpp-ci"])])
                    for n in names
                ]
            ),
            True,
        )
        + "\n"
    )
    census(
        SimpleNamespace(
            build=args.build,
            baseline=baseline,
            expected_count=155,
            no_additions=False,
            out=args.out,
        )
    )
    print(
        "C2 exact schema transition:",
        len(changes),
        "snapshot records; whole-snapshot cmp and four zero-byte residual projections; 155 tests",
    )


if __name__ == "__main__":
    main()
