"""Verify the complete, independently reconstructed llama.cpp production transition."""

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace

from backend_catalog_http import Server, prepare, request
from backend_definition_inventory import V1_NAMES, production_inventory
from backend_v1_snapshot import capture
from backend_v2_4e3a_audit import (
    git_bytes,
    purity,
    source_preservation,
)
from llamacpp_frozen_control import REPO, ROOT, verify

MANIFEST = REPO / "test/fixtures/backend-v2/llamacpp.catalog-transition.json"
EXPORTS = "sha256:b2e770dfdb68a27db72cbe2fdad29f26c3eeb7ab2ca5ea136d325bfe4a5dfa8a"
POLICY = "sha256:7e4a67f7dbb82b146277e5fafd994e62f33d789ef758adb872164827f7bc0976"
REGISTRY = "sha256:d2c4eaea8d0f6a3f5128dfd2430b9521a2ff3f5c9bb87b8f463802f71da424cf"
RECIPES = [
    "llamacpp",
    "llamacpp-hrx",
    "whispercpp",
    "moonshine",
    "kokoro",
    "sd-cpp",
    "flm",
    "ryzenai-llm",
    "vllm",
    "thenoise",
    "ds4",
    "cloud",
    "thinksound",
    "acestep",
    "onnxruntime",
    "trellis",
    "openmoss",
]


def serialized(value, pretty=False):
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        **({"indent": 2} if pretty else {"separators": (",", ":")}),
    )


def sha(data):
    return hashlib.sha256(data).hexdigest()


def identity(domain, value):
    return "sha256:" + sha(domain.encode() + serialized(value).encode())


def schema_id(value):
    return "sha256:" + sha(serialized(value).encode())


def extra(lemond):
    with tempfile.TemporaryDirectory(prefix="switch-representation2-") as temporary:
        root = Path(temporary)
        prepare(root)
        with Server(lemond, root, "--builtin-backends-only") as server:

            def api(path, **kwargs):
                status, body, headers = request(server.port, path, **kwargs)
                return dict(status=status, body=body, etag=headers.get("ETag"))

            schema = api("/v1/backends/schema?representation=2")
            return dict(
                schema2=schema,
                filtered2={
                    r: api("/v1/backends/schema?representation=2&recipe=" + r)
                    for r in RECIPES
                },
                conditional2=api(
                    "/v1/backends/schema?representation=2",
                    headers={"If-None-Match": schema["etag"]},
                ),
            )


def key(name):
    for suffix, result in (
        ("_backend", "backend"),
        ("_args", "args"),
        ("_device", "device"),
    ):
        if name.endswith(suffix):
            return result
    return name


