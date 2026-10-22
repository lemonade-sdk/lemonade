"""Replay the immutable llama activation and the independently derived HRX promotion."""

import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace

import backend_v2_4e3b_transition as first
from backend_v2_4e3a_audit import purity, protected_paths
from backend_v2_4e_audit import census
from backend_definition_inventory import production_inventory, V1_NAMES

REPO = first.REPO
MANIFEST = REPO / "test/fixtures/backend-v2/hrx.catalog-transition.json"
BASE = "85788f7872514e29598a6244021464643e9342ec"
SNAPSHOT = "8c59aba69ae0aed096f3df45095d4db5688f78c20468cebb5eecc2fcac3cecb6"
PARENT_POLICY = first.POLICY
HRX_POLICY = "sha256:36dd27c1b1cb774af2bd66e92de5ede08ddff7b336c61c22e04ee175da6b4a49"
REGISTRY = "sha256:825b372899c818ecf43a42529ccc9e254632721dfda84e661396474bcecb6f61"
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


def merge(target, value):
    for key, child in value.items():
        if isinstance(child, dict) and isinstance(target.get(key), dict):
            merge(target[key], child)
        else:
            target[key] = copy.deepcopy(child)


def pointer(value, path):
    for token in path.strip("/").split("/") if path else []:
        token = token.replace("~1", "/").replace("~0", "~")
        value = value[int(token)] if isinstance(value, list) else value[token]
    return value


def child_declaration(parent, child):
    result = copy.deepcopy(parent)
    merge(result, child["merge"])
    for edit in child["edits"]:
        path = edit["path"]
        if "match" in edit:
            array = pointer(result, path)
            matches = [
                i
                for i, v in enumerate(array)
                if all(v.get(k) == x for k, x in edit["match"].items())
            ]
            assert len(matches) == 1, "ChildEditHasOneExactMatch"
            owner, key = array, matches[0]
            if "at" in edit:
                path = edit["at"]
                selected = owner[key]
                owner, key = (
                    pointer(selected, path.rsplit("/", 1)[0]),
                    path.rsplit("/", 1)[1],
                )
        else:
            owner = pointer(result, path.rsplit("/", 1)[0])
            key = path.rsplit("/", 1)[1]
        op = edit["op"]
        if op == "remove":
            del owner[key]
        elif op == "clear":
            owner[key] = type(owner[key])()
        elif op == "merge":
            merge(owner[key], edit["value"])
        elif op == "replace":
            owner[key] = copy.deepcopy(edit["value"])
        elif op == "add":
            owner[key].append(copy.deepcopy(edit["value"]))
        else:
            raise AssertionError("ClosedChildEditVocabulary")
    result.update(schema_version=2, id="hrx", recipe="llamacpp-hrx")
    return result


def normalized(value):
    value = copy.deepcopy(value)
    value["launch"]["environment"].setdefault("order", "preset_first")
    for row in value["launch"]["argv"]:
        row.setdefault("when", True)
        row.setdefault("reserve", "always")
        row.setdefault("aliases", [])
    for row in value["launch"]["custom_args"]["defaults"]:
        row.setdefault("when", True)
        row.setdefault("aliases", [])
    for op in value["operations"]:
        op["request"].setdefault("remove", [])
        op["request"].setdefault("set", {})
        op["request"].setdefault("omit_missing", False)
    return value


def schema_controls():
    old = json.loads(git("schemas/backend-definition-v2.schema.json"))
    new = copy.deepcopy(old)
    launch = new["$defs"]["Launch"]["properties"]
    launch["device_assignment"] = dict(enum=["select_backend", "preserve"])
    launch["load_log_policy"] = dict(enum=["none", "loading_and_options"])
    launch["direct_stream_policy"]["enum"].append("bound_passthrough")
    actual = json.loads(
        (REPO / "schemas/backend-definition-v2.schema.json").read_bytes()
    )
    assert serialized(actual) == serialized(new), "ExactThreeSchemaControlsOnly"
    return dict(
        old_schema_id=schema_id(old),
        new_schema_id=schema_id(new),
        changes=diff(old, new),
    )


