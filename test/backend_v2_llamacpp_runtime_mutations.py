import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CONFIGURED = "src/cpp/server/backends/configured/"


def mutations():
    runner = CONFIGURED + "backend_definition_runner.cpp"
    program = CONFIGURED + "backend_execution_program.cpp"
    services = CONFIGURED + "backend_runtime_services.cpp"
    typed = CONFIGURED + "typed/llamacpp_hooks.cpp"
    yield (
        "raw-completion",
        runner,
        "if (original_body && op.identity && op.remove.empty() && op.set.empty() && chain.empty())",
        "if (false)",
        "runtime_services",
        "CompletionStreamingPreservesEveryOriginalByte",
    )
    yield (
        "repository-timing-without-declaration",
        runner,
        "if (context.phase == InstallPhase::Asset || full) {\n                    request(request",
        "if (true) {\n                    request(request",
        "runtime_services",
        "RepositoryDoesNotEvaluateSelectedAssetExpression",
    )
    yield (
        "undeclared-repository-definition",
        program,
        "if (full) for (const auto& fact : facts) require",
        "if (false) for (const auto& fact : facts) require",
        "runtime_services",
        "FullParametersCannotObtainUndeclaredRepositoryFacts",
    )
    yield (
        "repository-only-declaration",
        program,
        "(full || declared.empty())",
        "true",
        "runtime_services",
        "RepositoryFactDeclarationRequiresFullParameters",
    )
    yield (
        "repository-without-contract",
        program,
        "declared.empty() || fact_hook",
        "true",
        "runtime_services",
        "RepositoryFactsRequireBoundInstallContract",
    )
    yield (
        "unused-fact",
        program,
        "require(used_facts.count(fact),",
        "require(true,",
        "runtime_services",
        "RepositoryFactDeclarationCannotContainUnusedCapability",
    )
    yield (
        "fallback-reference",
        program,
        "fallback.empty() || backends.count(fallback)",
        "true",
        "runtime_services",
        "InstallFallbackMustNameDeclaredBackendGroup",
    )
    yield (
        "unmanaged-overlap",
        program,
        "require(!backends.count(backend),",
        "require(true,",
        "runtime_services",
        "UnmanagedBackendCannotOverlapManagedCase",
    )
    yield (
        "managed-policy",
        program,
        'program.install.strategy == "github_release" ||',
        "true ||",
        "runtime_services",
        "LegacyInstallPoliciesRequireManagedReleases",
    )
    yield (
        "conditional-before-platform",
        program,
        "if (early) require(expression.kind !=",
        "if (false) require(expression.kind !=",
        "runtime_services",
        "BeforePlatformCannotEagerlyEvaluateConditionalFactBranches",
    )
    yield (
        "inconsistent-before-platform",
        program,
        "found == backend_facts.end() || found->second == facts",
        "true",
        "runtime_services",
        "BeforePlatformGroupRequiresIdenticalFactRequirements",
    )
    yield (
        "cross-backend-fact",
        program,
        "if (early) for (const auto& fact : facts) require",
        "if (false) for (const auto& fact : facts) require",
        "runtime_services",
        "BeforePlatformCannotReadAnotherBackendFact",
    )
    yield (
        "full-repository-result",
        runner,
        "if (context.phase == InstallPhase::Repository && !full) return",
        "if (context.phase == InstallPhase::Repository) return",
        "runtime_services",
        "DeclaredRepositoryFactProducesFullPinnedParameters",
    )
    yield (
        "repository-pinned-version",
        runner,
        "context.phase == InstallPhase::Repository ? context.pinned_version : context.resolved_version",
        "context.resolved_version",
        "runtime_services",
        "DeclaredRepositoryFactProducesFullPinnedParameters",
    )
    yield (
        "early-facts",
        runner,
        'if (early) read_facts(*eligible.front());\n    filter("os", context.platform.os);',
        'filter("os", context.platform.os);\n    if (early) read_facts(*eligible.front());',
        "runtime_services",
        "DeclaredBeforePlatformFactPrecedesDeclaredPlatformError",
    )
    yield (
        "install-fallback",
        runner,
        'if (!has_backend(backend) && policy.contains("backend_fallback"))',
        "if (false)",
        "runtime_services",
        "DeclaredFallbackAppliesOnlyToInstallEvaluation",
    )
    yield (
        "declared-errors",
        runner,
        "if (errors.contains(key)) throw",
        "if (false) throw",
        "runtime_services",
        "DeclaredBeforePlatformFactPrecedesDeclaredPlatformError",
    )
    yield (
        "undeclared-service-fact",
        services,
        "if ((input_.phase != typed::InstallPhase::Asset &&",
        "if ((false &&",
        "runtime_services",
        "RepositoryServiceRejectsAssetFact",
    )
    yield (
        "declared-service-fact",
        services,
        "std::find(input_.repository_facts.begin(), input_.repository_facts.end(), fact) == input_.repository_facts.end()",
        "true",
        "runtime_services",
        "RepositoryServiceAcceptsOnlyDeclaredRequestedFact",
    )
    yield (
        "environment-parent-set",
        services,
        "key != typed::ParentKey::LibraryPath &&",
        "true &&",
        "runtime_services",
        "EnvironmentEnumeratedParentKeySetExact",
    )
    yield (
        "environment-runtime-set",
        services,
        "query != typed::RuntimeQuery::CudaArchitecture)",
        "true)",
        "runtime_services",
        "EnvironmentEnumeratedRuntimeQuerySetExact",
    )
    yield (
        "cuda-parent-override",
        typed,
        'services.parent(ParentKey::CudaVisibleDevices).value_or("").empty()',
        "true",
        "hook_abi",
        "CUDA parent override suppresses architecture and device queries",
    )
    yield (
        "cuda-query-order",
        typed,
        "services.parent(windows ? ParentKey::Path : ParentKey::LibraryPath)",
        "services.parent(ParentKey::Path)",
        "hook_abi",
        "CUDA environment uses exact enumerated keys and ordered queries",
    )
    yield (
        "runtime-input-policy",
        runner,
        'program_.launch.records.at("custom_args").value("runtime_input", "preserve") == "string_or_empty"',
        "false",
        "runtime_services",
        "FrozenRuntimeParityAll116Cases",
    )
    yield (
        "runtime-fact-namespace",
        runner,
        'frame.data.values["runtime"]["dflash"] = facts.dflash;',
        'frame.data.values["model"]["dflash"] = facts.dflash;',
        "runtime_services",
        "FrozenRuntimeParityAll116Cases",
    )
    yield (
        "response-error-normalization",
        runner,
        "    runtime::check_cancelled(cancel);\n    const auto& op = cell(operation, OperationDelivery::Buffered",
        '    if (response.contains("error")) return response;\n    runtime::check_cancelled(cancel);\n    const auto& op = cell(operation, OperationDelivery::Buffered',
        "runtime_services",
        "DeclaredResponseNormalizationConsumesErrorObjects",
    )
    yield (
        "strict-direct-stream",
        runner,
        "if (!legacy || !endpoint(path))",
        "if (!endpoint(path))",
        "runtime_services",
        "DefaultDirectStreamingStillRejectsUnknownEndpoints",
    )
    yield (
        "builtin-only-stream-policy",
        CONFIGURED + "backend_definition_execution.cpp",
        'source.key.space == DefinitionNamespace::Builtin && source.key.id == "llamacpp" &&\n                    staging.normalized["recipe"] == "llamacpp"',
        "true",
        "runtime_services",
        "LegacyDirectStreamingPolicyIsBuiltinOnly",
    )
    yield (
        "spec-alias-confinement",
        runner,
        "!valid_spec_aliases(*executable_, context.spec) ||",
        "",
        "runtime_services",
        "EmptyAliasesRemainBuiltinLlamaOnly",
    )

    yield (
        "runtime-fact-fields",
        CONFIGURED + "backend_definition_policy.cpp",
        '{"dflash", "mtp"}',
        '{"dflash", "mtp", "draft"}',
        "runtime_services",
        "RuntimeFactsCannotInventFields",
    )
    yield (
        "runtime-fact-stage",
        CONFIGURED + "backend_execution_expression.cpp",
        'at.rfind("/launch/custom_args/defaults/", 0) == 0 && use == 0',
        "true",
        "runtime_services",
        "RuntimeFactsCannotLeakIntoLaunchDefaults",
    )

    yield (
        "typed-repository-denial",
        typed,
        "input.phase != InstallPhase::Asset &&",
        "false &&",
        "hook_abi",
        "install phase mismatch fails closed",
    )
    yield (
        "typed-repository-opt-in",
        typed,
        "std::find(input.repository_facts.begin(), input.repository_facts.end(), fact) == input.repository_facts.end()",
        "true",
        "hook_abi",
        "declared repository phase fact exception is bounded",
    )
    yield (
        "staging-false-return",
        services,
        "BackendUtils::stage_therock_hip_runtime(architecture, utils::path_from_utf8(destination));",
        'if (!BackendUtils::stage_therock_hip_runtime(architecture, utils::path_from_utf8(destination))) throw std::runtime_error("staging false");',
        "runtime_services",
        "StagingFalseResultRetainsLegacyNonFailure",
    )

    yield (
        "home-fallback",
        typed,
        'home.empty() ? "/var/root" : home',
        "home",
        "hook_abi",
        "MissingPasswdHomeRetainsLegacyFallback",
    )


