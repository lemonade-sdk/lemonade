"""Replay three independently derived stages without weakening historical disclosures."""

import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
from unittest import mock

import backend_v2_4e3b_transition as first
import backend_v2_4f_transition as second
from backend_v2_4e_audit import census

REPO = first.REPO
BASE = "476c66303ab21990469a8226c8287a091ec80de7"
MANIFEST = REPO / "test/fixtures/backend-v2/exports.catalog-transition.json"
HISTORICAL = REPO / "test/fixtures/backend-v2/hrx.production-evidence.json"
EXPORTS = "sha256:d644387f2d2838a76ace64304318e673244e8cd117e496ae4394dc0792e2b9fa"
identity, serialized, sha, schema_id, diff, apply = (
    first.identity,
    first.serialized,
    first.sha,
    first.schema_id,
    first.diff,
    first.apply,
)


def git(path):
    return subprocess.check_output(
        ["git", "show", f"{BASE}:{path}"], cwd=REPO, timeout=30
    )


def history(values):
    base0, _, _, raw_base, raw_extra = values
    stage1, evidence1, ids1 = second.stage_one(base0, raw_base, raw_extra)
    raw = HISTORICAL.read_bytes()
    assert (
        sha(raw) == "1f22b55137c2ade04b4917e5c7629b59c42f1b93e1559ec028059f47201987d7"
    ), "PinnedStageTwoProductionEvidence"
    evidence2 = json.loads(raw)
    stage2, ids2 = second.expected_snapshot(stage1, evidence2, evidence1, ids1)
    assert second.MANIFEST.read_bytes() == git(
        str(second.MANIFEST.relative_to(REPO))
    ), "Historical141RecordsImmutable"
    manifest2 = json.loads(second.MANIFEST.read_bytes())
    second.validate(manifest2, stage1, stage2, evidence2, evidence1, ids1)
    replay = (serialized(apply(stage1, manifest2["changes"]), True) + "\n").encode()
    assert (
        replay == (serialized(stage2, True) + "\n").encode()
    ), "StageTwoWholeSnapshotByteReplay"
    return stage1, stage2, evidence1, evidence2, ids1, ids2