def identities(evidence, manifest):
    assert (
        sha(serialized(evidence["descriptors"]).encode())
        == "2f2e8ab357611e510ac9ff070b929dfd9d997a4f2e96b2fe942da1eff4b3acef"
    ), "AllSeventeenDescriptorFieldsAndDefaultsUnchanged"
    assert evidence["spec_aliases"] == [
        ["llamacpp", ""],
        ["llamacpp-hrx", "llamacpp-hrx"],
        ["whispercpp", ""],
        ["moonshine", "moonshine"],
        ["kokoro", "kokoro"],
        ["sd-cpp", ""],
        ["flm", ""],
        ["ryzenai-server", "ryzenai-llm"],
        ["vllm", ""],
        ["thenoise", ""],
        ["ds4", "ds4"],
        ["thinksound", "thinksound"],
        ["acestep", ""],
        ["onnxruntime", "onnxruntime"],
        ["trellis", "trellis"],
        ["openmoss", ""],
    ], "AllSpecAliasesUnchanged"
    assert evidence["v1_ids"] == [
        "hrx",
        "moonshine",
        "kokoro",
        "ryzenai",
        "ds4",
        "thinksound",
        "onnxruntime",
        "trellis",
    ], "EightV1IdsUnchanged"
    assert evidence["common_defaults"] == dict(
        auto_evict=None,
        auto_update=None,
        ctx_size=-1,
        downsize_idle_timeout=60,
        evict_idle_timeout=300,
        evict_weight_factor=1.0,
        merge_args=True,
        pinned=False,
    ), "CommonDefaultsUnchanged"
    candidate = json.loads(
        git_bytes("test/fixtures/backend-v2/llamacpp.candidate.json")
    )
    declaration = dict(
        candidate["merge"], id="llamacpp", recipe="llamacpp", schema_version=2
    )
    source = identity("lemonade.backend-source.v2\n", candidate)
    definition = identity("lemonade.backend-definition.v2\n", declaration)
    dependencies = identity(
        "lemonade.backend-dependencies.v2\n",
        dict(resolver_revision=1, leaf_source_digest=source, parents=[]),
    )
    assert (
        evidence["declaration"] == declaration
    ), "ResolvedDeclarationEqualsFrozenCandidate"
    policy = evidence["builtin_policy"]
    assert (
        identity("lemonade.backend-operation-owners.v1\n", policy["owners"])
        == policy["owner_graph_digest"]
    ), "OwnerGraphIdentityRecomputed"
    for slot in policy["slots"]:
        value = dict(slot)
        digest = value.pop("mutation_policy_digest")
        assert (
            identity("lemonade.backend-hook-mutation-policy.v1\n", value) == digest
        ), "SlotPolicyIdentityRecomputed"
    policy_id = identity("lemonade.backend-export-policy.v1\n", policy)
    assert policy_id == POLICY, "FixedReviewedPolicyIdentity"
    registry = identity(
        "lemonade.backend-export-registry.v1\n",
        dict(
            exports=[
                dict(
                    builtin="llamacpp",
                    contract=1,
                    source=source,
                    definition=definition,
                    dependencies=dependencies,
                    policy_revision=1,
                    policy=policy_id,
                )
            ],
            hook_abis=evidence["abi"],
        ),
    )
    assert (
        registry == REGISTRY and evidence["builtin_registry"] == registry
    ), "BuiltinRegistryIdentityRecomputed"
    assert (
        evidence["exports"] == EXPORTS
        and identity(
            "lemonade.backend-export-registry.v1\n",
            dict(exports=[], hook_abis=evidence["abi"]),
        )
        == EXPORTS
    ), "EmptyExportsAndNineteenAbiRecordsUnchanged"
    assert len(evidence["abi"]) == 19, "NineteenAbiRecords"
    authority = identity(
        "lemonade.backend-authorization.v1\n",
        dict(
            revision=3,
            registry=EXPORTS,
            definition=definition,
            dependencies=dependencies,
            grants=evidence["grants"],
            builtin_policy=policy_id,
            builtin_registry=registry,
            source=source,
        ),
    )
    assert evidence["authority"] == authority, "AuthorityIdentityRecomputed"
    defaults = json.loads(git_bytes("src/cpp/resources/defaults.json"))
    pins = json.loads(git_bytes("src/cpp/resources/backend_versions.json"))
    ids, recipes = set(RECIPES), set(RECIPES)
    sections = set(defaults) | {"no_broadcast"}
    options = set(defaults) | {"no_broadcast"} | set(evidence["common_defaults"])
    persisted = {}
    for d in evidence["descriptors"]:
        section = d["config_section"] or d["recipe"]
        ids.add(section)
        sections.add(section)
        if d["options"]:
            persisted[section] = sorted({key(o["name"]) for o in d["options"]})
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
        pins=pins,
        exports=EXPORTS,
    )
    builtin_context = identity("lemonade.backend-builtin-context.v1\n", context)
    admission = identity(
        "lemonade.backend-admitted-catalog.v1\n",
        dict(
            builtins=builtin_context,
            manifest=manifest,
            definitions=[dict(builtin="builtin:llamacpp@1", authorization=authority)],
            recipes=sorted(recipes),
            sections=sorted(sections),
            options=sorted(options),
            persisted=persisted,
            aliases=[],
        ),
    )
    assert evidence["admission"] == admission, "AdmissionIdentityRecomputed"
    compilation = identity(
        "lemonade.backend-compilation.v2\n",
        dict(
            hooks=evidence["hooks"],
            definition=definition,
            dependencies=dependencies,
            compiler=4,
            defaults=2,
            bindings=1,
            adapters=2,
            semantics=evidence["normalized"],
        ),
    )
    assert compilation == evidence["compilation"], "CompilationIdentityRecomputed"
    execution = identity(
        "lemonade.backend-execution.v1\n",
        dict(admission=admission, compilation=compilation, binder=1, targets=1),
    )
    assert execution == evidence["execution"], "ExecutionIdentityRecomputed"
    arms = [
        [
            r,
            (
                "D2"
                if r == "llamacpp"
                else (
                    "D"
                    if r
                    in {
                        "llamacpp-hrx",
                        "moonshine",
                        "kokoro",
                        "ryzenai-llm",
                        "ds4",
                        "thinksound",
                        "onnxruntime",
                        "trellis",
                    }
                    else "A"
                )
            ),
        ]
        for r in RECIPES
    ]
    assert evidence["arms"] == arms, "ExactlyOneArmAToD2"
    return (
        dict(
            source=source,
            definition=definition,
            dependencies=dependencies,
            owner_graph=policy["owner_graph_digest"],
            builtin_policy=policy_id,
            builtin_registry=registry,
            authority=authority,
            builtin_context=builtin_context,
            admission=admission,
            compilation=compilation,
            execution=execution,
        ),
        arms,
    )