def stage_one(base, raw_base, raw_extra):
    manifest_bytes = git("test/fixtures/backend-v2/llamacpp.catalog-transition.json")
    assert (
        REPO / "test/fixtures/backend-v2/llamacpp.catalog-transition.json"
    ).read_bytes() == manifest_bytes, "Historical171RecordsImmutable"
    manifest = json.loads(manifest_bytes)
    assert len(manifest["changes"]) == 171, "Historical171RecordCount"
    evidence_bytes = (
        REPO / "test/fixtures/backend-v2/llamacpp.production-evidence.json"
    ).read_bytes()
    assert (
        sha(evidence_bytes)
        == "f1832e10796d8818c524870d445bae87a4bf9955f7994bd321f3c2b0bdbe2f9a"
    ), "PinnedHistoricalProductionEvidence"
    evidence = json.loads(evidence_bytes)
    expected, _ = first.expected_snapshot(base, evidence)
    first.validate(manifest, base, expected, evidence, raw_base, raw_extra)
    replay = (serialized(apply(base, manifest["changes"]), True) + "\n").encode()
    assert (
        sha(replay) == SNAPSHOT
        and replay == (serialized(expected, True) + "\n").encode()
    ), "StageOneByteReplayMatchesComplete8c59Snapshot"
    return expected, evidence, manifest["identities"]


def identities(evidence, previous, old_ids, manifest):
    for field in ("descriptors", "spec_aliases", "common_defaults", "abi", "exports"):
        assert serialized(evidence[field]) == serialized(previous[field]), (
            "HardUnchangedEvidence:" + field
        )
    assert evidence["v1_ids"] == [
        x for x in previous["v1_ids"] if x != "hrx"
    ], "SevenV1IdsExact"
    parent = previous["declaration"]
    child = json.loads((REPO / "src/cpp/resources/backends/hrx.json").read_bytes())
    hrx = child_declaration(parent, child)
    assert (
        hrx["operations"] == parent["operations"] and len(hrx["operations"]) == 11
    ), "AllElevenHrxOperationRowsInherited"
    assert (
        evidence["declaration"] == parent
        and evidence["normalized"] == previous["normalized"]
        and evidence["hooks"] == previous["hooks"]
    ), "LlamaSourceResolvedPolicyAndProgramUnchanged"
    assert evidence["hrx"]["declaration"] == hrx and evidence["hrx"][
        "normalized"
    ] == normalized(hrx), "HrxDeclarationAndNormalizationIndependentlyReplayed"
    leaves = {}
    for name, source_value, declaration, parents, ev in (
        (
            "llamacpp",
            json.loads(git("src/cpp/resources/backends/llamacpp.json")),
            parent,
            [],
            evidence,
        ),
        (
            "hrx",
            child,
            hrx,
            [
                dict(
                    document=dict(namespace="builtin", id="llamacpp", contract=1),
                    source_digest=old_ids["source"],
                    definition_hash=old_ids["definition"],
                )
            ],
            evidence["hrx"],
        ),
    ):
        source = identity("lemonade.backend-source.v2\n", source_value)
        definition = identity("lemonade.backend-definition.v2\n", declaration)
        dependencies = identity(
            "lemonade.backend-dependencies.v2\n",
            dict(resolver_revision=1, leaf_source_digest=source, parents=parents),
        )
        policy = copy.deepcopy(previous["builtin_policy"])
        if name == "hrx":
            policy["builtin"] = "hrx"
            policy["slots"] = [
                s for s in policy["slots"] if s["slot"]["owner_kind"] != 0
            ]
            semantics = copy.deepcopy(parent["artifacts"]["main"])
            del semantics["hook"]
            semantics["required_when"] = True
            main = next(s for s in policy["slots"] if s["slot"]["owner_kind"] == 2)
            main["owner_semantics"] = identity(
                "lemonade.backend-hook-owner.v1\n", semantics
            )
            del main["mutation_policy_digest"]
            main["mutation_policy_digest"] = identity(
                "lemonade.backend-hook-mutation-policy.v1\n", main
            )
        assert ev["builtin_policy"] == policy, (
            "IndependentRestrictedFixedPolicy:" + name
        )
        policy_id = identity("lemonade.backend-export-policy.v1\n", policy)
        assert policy_id == (PARENT_POLICY if name == "llamacpp" else HRX_POLICY), (
            "FixedLeafPolicyIdentity:" + name
        )
        grants = [
            dict(builtin=name, contract=1, slot=s["slot"]) for s in policy["slots"]
        ]
        assert ev["grants"] == grants, "ExactLeafGrantOccurrences:" + name
        leaves[name] = dict(
            source=source,
            definition=definition,
            dependencies=dependencies,
            builtin_policy=policy_id,
            owner_graph=policy["owner_graph_digest"],
        )
    registry = identity(
        "lemonade.backend-export-registry.v1\n",
        dict(
            exports=[
                dict(
                    builtin=n,
                    contract=1,
                    source=v["source"],
                    definition=v["definition"],
                    dependencies=v["dependencies"],
                    policy_revision=1,
                    policy=v["builtin_policy"],
                )
                for n, v in sorted(leaves.items())
            ],
            hook_abis=evidence["abi"],
        ),
    )
    assert (
        registry == REGISTRY == evidence["builtin_registry"]
    ), "TwoLeafBuiltinRegistryRecomputed"
    for name, ids in leaves.items():
        ev = evidence if name == "llamacpp" else evidence["hrx"]
        ids["authority"] = identity(
            "lemonade.backend-authorization.v1\n",
            dict(
                revision=3,
                registry=first.EXPORTS,
                definition=ids["definition"],
                dependencies=ids["dependencies"],
                grants=ev["grants"],
                builtin_policy=ids["builtin_policy"],
                builtin_registry=registry,
                source=ids["source"],
            ),
        )
        assert ids["authority"] == ev["authority"], "LeafAuthorityRecomputed:" + name
        ids["compilation"] = identity(
            "lemonade.backend-compilation.v2\n",
            dict(
                hooks=ev["hooks"],
                definition=ids["definition"],
                dependencies=ids["dependencies"],
                compiler=5,
                defaults=2,
                bindings=1,
                adapters=2,
                semantics=ev["normalized"],
            ),
        )
        assert ids["compilation"] == ev["compilation"], (
            "LeafCompilationRevisionFive:" + name
        )
    defaults = json.loads(git("src/cpp/resources/defaults.json"))
    ids, recipes = set(first.RECIPES), set(first.RECIPES)
    sections = set(defaults) | {"no_broadcast"}
    options = set(defaults) | {"no_broadcast"} | set(evidence["common_defaults"])
    persisted = {}
    for d in evidence["descriptors"]:
        section = d["config_section"] or d["recipe"]
        ids.add(section)
        sections.add(section)
        if d["options"]:
            persisted[section] = sorted({first.key(o["name"]) for o in d["options"]})
        options.update(o["name"] for o in d["options"])
    for pair in evidence["spec_aliases"]:
        ids.update(x for x in pair if x)
    ids.update(evidence["v1_ids"])
    context = dict(
        ids=sorted(ids),
        recipes=sorted(recipes),
        sections=sorted(sections),
        options=sorted(options),
        persisted=persisted,
        defaults=defaults,
        pins=json.loads(git("src/cpp/resources/backend_versions.json")),
        exports=first.EXPORTS,
    )
    builtin_context = identity("lemonade.backend-builtin-context.v1\n", context)
    assert builtin_context == old_ids["builtin_context"], "BuiltinContextUnchanged"
    admission = identity(
        "lemonade.backend-admitted-catalog.v1\n",
        dict(
            builtins=builtin_context,
            manifest=manifest,
            definitions=[
                dict(builtin="builtin:" + n + "@1", authorization=v["authority"])
                for n, v in leaves.items()
            ],
            recipes=sorted(recipes),
            sections=sorted(sections),
            options=sorted(options),
            persisted=persisted,
            aliases=[],
        ),
    )
    assert evidence["admission"] == admission, "TwoLeafAdmissionRecomputed"
    for name, ids in leaves.items():
        ids["execution"] = identity(
            "lemonade.backend-execution.v1\n",
            dict(
                admission=admission, compilation=ids["compilation"], binder=1, targets=1
            ),
        )
        assert (
            ids["execution"]
            == (evidence if name == "llamacpp" else evidence["hrx"])["execution"]
        ), ("LeafExecutionRecomputed:" + name)
    arms = copy.deepcopy(previous["arms"])
    assert arms[1] == ["llamacpp-hrx", "D"], "BaselineHrxArmD"
    arms[1][1] = "D2"
    assert evidence["arms"] == arms, "OnlyHrxArmChangesDToD2"
    return (
        dict(
            leaves=leaves,
            builtin_registry=registry,
            builtin_context=builtin_context,
            admission=admission,
        ),
        arms,
    )


