"""CPU fake-engine proof through installed-registry production binaries and HTTP."""

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from backend_catalog_http import Server, free_port, prepare, request
from llamacpp_external_fork import run

REPO = Path(__file__).resolve().parents[1]
MARKER = "external-fork-cpu-4g-73f6"


def hashes(build):
    return {
        name: hashlib.sha256((build / name).read_bytes()).hexdigest()
        for name in ("lemond", "lemonade")
    }


def binary_invariance(before, after):
    assert (
        before.keys() == after.keys() == {"lemond", "lemonade"} and before == after
    ), "ProductionBinaryHashesInvariant"


def edit(op, path, value=None, **kwargs):
    result = dict(op=op, path=path, **kwargs)
    if value is not None:
        result["value"] = value
    return result


def security_cases(fork, child):
    cases = []

    def case(name, code, mutate, source=fork):
        value = copy.deepcopy(source)
        mutate(value)
        cases.append((name, code, value))

    case(
        "unexported-hrx-parent",
        "unexported_builtin",
        lambda d: d["extends"].update(builtin="hrx"),
    )
    case(
        "origin-promotion", "unknown_key", lambda d: d["merge"].update(origin="builtin")
    )
    case(
        "compatibility-attachment",
        "unknown_key",
        lambda d: d["merge"].update(compatibility="legacy_non_chat_sse"),
    )
    case(
        "grant-growth",
        "authored_hook_selection",
        lambda d: d["edits"].append(
            edit("add", "/hooks/system", dict(id="llamacpp.system.v1", params={}))
        ),
    )
    case(
        "hook-re-authoring",
        "authored_hook_selection",
        lambda d: d["edits"].append(
            edit("replace", "/hooks/backend", dict(id="llamacpp.backend.v1", params={}))
        ),
    )
    case(
        "hook-relocation",
        "authored_hook_selection",
        lambda d: d["edits"].append(
            edit(
                "add",
                "/hooks/install_context",
                dict(id="llamacpp.backend.v1", params={}),
            )
        ),
    )
    case(
        "mandatory-slot-removal",
        "mandatory_slot_removed",
        lambda d: d["edits"].append(edit("remove", "/hooks/backend")),
    )
    case(
        "mandatory-artifact-removal",
        "sealed_data_changed",
        lambda d: d["edits"].append(edit("remove", "/artifacts/main")),
    )
    case(
        "owner-laundering",
        "identity_protected",
        lambda d: d["edits"].append(
            edit(
                "replace",
                "/operations",
                "laundered-owner",
                match={"id": "chat.buffered"},
                at="/id",
            )
        ),
    )
    case(
        "builtin-shadowing",
        "id_collision",
        lambda d: d.update(id="llamacpp", recipe="llamacpp"),
    )
    case(
        "parent-pin-reuse",
        "invalid_local_executable_profile",
        lambda d: d["merge"]["install"].update(pin_recipe="llamacpp"),
        child,
    )
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument(
        "--out",
        type=Path,
        default=REPO / ".synagent/artifacts/backend-v2-4h/production",
    )
    parser.add_argument(
        "--scratch",
        type=Path,
        default=REPO / ".synagent/scratch/backend-v2-4h/production",
    )
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    root = (args.scratch / str(time.time_ns())).resolve()
    root.mkdir(parents=True)
    os.environ.update(TMPDIR=str(root), TMP=str(root), TEMP=str(root))
    tempfile.tempdir = str(root)
    env = dict(
        os.environ,
        GGML_DISABLE_VULKAN="parent-sentinel",
        HF_HUB_CACHE=str(root / "hf-empty"),
    )
    Path(env["HF_HUB_CACHE"]).mkdir()
    config = prepare(root)
    with Server(args.build / "lemond", root, env_overrides=env) as server:
        status, before_schema, _ = request(
            server.port, "/v1/backends/schema?representation=2"
        )
        assert (
            status == 200 and len(before_schema["recipes"]) == 17
        ), "ProductionBaselineHasNoFork"
        health = request(server.port, "/v1/health")[1]
        assert health["all_models_loaded"] == [], "ProductionBaselineNoModels"
    before = hashes(args.build)
    source = REPO / "test/fixtures/backend-v2/external"
    fork = json.loads((source / "research-llama.json").read_bytes())
    child = json.loads((source / "fork-b.json").read_bytes())
    directory = config / "backends.d"
    directory.mkdir()
    for name in ("research-llama", "fork-b"):
        shutil.copyfile(source / (name + ".json"), directory / (name + ".json"))
    after_declaration = hashes(args.build)
    binary_invariance(before, after_declaration)
    fake = root / "research-llama-server"
    shutil.copy2(args.build / "test_external_fake_engine", fake)
    transcript = root / "process.json"
    artifact = root / "cpu-fake.fixture"
    artifact.write_text("CPU fake artifact; no model\n")
    settings = json.loads((config / "config.json").read_bytes())
    settings.update(
        {
            "research-llama": {"cpu_bin": str(fake)},
            "global_timeout": 3,
            "auto_evict": False,
        }
    )
    (config / "config.json").write_text(json.dumps(settings))
    (config / "user_models.json").write_text(
        json.dumps(
            {
                "ActivationFake": {
                    "recipe": "research-llama",
                    "checkpoint": str(artifact),
                    "source": "local_path",
                    "labels": ["chat"],
                }
            }
        )
    )
    security = []
    with Server(args.build / "lemond", root, env_overrides=env) as server:
        status, schema, _ = request(server.port, "/v1/backends/schema?representation=2")
        assert (
            status == 200 and len(schema["recipes"]) == 19
        ), "ProductionForkDiscoveredAndFinalized"
        assert (
            schema["export_registry_id"]
            == "sha256:d644387f2d2838a76ace64304318e673244e8cd117e496ae4394dc0792e2b9fa"
        ), "ProductionRegistryExact"
        for name, code, candidate in security_cases(fork, child):
            status, result, _ = request(
                server.port, "/v1/backends/validate", body=candidate
            )
            codes = [r["code"] for r in result["diagnostics"]]
            assert status == 400 and result["valid"] is False and code in codes, (
                "ProductionSecurity:" + name,
                status,
                result,
            )
            assert (
                result["catalog_id"] == schema["catalog_id"]
            ), "RejectedCandidateRetainsProductionGeneration"
            security.append(dict(rule=name, code=code, result=result))
        (args.out / "security.json").write_text(json.dumps(security, indent=2) + "\n")
        options = dict(
            model_name="user.ActivationFake",
            ctx_size=512,
            research_llama_backend="cpu",
            research_llama_args="--test-transcript " + str(transcript),
        )
        status, loaded, _ = request(server.port, "/v1/load", body=options)
        assert status == 200, ("ProductionForkLoadsCPUFake", status, loaded)
        body = dict(
            model="user.ActivationFake",
            messages=[dict(role="user", content="hello CPU fake")],
            max_completion_tokens=7,
        )
        status, response, _ = request(server.port, "/v1/chat/completions", body=body)
        assert status == 200, ("ProductionForkInheritedProtocol", status, response)
        process = json.loads(transcript.read_bytes())
        expected = dict(body, max_tokens=7)
        assert process["requests"] == [
            dict(path="/v1/chat/completions", body=expected)
        ], "ProductionInheritedMaxTokenAlias"
        assert response == dict(
            id=MARKER,
            model="user.ActivationFake",
            choices=[dict(message=dict(role="assistant", content=MARKER))],
        ), "ProductionInheritedClientModelTransform"
        assert (
            process["path"] == str(fake)
            and process["sha256"] == hashlib.sha256(fake.read_bytes()).hexdigest()
            and process["pid"] != server.process.pid
        ), "ProductionForkOwnProcessIdentity"
        argv = process["argv"]
        port = argv[argv.index("--port") + 1]
        expected_argv = [
            "-m",
            str(artifact),
            "--ctx-size",
            "512",
            "--port",
            port,
            "--jinja",
            "--metrics",
            "--test-transcript",
            str(transcript),
            "--parallel",
            "2",
            "--cache-type-k",
            "q8_0",
            "--cache-type-v",
            "q8_0",
        ]
        assert argv == expected_argv, "ProductionForkExactArgv"
        assert (
            process["environment"]["GGML_DISABLE_VULKAN"] == "1"
        ), "ProductionForkChildEnvironment"
        status, unloaded, _ = request(
            server.port, "/v1/unload", body=dict(model_name="user.ActivationFake")
        )
        assert status == 200, ("ProductionForkUnloads", status, unloaded)
        try:
            request(int(port), "/health", key=None)
        except OSError:
            pass
        else:
            raise AssertionError("ProductionForkTeardownStopsProcess")
    bad = copy.deepcopy(fork)
    bad["edits"].append(
        edit("replace", "/hooks/backend", dict(id="llamacpp.backend.v1", params={}))
    )
    (directory / "research-llama.json").write_text(json.dumps(bad))
    code, output = run(
        [
            "timeout",
            "15",
            str(args.build / "lemond"),
            str(root / "cache"),
            str(config),
            "--host",
            "127.0.0.1",
            "--port",
            str(free_port()),
            "--no-broadcast",
            "--log-file",
            "disabled",
        ],
        env,
        args.out / "protected-hook-startup.log",
        timeout=20,
    )
    assert (
        code not in (0, 124) and "authored_hook_selection" in output
    ), "ProductionProtectedHookRejectedBeforePublication"
    after = hashes(args.build)
    binary_invariance(before, after)
    mutation = root / "binary-hash-mutation"
    mutation.mkdir()
    for name in before:
        shutil.copyfile(args.build / name, mutation / name)
    mutation_before = hashes(mutation)
    with (mutation / "lemond").open("ab") as changed:
        changed.write(b"deliberate declaration-time binary mutation")
    mutation_after = hashes(mutation)
    try:
        binary_invariance(mutation_before, mutation_after)
    except AssertionError as error:
        assert str(error) == "ProductionBinaryHashesInvariant"
    else:
        raise AssertionError("BinaryHashMutationEscaped")
    for path in (
        args.build / "lemond",
        args.build / "liblemonade-backend-definition-v2.a",
    ):
        symbols = subprocess.check_output(
            ["nm", "-C", str(path)], text=True, timeout=30
        )
        assert (
            "test_selected_builtin_exports" not in symbols
            and "external_fixture::" not in symbols
        ), "ProductionNoTestRegistryOrFixtureSymbols"
    database = json.loads((args.build / "compile_commands.json").read_bytes())
    assert not any(
        "LEMON_BACKEND_FORK_TESTING" in r["command"] for r in database
    ), "NoRegistryInjectionCompileDefinition"
    result = dict(
        cpu_only=True,
        real_model=False,
        production_binary=True,
        hashes_before=before,
        hashes_after_declaration=after_declaration,
        hashes_after=after,
        binary_hash_mutation_caught=True,
        binary_hash_mutation=dict(before=mutation_before, after=mutation_after),
        process=process,
        response=response,
        security_rules=len(security),
        protected_hook_startup_exit=code,
        baseline_catalog=before_schema["catalog_id"],
        fork_catalog=schema["catalog_id"],
        scratch=str(root),
    )
    (args.out / "production.json").write_text(json.dumps(result, indent=2) + "\n")
    print(
        "Production fork: discovery, exact subprocess transcript, security, protected-hook control and unchanged binary hashes passed"
    )


if __name__ == "__main__":
    main()