def expected_snapshot(base, evidence):
    expected = copy.deepcopy(base)
    admission_manifest = expected["v2"]["schema2"]["body"]["admission_manifest"]
    admission_manifest["acceptance_stage"] = "execution_finalization"
    admission_manifest["executable_definition_versions"] = [1, 2]
    ids, arms = identities(evidence, admission_manifest)
    original_schema = copy.deepcopy(base["v1"]["schema"]["body"])
    schema = copy.deepcopy(original_schema)
    row = schema["recipes"][0]
    assert [
        r["recipe"] for r in schema["recipes"]
    ] == RECIPES, "SameOrderedSeventeenRecipes"
    row.update(
        definition_id="llamacpp",
        definition_hash=ids["definition"],
        operations_complete=True,
    )
    modes = [
        "chat",
        "embedding",
        "reranking",
        "transcription",
        "image",
        "tts",
        "audio-generation",
        "classification",
        "mesh",
    ]
    operations = [
        ("chat_completion", "buffered"),
        ("completion", "buffered"),
        ("responses", "buffered"),
        ("chat_completion", "streaming"),
        ("completion", "streaming"),
        ("responses", "streaming"),
        ("embeddings", "buffered"),
        ("reranking", "buffered"),
        ("slots", "buffered"),
        ("slot_action", "buffered"),
        ("tokenize", "buffered"),
    ]
    row["operations"] = [
        dict(operation=o, delivery=d, model_modes=modes, provenance="implemented")
        for o, d in operations
    ]
    unhashed = copy.deepcopy(schema)
    del unhashed["catalog_id"]
    ids["catalog"] = schema_id(
        dict(
            export=unhashed,
            arms=arms,
            executions=[ids["execution"]],
            runner_service_policy=4,
        )
    )
    schema["catalog_id"] = ids["catalog"]
    ids["catalog_v2"] = schema_id(
        dict(
            domain="lemonade.backend-catalog-v2.v1",
            admission=ids["admission"],
            executable=ids["catalog"],
        )
    )
    ids["admission_manifest"] = schema_id(admission_manifest)
    assert (
        evidence["catalog"] == ids["catalog"]
        and evidence["catalog_v2"] == ids["catalog_v2"]
    ), "CatalogIdentitiesRecomputed"

    def response(body):
        return dict(status=200, body=body, etag='"' + schema_id(body) + '"')

    expected["v1"]["schema"] = response(schema)
    for recipe in RECIPES:
        body = copy.deepcopy(schema)
        body["recipes"] = [r for r in body["recipes"] if r["recipe"] == recipe]
        expected["v1"]["filtered"][recipe] = response(body)
    expected["v1"]["conditional"]["etag"] = expected["v1"]["schema"]["etag"]
    expected["v1"]["definitions"]["llamacpp"] = ids["definition"]
    expected["v1"]["file_sha256"]["src/cpp/resources/backends/llamacpp.json"] = sha(
        git_bytes("test/fixtures/backend-v2/llamacpp.candidate.json")
    )
    expected["v1"]["cli_schema"]["stdout"] = serialized(schema, True) + "\n"
    old_catalog = original_schema["catalog_id"]
    for case in expected["v1"]["cli_validation"].values():
        for result in case.values():
            result["stdout"] = result["stdout"].replace(old_catalog, ids["catalog"])
    schema2 = copy.deepcopy(base["v2"]["schema2"]["body"])
    schema2.update(
        catalog_id=ids["catalog"],
        catalog_v2_id=ids["catalog_v2"],
        feature_manifest_id=ids["admission_manifest"],
        admission_manifest=admission_manifest,
    )
    schema2["recipes"][0] = dict(row, input_schema_version=2)
    expected["v2"]["schema2"] = response(schema2)
    for recipe in RECIPES:
        body = copy.deepcopy(schema2)
        body["recipes"] = [r for r in body["recipes"] if r["recipe"] == recipe]
        expected["v2"]["filtered2"][recipe] = response(body)
    expected["v2"]["conditional2"]["etag"] = expected["v2"]["schema2"]["etag"]
    return expected, ids


