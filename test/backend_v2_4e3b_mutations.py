"""Compile one activation regression at a time without altering the production tree."""

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

from backend_definition_inventory import V1_NAMES, production_inventory
from llamacpp_frozen_control import REPO
from llamacpp_oracle_audit import link_command

CONFIGURED = "src/cpp/server/backends/configured/"


def run(command, cwd, log, expected=0, assertion=None):
    process = subprocess.run(
        command, cwd=cwd, capture_output=True, text=True, timeout=180
    )
    output = process.stdout + process.stderr
    log.write_text(output)
    assert process.returncode == expected, (command, process.returncode, output[-3000:])
    if assertion:
        assert assertion in output, (assertion, output[-3000:])
    return output


def compile_mutant(args, source, destination, target, text=None):
    original = REPO / source
    entry = next(
        r
        for r in args.database
        if Path(r["file"]) == original
        and (
            target + ".dir" in r["command"]
            or "lemonade-server-core.dir" in r["command"]
            or "lemonade-backend-definition-v2.dir" in r["command"]
        )
    )
    command = shlex.split(entry["command"])
    obj = command[command.index("-o") + 1]
    path = destination / original.name
    path.write_text(original.read_text() if text is None else text)
    mutant_obj = destination / Path(obj).name
    command[command.index("-c") + 1] = str(path)
    command[command.index("-o") + 1] = str(mutant_obj)
    command = ["-O0" if x.startswith("-O") else x for x in command]
    command.extend(["-iquote", str(original.parent)])
    run(command, args.build, destination / "compile.log")
    return obj, mutant_obj