def run(command, cwd, log, env, timeout=180):
    with log.open("w") as output:
        return subprocess.run(
            ["timeout", str(timeout), *command],
            cwd=cwd,
            env=env,
            stdout=output,
            stderr=subprocess.STDOUT,
            check=False,
        ).returncode


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=ROOT / "build")
    parser.add_argument(
        "--scratch", type=Path, default=ROOT / ".synagent/scratch/backend-v2-4e2b"
    )
    parser.add_argument(
        "--out", type=Path, default=ROOT / ".synagent/artifacts/backend-v2-4e2b"
    )
    parser.add_argument("--only")
    args = parser.parse_args()
    build = args.build.resolve()
    scratch, out = args.scratch.resolve(), args.out.resolve()
    for path in (scratch, out):
        if path == Path("/tmp") or Path("/tmp") in path.parents:
            raise ValueError("Use persistent project storage")
        path.mkdir(parents=True, exist_ok=True)
    temp = scratch / "temp"
    temp.mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp))
    commands = json.loads((build / "compile_commands.json").read_text())
    results = []
    for name, relative, before, after, suite, assertion in mutations():
        if args.only and name not in args.only.split(","):
            continue
        directory = scratch / "mutations" / name
        directory.mkdir(parents=True, exist_ok=True)
        source = ROOT / relative
        original = source.read_text()
        if original.count(before) != 1:
            raise ValueError(f"Mutation anchor must be unique: {name}")
        copied = directory / source.name
        copied.write_text(original.replace(before, after, 1))
        target = "test_backend_" + suite
        candidates = [row for row in commands if Path(row["file"]) == source]
        entry = next(
            (
                row
                for row in candidates
                if "lemonade-typed-hook-test.dir" in row["command"]
            ),
            candidates[0],
        )
        compile_command = shlex.split(entry["command"])
        old_object = compile_command[compile_command.index("-o") + 1]
        obj = directory / "mutant.o"
        compile_command[compile_command.index("-o") + 1] = str(obj)
        compile_command[compile_command.index(str(source))] = str(copied)
        compile_command.extend(["-iquote", str(source.parent)])
        compile_command = [
            "-O0" if token.startswith("-O") else token for token in compile_command
        ]
        record = {
            "rule": name,
            "source": relative,
            "before": before,
            "after": after,
            "assertion": assertion,
        }
        record["compile"] = run(compile_command, build, directory / "compile.log", env)
        if record["compile"] == 0:
            lines = subprocess.check_output(
                ["ninja", "-C", str(build), "-t", "commands", target], text=True
            ).splitlines()
            link = shlex.split(lines[-1])
            start = next(
                i
                for i, token in enumerate(link)
                if Path(token).name in ("c++", "g++", "clang++")
            )
            link = link[start:]
            if "&&" in link:
                link = link[: link.index("&&")]
            link[link.index("-o") + 1] = str(directory / "probe")
            if old_object in link:
                link[link.index(old_object)] = str(obj)
            else:
                link.insert(1, str(obj))
            record["link"] = run(link, build, directory / "link.log", env)
            if record["link"] == 0:
                record["run"] = run(
                    [str(directory / "probe")], ROOT, directory / "run.log", env, 120
                )
                record["caught"] = (
                    record["run"] == 1
                    and "FAIL " + assertion in (directory / "run.log").read_text()
                )
        results.append(record)
        (out / "runtime-mutations.json").write_text(
            json.dumps(results, indent=2) + "\n"
        )
        print(
            f"{name}: {'caught' if record.get('caught') else 'NOT CAUGHT'}", flush=True
        )
    if not results or not all(row.get("caught") for row in results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