def diff(old, new, path=""):
    if type(old) is type(new) and isinstance(old, dict):
        result = []
        for k in sorted(old.keys() | new.keys()):
            p = path + "/" + k.replace("~", "~0").replace("/", "~1")
            if k not in old:
                result.append(dict(path=p, op="add", new=new[k]))
            elif k not in new:
                result.append(dict(path=p, op="remove", old=old[k]))
            else:
                result.extend(diff(old[k], new[k], p))
        return result
    if type(old) is type(new) and isinstance(old, list) and len(old) == len(new):
        return [
            r
            for i, (a, b) in enumerate(zip(old, new))
            for r in diff(a, b, path + "/" + str(i))
        ]
    if serialized(old) == serialized(new):
        return []
    return [dict(path=path, op="replace", old=old, new=new)]


def raw_snapshot(actual, first, second):
    for name, data in (("v1", first), ("v2", second)):
        assert data == (serialized(actual[name], True) + "\n").encode(), (
            "RawSnapshotSerializationExact:" + name
        )


def apply(base, changes):
    value = copy.deepcopy(base)
    for record in changes:
        tokens = [
            p.replace("~1", "/").replace("~0", "~")
            for p in record["path"].split("/")[1:]
        ]
        parent = value
        for p in tokens[:-1]:
            parent = parent[int(p)] if isinstance(parent, list) else parent[p]
        k = int(tokens[-1]) if isinstance(parent, list) else tokens[-1]
        if record["op"] != "add":
            assert serialized(parent[k]) == serialized(
                record["old"]
            ), "TransitionExactOldValue"
        if record["op"] == "remove":
            del parent[k]
        else:
            parent[k] = copy.deepcopy(record["new"])
    return value


def validate(manifest, base, actual, evidence, raw_base, raw_extra):
    prepared = json.loads(
        subprocess.check_output(
            [
                "git",
                "show",
                "c4f7ecdb9bcebcba35c1c3c9bcbadadd77f76a8f:test/fixtures/backend-v2/llamacpp.catalog-transition.json",
            ],
            cwd=REPO,
        )
    )
    assert set(manifest) == set(prepared) | {
        "base_representation2_sha256",
        "identities",
    }, "ExactTransitionManifestFields"
    for k in set(prepared) - {"stage", "changes"}:
        assert serialized(manifest[k]) == serialized(prepared[k]), (
            "PinnedTransitionProvenance:" + k
        )
    assert manifest["stage"] == "production-activation", "ProductionTransitionStage"
    assert (
        sha(raw_base) == manifest["snapshot_sha256"]
        and len(raw_base) == manifest["snapshot_bytes"]
    ), "PinnedWholeBaseSnapshot"
    assert (
        sha(raw_extra) == manifest["base_representation2_sha256"]
    ), "PinnedRepresentationTwoBaseline"
    expected, ids = expected_snapshot(base, evidence)
    assert manifest["identities"] == ids, "EveryAffectedIdentityRecomputed"
    records = manifest["changes"]
    assert len({r["path"] for r in records}) == len(
        records
    ), "NoDuplicateTransitionRecord"
    permitted = diff(base, expected)
    allowed = {r["path"]: r for r in permitted}
    for r in records:
        assert r["path"] in allowed, "NoForeignOrUnnecessaryTransitionRecord"
        assert serialized(r) == serialized(
            allowed[r["path"]]
        ), "NoStaleTransitionRecord"
    assert records == permitted, "CompleteTransitionRecordsNoMissingOrUnused"
    raw = diff(base, actual)
    assert raw == records, "CompleteRawSnapshotDiffEqualsDisclosure"
    assert serialized(apply(base, records), True) == serialized(
        actual, True
    ), "AppliedDisclosureReproducesWholeSnapshot"
    assert serialized(actual, True) == serialized(
        expected, True
    ), "IndependentlyDerivedWholeSnapshot"
    return ids, raw