def binary(
    args,
    name,
    source,
    old,
    new,
    assertion,
    target="test_llamacpp_v2_production_switch",
    header=None,
    wrapper=False,
):
    work = args.scratch / name
    work.mkdir(parents=True, exist_ok=True)
    resources = work / "resources"
    if not resources.exists():
        resources.symlink_to(args.build / "resources", target_is_directory=True)
    text = (REPO / source).read_text()
    assert old in text, (name, old)
    text = text.replace(old, new, 1)
    if wrapper:
        text = '#include "lemon/backends/llamacpp/llamacpp_server.h"\n' + text
    if header:
        path, before, after = header
        value = path.read_text()
        assert before in value, name
        (work / path.name).write_text(value.replace(before, after, 1))
    obj, mutant = compile_mutant(args, source, work, target, text)
    command = link_command(args.build, target)
    if obj in command:
        command[command.index(obj)] = str(mutant)
    else:
        archive = args.build / "liblemonade-backend-definition-v2.a"
        assert str(archive.name) in command, (name, obj)
        replacement = work / archive.name
        shutil.copyfile(archive, replacement)
        run(
            ["ar", "r", str(replacement), str(mutant)], args.build, work / "archive.log"
        )
        command = [str(replacement) if x == archive.name else x for x in command]
    if wrapper:
        wrapper_source = REPO / "src/cpp/server/backends/llamacpp/llamacpp_server.cpp"
        entry = next(
            r
            for r in args.database
            if r["file"].endswith("/legacy_llamacpp_artifact_ops.cpp")
            and "lemonade-server-core.dir" in r["command"]
        )
        compile_command = shlex.split(entry["command"])
        wrapper_obj = work / "wrapper.o"
        compile_command[compile_command.index("-o") + 1] = str(wrapper_obj)
        compile_command[compile_command.index("-c") + 1] = str(wrapper_source)
        run(compile_command, args.build, work / "wrapper-compile.log")
        command.insert(command.index("-o"), str(wrapper_obj))
    probe = work / "probe"
    command[command.index("-o") + 1] = str(probe)
    run(command, args.build, work / "link.log")
    expected = 2 if name == "transport-session-required" else 1
    run(
        ["timeout", "120", str(probe)],
        args.build,
        work / "run.log",
        expected,
        assertion,
    )
    if name in ("missing-policy", "stale-source-policy"):
        run(
            ["timeout", "120", str(probe), "--assert-unavailable"],
            args.build,
            work / "fail-closed.log",
        )
    result = dict(
        rule=name,
        compiled=True,
        linked=True,
        exit=expected,
        assertion=assertion,
        caught=True,
    )
    print(result, flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument(
        "--scratch",
        type=Path,
        default=REPO / ".synagent/scratch/backend-v2-4e3b/mutations",
    )
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.scratch = args.scratch.resolve()
    args.scratch.mkdir(parents=True, exist_ok=True)
    args.out.mkdir(parents=True, exist_ok=True)
    temp = args.scratch / "tmp"
    temp.mkdir(exist_ok=True)
    os.environ.update(TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp))
    args.database = json.loads((args.build / "compile_commands.json").read_text())
    results = []
    cases = [
        (
            "missing-policy",
            CONFIGURED + "backend_builtin_authorization.cpp",
            "return fixed_builtin_v2_policies();",
            "static const BuiltinExecutionPolicies empty{}; return empty;",
            "ExactlyOneFixedBuiltinPolicy",
        ),
        (
            "stale-source-policy",
            CONFIGURED + "backend_llamacpp_policy.cpp",
            "sha256:e8ea19f3af8db0b892cb5d3c9d5e217c944517734fbe26027d7270742e2371da",
            "sha256:" + "0" * 64,
            "ProductionSwitch:",
        ),
        (
            "duplicate-recipe",
            CONFIGURED + "backend_catalog.cpp",
            "!seed_recipes.insert(seed.descriptor->recipe).second",
            "false",
            "FailedBuiltinOrDescendantPublishesNothing:duplicate-recipe",
        ),
        (
            "ambiguous-legacy-arm",
            CONFIGURED + "backend_catalog.cpp",
            "seed.legacy.create || seed.legacy.spec || seed.legacy.ops || seed.legacy.configured_entry",
            "false",
            "FailedBuiltinOrDescendantPublishesNothing:ambiguous-legacy-arm",
        ),
        (
            "premature-builtin-read",
            "src/cpp/server/backends/backend_registry.cpp",
            "    catalog_publication().server();\n",
            "",
            "ReadsDuringBuildingFailWithoutFallback",
        ),
        (
            "metadata-manifest",
            CONFIGURED + "backend_catalog_projection.cpp",
            '"execution_finalization"',
            '"declaration_authorization"',
            "NestedAdmissionManifestReportsFinalizationAndVersions",
        ),
        (
            "metadata-executable-versions",
            CONFIGURED + "backend_catalog_projection.cpp",
            '{"executable_definition_versions", {1, 2}}',
            '{"executable_definition_versions", {1}}',
            "NestedAdmissionManifestReportsFinalizationAndVersions",
        ),
    ]
    cases.append(
        (
            "descendant-before-parent",
            CONFIGURED + "backend_catalog_execution.cpp",
            "for (const auto& program : compiled) {",
            "auto reversed = compiled; std::reverse(reversed.begin(), reversed.end()); for (const auto& program : reversed) {",
            "FailingDescendantBindingRejectsAlreadyBoundParent",
            "test_backend_catalog_execution",
        )
    )
    cases.append(
        (
            "auxiliary-role-dispatch",
            CONFIGURED + "backend_definition_runner.cpp",
            'const auto owner = declared ? role : "main";',
            'const auto owner = role == "mmproj" || role == "draft" ? "main" : role;',
            "RegistryAuxiliaryRolesUseTheBoundMainArtifactInventory",
            "test_backend_runtime_services",
        )
    )
    cases.append(
        (
            "npu-cache-resolution",
            CONFIGURED + "backend_definition_runner.cpp",
            'if (context.type == "npu_cache") return "";',
            "",
            "NpuSideCacheHasNoLocalArtifactResolution",
            "test_backend_runtime_services",
        )
    )
    for case in cases:
        results.append(binary(args, *case))
        (args.out / "compiled-mutations.json").write_text(
            json.dumps(results, indent=2) + "\n"
        )
    test = "test/cpp/test_llamacpp_v2_production_switch.cpp"
    results.append(
        binary(
            args,
            "wrong-capability-mask",
            test,
            "const auto op = BackendOperation::Embeddings",
            "const auto op = BackendOperation::Embeddings",
            "CapabilityMaskIndependentlyMatchesFinalizedBindingsAndTypedAdapters",
            header=(
                args.build / "include/backend_capabilities_generated.h",
                "lemon::CAP_EMBEDDINGS | lemon::CAP_RERANKING",
                "0",
            ),
        )
    )
    results.append(
        binary(
            args,
            "wrapper-fallback",
            "src/cpp/server/router.cpp",
            '    auto fallback = backends::create_server("llamacpp", ctx);',
            '    return std::make_unique<backends::LlamaCppServer>(log_level, model_manager_, backend_manager_);\n    auto fallback = backends::create_server("llamacpp", ctx);',
            "NoLlamaWrapperFallback",
            wrapper=True,
            header=None,
        )
    )
    results.append(
        binary(
            args,
            "transport-session-required",
            "test/cpp/test_llamacpp_oracle_transport.cpp",
            '#include "configured_server_transport_fixture.h"',
            '#include "configured_server_transport_fixture.h"',
            "Configured v2 session is not loaded",
            target="test_llamacpp_oracle_transport",
            header=(
                REPO / "test/cpp/configured_server_transport_fixture.h",
                "configured.load(name, model, options);",
                "(void)configured;",
            ),
        )
    )
    service = CONFIGURED + "backend_artifact_services.cpp"
    results.append(
        binary(
            args,
            "malformed-optional-metadata",
            service,
            "    catch (const std::invalid_argument&) { return {}; }",
            "    catch (const std::invalid_argument&) { throw; }",
            "MalformedOptionalMetadataRetainsLegacySkipBehavior",
            target="test_backend_runtime_services",
        )
    )
    results.append(
        binary(
            args,
            "truncated-optional-metadata",
            service,
            "    catch (const std::length_error&) { return {}; }",
            "    catch (const std::length_error&) { throw; }",
            "MalformedOptionalMetadataRetainsLegacySkipBehavior",
            target="test_backend_runtime_services",
        )
    )
    (args.out / "compiled-mutations.json").write_text(
        json.dumps(results, indent=2) + "\n"
    )
    work = args.scratch / "inventory"
    sources = work / "src/cpp/resources/backends"
    sources.mkdir(parents=True, exist_ok=True)
    fixture = work / "test/fixtures/backend-v2"
    fixture.mkdir(parents=True, exist_ok=True)
    for name in V1_NAMES | {"llamacpp"}:
        shutil.copyfile(
            REPO / f"src/cpp/resources/backends/{name}.json", sources / (name + ".json")
        )
    shutil.copyfile(
        REPO / "test/fixtures/backend-v2/llamacpp.candidate.json",
        fixture / "llamacpp.candidate.json",
    )
    assert production_inventory(work) == V1_NAMES | {"llamacpp"}
    (sources / "foreign.json").write_text("{}")
    try:
        production_inventory(work)
    except AssertionError as error:
        assert str(error) == "ExactMixedDefinitionSources"
    else:
        raise AssertionError("Uncaught extra catalog inventory entry")
    results.append(
        dict(
            rule="extra-catalog-inventory-entry",
            assertion="ExactMixedDefinitionSources",
            caught=True,
        )
    )
    (sources / "foreign.json").unlink()
    with (sources / "llamacpp.json").open("a") as out:
        out.write("\n")
    try:
        production_inventory(work)
    except AssertionError as error:
        assert str(error) == "SoleAdditionalSourceIsQualifiedLlamaCandidate"
    else:
        raise AssertionError("Uncaught changed production candidate")
    results.append(
        dict(
            rule="changed-production-candidate",
            assertion="SoleAdditionalSourceIsQualifiedLlamaCandidate",
            caught=True,
        )
    )
    (args.out / "mutations.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"{len(results)} activation mutations caught")


if __name__ == "__main__":
    main()