def expected_snapshot(base, evidence, previous, old_ids):
    expected = copy.deepcopy(base)
    comparable = copy.deepcopy(evidence)
    comparable.pop("export_contract")
    for key in (
        "exports",
        "authority",
        "admission",
        "execution",
        "catalog",
        "catalog_v2",
    ):
        comparable[key] = previous[key]
    comparable["hrx"]["authority"] = previous["hrx"]["authority"]
    comparable["hrx"]["execution"] = previous["hrx"]["execution"]
    assert serialized(comparable) == serialized(
        previous
    ), "OnlyRegistryDependentEvidenceChanges"
    contract = evidence["export_contract"]
    pins = json.loads(git("test/fixtures/backend-v2/external/llamacpp.pins.json"))
    assert (
        identity("lemonade.backend-operation-owners.v1\n", contract["owners"])
        == pins["owner_graph"]
    ), "ReviewedExportOwnerGraph"
    for i, slot in enumerate(contract["slots"]):
        unhashed = dict(slot)
        digest = unhashed.pop("mutation_policy_digest")
        assert (
            digest
            == pins["slots"][i]
            == identity("lemonade.backend-hook-mutation-policy.v1\n", unhashed)
        ), "ReviewedExportSlotPins"
    assert (
        len(contract["slots"]) == 17 and len(contract["owners"]) == 11
    ), "CompleteExportContract"
    assert (
        identity("lemonade.backend-export-policy.v1\n", contract) == pins["policy"]
    ), "ReviewedExternalPolicyPin"
    registry = identity(
        "lemonade.backend-export-registry.v1\n",
        dict(
            exports=[
                dict(
                    builtin=contract["builtin"],
                    contract=contract["contract"],
                    source=old_ids["leaves"]["llamacpp"]["source"],
                    definition=old_ids["leaves"]["llamacpp"]["definition"],
                    dependencies=old_ids["leaves"]["llamacpp"]["dependencies"],
                    policy_revision=contract["policy_revision"],
                    policy=pins["policy"],
                )
            ],
            hook_abis=evidence["abi"],
        ),
    )
    assert (
        registry == EXPORTS == pins["registry"] == evidence["exports"]
    ), "InstalledRegistryRecomputed"
    ids = copy.deepcopy(old_ids)
    ids["export_registry"] = registry
    for name, leaf in ids["leaves"].items():
        ev = evidence if name == "llamacpp" else evidence["hrx"]
        leaf["authority"] = identity(
            "lemonade.backend-authorization.v1\n",
            dict(
                revision=3,
                registry=registry,
                definition=leaf["definition"],
                dependencies=leaf["dependencies"],
                grants=ev["grants"],
                builtin_policy=leaf["builtin_policy"],
                builtin_registry=ids["builtin_registry"],
                source=leaf["source"],
            ),
        )
        assert ev["authority"] == leaf["authority"], (
            "RegistryDependentAuthority:" + name
        )
    defaults = json.loads(git("src/cpp/resources/defaults.json"))
    recipes = set(first.RECIPES)
    names = set(recipes) | set(evidence["v1_ids"])
    sections = set(defaults) | {"no_broadcast"}
    options = set(defaults) | {"no_broadcast"} | set(evidence["common_defaults"])
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
    context = dict(
        ids=sorted(names),
        recipes=sorted(recipes),
        sections=sorted(sections),
        options=sorted(options),
        persisted=persisted,
        defaults=defaults,
        pins=json.loads(git("src/cpp/resources/backend_versions.json")),
        exports=registry,
    )
    ids["builtin_context"] = identity("lemonade.backend-builtin-context.v1\n", context)
    manifest = copy.deepcopy(base["v2"]["schema2"]["body"]["admission_manifest"])
    manifest["export_registry_id"] = registry
    ids["admission_manifest"] = schema_id(manifest)
    ids["admission"] = identity(
        "lemonade.backend-admitted-catalog.v1\n",
        dict(
            builtins=ids["builtin_context"],
            manifest=manifest,
            definitions=[
                dict(
                    builtin="builtin:" + n + "@1",
                    authorization=ids["leaves"][n]["authority"],
                )
                for n in ("llamacpp", "hrx")
            ],
            recipes=sorted(recipes),
            sections=sorted(sections),
            options=sorted(options),
            persisted=persisted,
            aliases=[],
        ),
    )
    assert ids["admission"] == evidence["admission"], "RegistryDependentAdmission"
    for name, leaf in ids["leaves"].items():
        leaf["execution"] = identity(
            "lemonade.backend-execution.v1\n",
            dict(
                admission=ids["admission"],
                compilation=leaf["compilation"],
                binder=1,
                targets=1,
            ),
        )
        assert (
            leaf["execution"]
            == (evidence if name == "llamacpp" else evidence["hrx"])["execution"]
        ), ("RegistryDependentExecution:" + name)
    schema = copy.deepcopy(base["v1"]["schema"]["body"])
    unhashed = copy.deepcopy(schema)
    del unhashed["catalog_id"]
    ids["catalog"] = schema_id(
        dict(
            export=unhashed,
            arms=evidence["arms"],
            executions=[ids["leaves"][n]["execution"] for n in ("llamacpp", "hrx")],
            runner_service_policy=6,
        )
    )
    ids["catalog_v2"] = schema_id(
        dict(
            domain="lemonade.backend-catalog-v2.v1",
            admission=ids["admission"],
            executable=ids["catalog"],
        )
    )
    assert (
        ids["catalog"] == evidence["catalog"]
        and ids["catalog_v2"] == evidence["catalog_v2"]
    ), "RegistryDependentCatalogIdentities"
    schema["catalog_id"] = ids["catalog"]

    def response(body):
        return dict(status=200, body=body, etag='"' + schema_id(body) + '"')

    expected["v1"]["schema"] = response(schema)
    expected["v1"]["cli_schema"]["stdout"] = serialized(schema, True) + "\n"
    for case in expected["v1"]["cli_validation"].values():
        for result in case.values():
            result["stdout"] = result["stdout"].replace(
                old_ids["catalog"], ids["catalog"]
            )
    schema2 = copy.deepcopy(base["v2"]["schema2"]["body"])
    schema2.update(
        catalog_id=ids["catalog"],
        catalog_v2_id=ids["catalog_v2"],
        feature_manifest_id=ids["admission_manifest"],
        admission_manifest=manifest,
        export_registry_id=registry,
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
    return expected, ids


def disclosure(base, expected, ids, old_ids):
    old = dict(old_ids, export_registry=first.EXPORTS)
    return dict(
        schema_version=1,
        stage="production-external-export-activation",
        base_commit=BASE,
        base_tree=subprocess.check_output(
            ["git", "rev-parse", BASE + "^{tree}"], cwd=REPO, text=True
        ).strip(),
        historical_manifests={
            str(p.relative_to(REPO)): sha(git(str(p.relative_to(REPO))))
            for p in (first.MANIFEST, second.MANIFEST)
        },
        snapshot_sha256=sha((serialized(base, True) + "\n").encode()),
        acceptance_surface={
            k: dict(
                old=base["v2"]["schema2"]["body"][k],
                new=expected["v2"]["schema2"]["body"][k],
            )
            for k in (
                "accepted_definition_versions",
                "executable_definition_versions",
                "acceptance_stage",
            )
        },
        revisions=dict(
            compiler=dict(old=5, new=5), runner_service_policy=dict(old=5, new=6)
        ),
        authority_changes=diff(old, ids),
        identities=ids,
        changes=diff(base, expected),
    )


def validate(manifest, base, actual, evidence, previous, old_ids):
    expected, ids = expected_snapshot(base, evidence, previous, old_ids)
    required = disclosure(base, expected, ids, old_ids)
    assert set(manifest) == set(required), "ExactThirdStageManifestFields"
    for key in set(required) - {"changes"}:
        assert serialized(manifest[key]) == serialized(required[key]), (
            "ExactThirdStageDisclosure:" + key
        )
    records = manifest["changes"]
    assert len({r["path"] for r in records}) == len(
        records
    ), "NoDuplicateTransitionRecord"
    allowed = {r["path"]: r for r in required["changes"]}
    for record in records:
        assert record["path"] in allowed, "NoForeignOrUnnecessaryTransitionRecord"
        assert serialized(record) == serialized(
            allowed[record["path"]]
        ), "NoStaleTransitionRecord"
    assert serialized(records) == serialized(
        required["changes"]
    ), "CompleteTransitionRecordsNoMissingOrUnused"
    assert serialized(diff(base, actual)) == serialized(
        records
    ), "CompleteRawSnapshotDiffEqualsDisclosure"
    assert (
        (serialized(apply(base, records), True) + "\n").encode()
        == (serialized(actual, True) + "\n").encode()
        == (serialized(expected, True) + "\n").encode()
    ), "StageThreeWholeSnapshotByteReplay"
    return ids, records


def mutations(args, manifest, values):
    results = []

    def probe(name, assertion, change, actual_change=None):
        candidate = copy.deepcopy(manifest)
        actual = copy.deepcopy(values[1])
        change(candidate)
        if actual_change:
            actual_change(actual)
        try:
            validate(candidate, values[0], actual, *values[2:])
        except AssertionError as error:
            assert str(error) == assertion, (name, str(error))
        else:
            raise AssertionError("UncaughtThirdStageMutation:" + name)
        results.append(dict(rule=name, assertion=assertion, caught=True))
        (args.scratch / (name + ".json")).write_text(
            serialized(dict(manifest=candidate, actual=actual), True) + "\n"
        )

    probe(
        "extra-manifest-field",
        "ExactThirdStageManifestFields",
        lambda m: m.update(foreign=True),
    )
    for value, name in (
        (True, "boolean-manifest-version"),
        (1.0, "float-manifest-version"),
    ):
        probe(
            name,
            "ExactThirdStageDisclosure:schema_version",
            lambda m, v=value: m.update(schema_version=v),
        )
    probe(
        "foreign-history",
        "ExactThirdStageDisclosure:historical_manifests",
        lambda m: m.update(historical_manifests={}),
    )
    probe(
        "duplicate",
        "NoDuplicateTransitionRecord",
        lambda m: m["changes"].append(m["changes"][0]),
    )
    probe(
        "missing",
        "CompleteTransitionRecordsNoMissingOrUnused",
        lambda m: m["changes"].pop(),
    )
    probe(
        "foreign",
        "NoForeignOrUnnecessaryTransitionRecord",
        lambda m: m["changes"].append(dict(path="/foreign", op="add", new=True)),
    )
    probe(
        "unnecessary",
        "NoForeignOrUnnecessaryTransitionRecord",
        lambda m: m["changes"].append(
            dict(path="/v1/platform", op="replace", old="Linux", new="Linux")
        ),
    )
    probe(
        "stale",
        "NoStaleTransitionRecord",
        lambda m: m["changes"][0].update(old="stale"),
    )

    def unused_record(actual):
        record = manifest["changes"][0]
        parent, key = record["path"].rsplit("/", 1)
        target = second.pointer(actual, parent)
        target[
            (
                int(key)
                if isinstance(target, list)
                else key.replace("~1", "/").replace("~0", "~")
            )
        ] = copy.deepcopy(record["old"])

    probe(
        "unused-record",
        "CompleteRawSnapshotDiffEqualsDisclosure",
        lambda m: None,
        unused_record,
    )
    probe(
        "unused",
        "CompleteRawSnapshotDiffEqualsDisclosure",
        lambda m: None,
        lambda a: a["v1"].update(foreign=None),
    )
    probe(
        "undisclosed-identity",
        "CompleteRawSnapshotDiffEqualsDisclosure",
        lambda m: None,
        lambda a: a["v2"]["schema2"]["body"].update(catalog_v2_id="foreign"),
    )
    probe(
        "stale-filter-etag",
        "CompleteRawSnapshotDiffEqualsDisclosure",
        lambda m: None,
        lambda a: a["v2"]["filtered2"]["llamacpp-hrx"].update(
            etag=values[0]["v2"]["filtered2"]["llamacpp-hrx"]["etag"]
        ),
    )
    probe(
        "typed-actual",
        "CompleteRawSnapshotDiffEqualsDisclosure",
        lambda m: None,
        lambda a: a["v2"]["schema2"]["body"].update(
            executable_definition_versions=[1, 2.0]
        ),
    )
    for field in ("identities", "authority_changes", "acceptance_surface", "revisions"):
        probe(
            "undisclosed-" + field,
            "ExactThirdStageDisclosure:" + field,
            lambda m, f=field: m.update({f: {}}),
        )
    (args.out / "transition-mutations.json").write_text(
        serialized(results, True) + "\n"
    )


def census_mutations(args):
    current = json.loads((args.out / "cpp-ci.json").read_bytes())
    dependencies = (args.out / "dependencies.txt").read_text()
    results = []
    for name, assertion in (
        ("drop", "CppCiCountExact"),
        ("rename", "CppCiNamesExact"),
        ("extra", "CppCiCountExact"),
        ("helper", "CppCiExecutableDependencies"),
    ):
        changed = copy.deepcopy(current)
        selected = dependencies
        if name == "drop":
            changed["tests"].pop(0)
        elif name == "rename":
            changed["tests"][0]["name"] = "ForeignRenamedTest"
        elif name == "extra":
            changed["tests"].append(
                dict(
                    name="ForeignAddedTest",
                    properties=[dict(name="LABELS", value=["cpp-ci"])],
                )
            )
        else:
            selected = dependencies.replace(
                "test_external_fake_engine", "foreign-helper"
            )
        with mock.patch(
            "subprocess.check_output", side_effect=[serialized(changed), selected]
        ):
            try:
                census(
                    SimpleNamespace(
                        build=args.build,
                        baseline=args.out / "base-cpp-ci.json",
                        expected_count=153,
                        no_additions=False,
                        out=None,
                    )
                )
            except AssertionError as error:
                assert str(error) == assertion, (name, str(error))
            else:
                raise AssertionError("UncaughtCensusMutation:" + name)
        results.append(dict(rule=name, assertion=assertion, caught=True))
    (args.out / "census-mutations.json").write_text(serialized(results, True) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gate", choices=["capture", "check", "run", "mutations"])
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--scratch",
        type=Path,
        default=REPO / ".synagent/scratch/backend-v2-4h/transition",
    )
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    args.scratch.mkdir(parents=True, exist_ok=True)
    temp = args.scratch / "tmp"
    temp.mkdir(exist_ok=True)
    os.environ.update(
        TMPDIR=str(temp.resolve()), TMP=str(temp.resolve()), TEMP=str(temp.resolve())
    )
    tempfile.tempdir = str(temp.resolve())
    os.environ["HF_HUB_CACHE"] = str((args.scratch / "hf-empty").resolve())
    Path(os.environ["HF_HUB_CACHE"]).mkdir(exist_ok=True)
    if args.gate in ("capture", "run"):
        first.capture_all(args)
    if args.gate == "capture":
        return
    values = first.load(args)
    stage1, base, evidence1, previous, ids1, old_ids = history(values)
    actual, evidence = values[1:3]
    manifest = json.loads(MANIFEST.read_bytes())
    ids, records = validate(manifest, base, actual, evidence, previous, old_ids)
    first.raw_snapshot(
        actual,
        (args.out / "current-snapshot.json").read_bytes(),
        (args.out / "current-extra.json").read_bytes(),
    )
    second.source_preservation(args.out)
    second.purity(args.build, args.out)
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
            expected_count=153,
            no_additions=False,
            out=args.out,
        )
    )
    replay = (serialized(apply(base, records), True) + "\n").encode()
    (args.out / "applied-snapshot.json").write_bytes(replay)
    (args.out / "three-stage-result.json").write_text(
        serialized(
            dict(
                records=[171, 141, len(records)],
                byte_replay=True,
                stage3_sha256=sha(replay),
                identities=ids,
            ),
            True,
        )
        + "\n"
    )
    if args.gate in ("run", "mutations"):
        for name in ("stage1", "stage2"):
            (args.out / name).mkdir(exist_ok=True)
            (args.scratch / name).mkdir(exist_ok=True)
        first.mutations(
            SimpleNamespace(out=args.out / "stage1", scratch=args.scratch / "stage1"),
            json.loads(first.MANIFEST.read_bytes()),
            (values[0], stage1, evidence1, values[3], values[4]),
        )
        second.mutations(
            SimpleNamespace(out=args.out / "stage2", scratch=args.scratch / "stage2"),
            json.loads(second.MANIFEST.read_bytes()),
            (stage1, base, previous, evidence1, ids1),
        )
        mutations(args, manifest, (base, actual, evidence, previous, old_ids))
        census_mutations(args)
    print("All three transition stages replay byte-for-byte:", 171, 141, len(records))


if __name__ == "__main__":
    main()