def capture_all(args):
    verify(ROOT / "source", ROOT / "build")
    for prefix, build, source in (
        ("base", ROOT / "build", ROOT / "source"),
        ("current", args.build, REPO),
    ):
        first = capture(build / "lemond", build / "lemonade", source)
        second = extra(build / "lemond")
        (args.out / (prefix + "-snapshot.json")).write_text(
            serialized(first, True) + "\n"
        )
        (args.out / (prefix + "-extra.json")).write_text(
            serialized(second, True) + "\n"
        )
    env = dict(os.environ, LEMON_SWITCH_OUT=str(args.out / "identities.json"))
    result = subprocess.run(
        ["timeout", "120", str(args.build / "test_llamacpp_v2_production_switch")],
        env=env,
        capture_output=True,
        text=True,
        timeout=130,
    )
    (args.out / "production-switch.log").write_text(result.stdout + result.stderr)
    assert result.returncode == 0, "ActualProductionSwitchSuite"


def load(args):
    raw_base = (args.out / "base-snapshot.json").read_bytes()
    raw_extra = (args.out / "base-extra.json").read_bytes()
    base = dict(v1=json.loads(raw_base), v2=json.loads(raw_extra))
    actual = dict(
        v1=json.loads((args.out / "current-snapshot.json").read_bytes()),
        v2=json.loads((args.out / "current-extra.json").read_bytes()),
    )
    evidence = json.loads((args.out / "identities.json").read_bytes())
    return base, actual, evidence, raw_base, raw_extra