def expected_snapshot(base, evidence, previous, old_ids):
    expected = copy.deepcopy(base)
    manifest = copy.deepcopy(base["v2"]["schema2"]["body"]["admission_manifest"])
    controls = schema_controls()
    assert manifest["schema_id"] == controls["old_schema_id"], "OldAdmissionSchemaExact"
    manifest["schema_id"] = controls["new_schema_id"]
    ids, arms = identities(evidence, previous, old_ids, manifest)
    schema = copy.deepcopy(base["v1"]["schema"]["body"])
    row = schema["recipes"][1]
    assert (
        row["recipe"] == "llamacpp-hrx"
        and row["definition_id"] == "hrx"
        and row["operations_complete"] is True
    ), "HrxPublicMetadataAlreadyComplete"
    row["definition_hash"] = ids["leaves"]["hrx"]["definition"]
    unhashed = copy.deepcopy(schema)
    del unhashed["catalog_id"]
    ids["catalog"] = schema_id(
        dict(
            export=unhashed,
            arms=arms,
            executions=[ids["leaves"][n]["execution"] for n in ("llamacpp", "hrx")],
            runner_service_policy=5,
        )
    )
    ids["catalog_v2"] = schema_id(
        dict(
            domain="lemonade.backend-catalog-v2.v1",
            admission=ids["admission"],
            executable=ids["catalog"],
        )
    )
    ids["admission_manifest"] = schema_id(manifest)
    assert (
        evidence["catalog"] == ids["catalog"]
        and evidence["catalog_v2"] == ids["catalog_v2"]
    ), "CatalogIdentitiesRecomputed"
    schema["catalog_id"] = ids["catalog"]

    def response(body):
        return dict(status=200, body=body, etag='"' + schema_id(body) + '"')

    expected["v1"]["schema"] = response(schema)
    expected["v1"]["definitions"]["hrx"] = ids["leaves"]["hrx"]["definition"]
    expected["v1"]["file_sha256"]["src/cpp/resources/backends/hrx.json"] = sha(
        (REPO / "src/cpp/resources/backends/hrx.json").read_bytes()
    )
    expected["v1"]["cli_schema"]["stdout"] = serialized(schema, True) + "\n"
    for case in expected["v1"]["cli_validation"].values():
        for result in case.values():
            result["stdout"] = result["stdout"].replace(
                base["v1"]["schema"]["body"]["catalog_id"], ids["catalog"]
            )
    schema2 = copy.deepcopy(base["v2"]["schema2"]["body"])
    schema2.update(
        catalog_id=ids["catalog"],
        catalog_v2_id=ids["catalog_v2"],
        feature_manifest_id=ids["admission_manifest"],
        admission_manifest=manifest,
    )
    schema2["recipes"][1] = dict(row, input_schema_version=2)
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
    leaf_fields = {
        "source",
        "definition",
        "dependencies",
        "builtin_policy",
        "owner_graph",
        "authority",
        "compilation",
        "execution",
    }
    old_authority = dict(
        leaves=dict(llamacpp={k: v for k, v in old_ids.items() if k in leaf_fields}),
        **{k: v for k, v in old_ids.items() if k not in leaf_fields},
    )
    return dict(
        schema_version=1,
        stage="hrx-builtin-child",
        base_commit=BASE,
        base_tree=subprocess.check_output(
            ["git", "rev-parse", BASE + "^{tree}"], cwd=REPO, text=True
        ).strip(),
        snapshot_sha256=SNAPSHOT,
        snapshot_bytes=672006,
        historical_manifest_sha256=sha(
            git("test/fixtures/backend-v2/llamacpp.catalog-transition.json")
        ),
        schema_controls=schema_controls(),
        revisions=dict(
            compiler=dict(old=4, new=5), runner_service_policy=dict(old=4, new=5)
        ),
        inventory=dict(
            old=dict(v1=8, v2=1, sources=9),
            new=dict(v1=7, v2=2, sources=9),
            recipes=first.RECIPES,
        ),
        authority_changes=diff(old_authority, ids),
        identities=ids,
        changes=diff(base, expected),
    )