def mutations(args, manifest, values):
    base, actual, evidence, raw_base, raw_extra = values
    cases = []

    def mutated(name, assertion, edit):
        candidate = copy.deepcopy(manifest)
        edit(candidate)
        cases.append((name, assertion, candidate, actual))

    mutated(
        "boolean-schema-version",
        "PinnedTransitionProvenance:schema_version",
        lambda m: m.update(schema_version=True),
    )
    mutated(
        "float-schema-version",
        "PinnedTransitionProvenance:schema_version",
        lambda m: m.update(schema_version=1.0),
    )

    def replace_boolean(m):
        record = next(r for r in m["changes"] if r.get("new") is True)
        record["new"] = 1

    mutated("boolean-record-type", "NoStaleTransitionRecord", replace_boolean)
    mutated(
        "duplicate",
        "NoDuplicateTransitionRecord",
        lambda m: m["changes"].append(m["changes"][0]),
    )
    mutated(
        "missing",
        "CompleteTransitionRecordsNoMissingOrUnused",
        lambda m: m["changes"].pop(),
    )
    mutated(
        "foreign",
        "NoForeignOrUnnecessaryTransitionRecord",
        lambda m: m["changes"].append(dict(path="/v1/foreign", op="add", new=True)),
    )
    mutated(
        "unnecessary",
        "NoForeignOrUnnecessaryTransitionRecord",
        lambda m: m["changes"].append(
            dict(path="/v1/platform", op="replace", old="Linux", new="Linux")
        ),
    )
    mutated(
        "stale",
        "NoStaleTransitionRecord",
        lambda m: m["changes"][0].update(old="stale"),
    )
    mutated(
        "stale-identity",
        "EveryAffectedIdentityRecomputed",
        lambda m: m["identities"].update(execution="sha256:" + "0" * 64),
    )
    for name, edit in (
        ("undisclosed-output", lambda a: a["v1"].update(foreign=True)),
        (
            "stale-etag",
            lambda a: a["v2"]["filtered2"]["llamacpp-hrx"].update(
                etag=base["v2"]["filtered2"]["llamacpp-hrx"]["etag"]
            ),
        ),
    ):
        changed = copy.deepcopy(actual)
        edit(changed)
        cases.append(
            (name, "CompleteRawSnapshotDiffEqualsDisclosure", manifest, changed)
        )
    results = []
    for name, assertion, candidate, changed in cases:
        (args.scratch / (name + ".json")).write_text(
            serialized(dict(manifest=candidate, actual=changed), True) + "\n"
        )
        try:
            validate(candidate, base, changed, evidence, raw_base, raw_extra)
        except AssertionError as error:
            assert str(error) == assertion, (name, error)
        else:
            raise AssertionError("UncaughtTransitionMutation:" + name)
        results.append(dict(rule=name, assertion=assertion, caught=True))
    for name, assertion, edit in (
        (
            "descriptor-fields",
            "AllSeventeenDescriptorFieldsAndDefaultsUnchanged",
            lambda e: e["descriptors"][0].update(binary="wrong"),
        ),
        (
            "spec-aliases",
            "AllSpecAliasesUnchanged",
            lambda e: e["spec_aliases"][0].append("wrong"),
        ),
        ("v1-inventory", "EightV1IdsUnchanged", lambda e: e["v1_ids"].pop()),
        (
            "common-defaults",
            "CommonDefaultsUnchanged",
            lambda e: e["common_defaults"].update(ctx_size=0),
        ),
    ):
        changed = copy.deepcopy(evidence)
        edit(changed)
        (args.scratch / (name + ".json")).write_text(serialized(changed, True) + "\n")
        try:
            validate(manifest, base, actual, changed, raw_base, raw_extra)
        except AssertionError as error:
            assert str(error) == assertion, (name, error)
        else:
            raise AssertionError("UncaughtEvidenceMutation:" + name)
        results.append(dict(rule=name, assertion=assertion, caught=True))
    for name in ("v1", "v2"):
        raw = {k: (serialized(actual[k], True) + "\n").encode() for k in ("v1", "v2")}
        raw[name] += b" "
        try:
            raw_snapshot(actual, raw["v1"], raw["v2"])
        except AssertionError as error:
            assert str(error) == "RawSnapshotSerializationExact:" + name
        else:
            raise AssertionError("UncaughtRawSerializationMutation:" + name)
        results.append(
            dict(
                rule="raw-serialization-" + name,
                assertion="RawSnapshotSerializationExact:" + name,
                caught=True,
            )
        )
    (args.out / "transition-mutations.json").write_text(
        serialized(results, True) + "\n"
    )
    print(f"{len(results)} transition mutations caught")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gate", choices=["capture", "check", "mutations", "run"])
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--scratch",
        type=Path,
        default=REPO / ".synagent/scratch/backend-v2-4e3b/transition",
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
        capture_all(args)
        if args.gate == "capture":
            return
    baseline_tests = args.out / "base-cpp-ci.json"
    baseline_tests.write_text(
        subprocess.check_output(
            [
                "ctest",
                "--test-dir",
                str(ROOT / "build"),
                "-L",
                "^cpp-ci$",
                "--show-only=json-v1",
            ],
            text=True,
            timeout=30,
        )
    )
    from backend_v2_4e_audit import census

    census(
        SimpleNamespace(
            build=args.build, baseline=baseline_tests, expected_count=151, out=args.out
        )
    )
    values = load(args)
    manifest = json.loads(MANIFEST.read_bytes())
    ids, records = validate(manifest, *values)
    raw_snapshot(
        values[1],
        (args.out / "current-snapshot.json").read_bytes(),
        (args.out / "current-extra.json").read_bytes(),
    )
    source_preservation(args.out)
    purity(args.build, args.out)
    assert production_inventory() == V1_NAMES | {"llamacpp"}, "EightV1OneV2NineSources"
    commands = json.loads((args.build / "compile_commands.json").read_text())
    assert not any(
        "lemonade-server-core.dir" in r["command"]
        and r["file"].endswith("/llamacpp_server.cpp")
        for r in commands
    ), "WrapperAbsentFromProductionCompilation"
    for binary in ("lemond", "lemonade"):
        symbols = subprocess.check_output(
            ["nm", "-C", str(args.build / binary)], text=True, timeout=60
        )
        assert (
            "LlamaCppServer" not in symbols
            and "lemon::backends::llamacpp::ops()" not in symbols
        ), "NoLlamaWrapperSymbolsInProductionLinks"
    (args.out / "complete-raw-diff.json").write_text(serialized(records, True) + "\n")
    (args.out / "applied-snapshot.json").write_text(
        serialized(apply(values[0], records), True) + "\n"
    )
    current_bytes = (serialized(values[1], True) + "\n").encode()
    (args.out / "current-complete-snapshot.json").write_bytes(current_bytes)
    assert (
        args.out / "applied-snapshot.json"
    ).read_bytes() == current_bytes, "AppliedRawSnapshotBytesEqual"
    (args.out / "transition-result.json").write_text(
        serialized(
            dict(
                records=len(records),
                whole_snapshot_sha256=sha(current_bytes),
                identities=ids,
                complete_diff=True,
                application_exact=True,
            ),
            True,
        )
        + "\n"
    )
    if args.gate in ("mutations", "run"):
        mutations(args, manifest, values)
    print(
        f"Complete raw snapshot diff: {len(records)} exact records; application reproduces whole snapshot; {len(ids)} identities recomputed"
    )


if __name__ == "__main__":
    main()