def validate(manifest, base, actual, evidence, previous, old_ids):
    expected, ids = expected_snapshot(base, evidence, previous, old_ids)
    required = disclosure(base, expected, ids, old_ids)
    assert set(manifest) == set(required), "ExactSecondStageManifestFields"
    for field in set(required) - {"changes"}:
        assert serialized(manifest[field]) == serialized(required[field]), (
            "ExactSecondStageDisclosure:" + field
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
    replay = (serialized(apply(base, records), True) + "\n").encode()
    assert (
        replay
        == (serialized(actual, True) + "\n").encode()
        == (serialized(expected, True) + "\n").encode()
    ), "StageTwoWholeSnapshotByteReplay"
    return ids, records


def source_preservation(out):
    paths = protected_paths() + ["src/cpp/resources/backends/llamacpp.json"]
    hashes = {}
    for path in paths:
        if path in {
            "src/cpp/resources/backends/hrx.json",
            "schemas/backend-definition-v2.schema.json",
        }:
            continue
        actual = (REPO / path).read_bytes()
        assert actual == git(path), "ProtectedSource:" + path
        hashes[path] = sha(actual)
    (out / "source-preservation.json").write_text(serialized(hashes, True) + "\n")
    assert len(hashes) == 40, "ProtectedSourceInventoryComplete"
    assert (REPO / "test/fixtures/backend-v1-compat/hrx.v1.json").read_bytes() == git(
        "src/cpp/resources/backends/hrx.json"
    ), "HistoricalHrxSourceByteExact"


def mutations(args, manifest, values):
    results = []

    def probe(name, assertion, edit, actual_edit=None):
        candidate = copy.deepcopy(manifest)
        actual = copy.deepcopy(values[1])
        edit(candidate)
        if actual_edit:
            actual_edit(actual)
        try:
            validate(candidate, values[0], actual, *values[2:])
        except AssertionError as error:
            assert str(error) == assertion, (name, str(error))
        else:
            raise AssertionError("UncaughtTransitionMutation:" + name)
        results.append(dict(rule=name, assertion=assertion, caught=True))
        (args.scratch / (name + ".json")).write_text(
            serialized(dict(manifest=candidate, actual=actual), True) + "\n"
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
    probe(
        "typed-value",
        "NoStaleTransitionRecord",
        lambda m: next(
            r for r in m["changes"] if r.get("new") == 2 and type(r.get("new")) is int
        ).update(new=2.0),
    )
    probe(
        "undisclosed-output",
        "CompleteRawSnapshotDiffEqualsDisclosure",
        lambda m: None,
        lambda a: a["v1"].update(foreign=True),
    )
    probe(
        "typed-actual",
        "CompleteRawSnapshotDiffEqualsDisclosure",
        lambda m: None,
        lambda a: a["v2"]["schema2"]["body"]["recipes"][1].update(
            input_schema_version=2.0
        ),
    )
    probe(
        "stale-etag",
        "CompleteRawSnapshotDiffEqualsDisclosure",
        lambda m: None,
        lambda a: a["v2"]["filtered2"]["llamacpp-hrx"].update(
            etag=values[0]["v2"]["filtered2"]["llamacpp-hrx"]["etag"]
        ),
    )
    for field in ("schema_controls", "revisions", "identities", "authority_changes"):
        probe(
            "undisclosed-" + field,
            "ExactSecondStageDisclosure:" + field,
            lambda m, f=field: m.update({f: {}}),
        )
    (args.out / "transition-mutations.json").write_text(
        serialized(results, True) + "\n"
    )
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gate", choices=["capture", "check", "run", "mutations"])
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--scratch",
        type=Path,
        default=REPO / ".synagent/scratch/backend-v2-4f/transition",
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
    base0, actual, evidence, raw_base, raw_extra = values
    base, previous, old_ids = stage_one(base0, raw_base, raw_extra)
    ids, records = validate(
        json.loads(MANIFEST.read_bytes()), base, actual, evidence, previous, old_ids
    )
    first.raw_snapshot(
        actual,
        (args.out / "current-snapshot.json").read_bytes(),
        (args.out / "current-extra.json").read_bytes(),
    )
    source_preservation(args.out)
    purity(args.build, args.out)
    assert production_inventory() == V1_NAMES | {"llamacpp"}, "SevenV1TwoV2NineSources"
    names = json.loads(
        (REPO / "test/fixtures/backend-v2/hrx.cpp-ci-names.json").read_bytes()
    )
    baseline = args.out / "base-cpp-ci.json"
    baseline.write_text(
        serialized(
            dict(
                tests=[
                    dict(name=name, properties=[dict(name="LABELS", value=["cpp-ci"])])
                    for name in names
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
            expected_count=151,
            no_additions=True,
            out=args.out,
        )
    )
    replay = (serialized(apply(base, records), True) + "\n").encode()
    (args.out / "applied-snapshot.json").write_bytes(replay)
    (args.out / "two-stage-result.json").write_text(
        serialized(
            dict(
                first_records=171,
                second_records=len(records),
                stage1_sha256=SNAPSHOT,
                stage2_sha256=sha(replay),
                byte_replay=True,
                identities=ids,
            ),
            True,
        )
        + "\n"
    )
    if args.gate in ("run", "mutations"):
        stage1 = SimpleNamespace(
            out=args.out / "stage1", scratch=args.scratch / "stage1"
        )
        stage1.out.mkdir(exist_ok=True)
        stage1.scratch.mkdir(exist_ok=True)
        first.mutations(
            stage1,
            json.loads(
                git("test/fixtures/backend-v2/llamacpp.catalog-transition.json")
            ),
            (base0, base, previous, raw_base, raw_extra),
        )
        mutations(
            args,
            json.loads(MANIFEST.read_bytes()),
            (base, actual, evidence, previous, old_ids),
        )
    print("Both transition stages replay byte-for-byte:", 171, len(records))


if __name__ == "__main__":
    main()
