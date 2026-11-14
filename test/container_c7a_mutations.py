"""Require normal exit 1 and a named failing assertion for each C6 mutation."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

REPO = Path(__file__).resolve().parents[1]
CONFIGURED = REPO / "src/cpp/server/backends/configured"
SUITE = "test_container_c7a"


def rules():
    return [
        (
            "external-stream-escape",
            "backend_definition_execution.cpp",
            'if (staging.normalized["launch"].value("direct_stream_policy", "strict") == "builtin_container_legacy")\n                execution::require(source.key.space == DefinitionNamespace::Builtin && container_capable(staging.normalized) &&\n                    AuthorityInspection::permits_execution(*authorized, staging.normalized),',
            'if (staging.normalized["launch"].value("direct_stream_policy", "strict") == "builtin_container_legacy")\n                execution::require(true,',
            "C7aExternalCannotStream",
        ),
        (
            "ungranted-stream",
            "backend_definition_execution.cpp",
            'if (staging.normalized["launch"].value("direct_stream_policy", "strict") == "builtin_container_legacy")\n                execution::require(source.key.space == DefinitionNamespace::Builtin && container_capable(staging.normalized) &&\n                    AuthorityInspection::permits_execution(*authorized, staging.normalized),',
            'if (staging.normalized["launch"].value("direct_stream_policy", "strict") == "builtin_container_legacy")\n                execution::require(source.key.space == DefinitionNamespace::Builtin && container_capable(staging.normalized) &&\n                    true,',
            "C7aUngrantedIdentityCannotStream",
        ),
        (
            "unrequested-stream",
            "backend_definition_runner.cpp",
            "if (!legacy || !endpoint(path))",
            "if (!endpoint(path))",
            "C7aUnrequestedEndpointUnreachable",
        ),
        (
            "stream-policy-unsealed",
            "backend_definition_authorization.cpp",
            'if (launch.value("direct_stream_policy", "strict") == "builtin_container_legacy") {',
            "if (false) {",
            "C7aStreamPolicySealed",
        ),
        (
            "stream-forwarding-lost",
            "backend_definition_runner.cpp",
            'policy == "llamacpp_legacy" || policy == "builtin_container_legacy"',
            'policy == "llamacpp_legacy"',
            "C7aPipeline",
        ),
        (
            "load-log-lost-on-start-failure",
            "configured_server.cpp",
            "        if (log && log->starting) log->starting(category, prepared.session->frame.values, plan.command);",
            "",
            "C7aStartingBeforeProcessFailureExactOrder",
        ),
        (
            "load-log-wrong-phase",
            "configured_server.cpp",
            "        const auto prepared = entry_->runner_->prepare_load",
            '        if (log && log->starting) log->starting(category, json{{"model", {{"name", name}}}}, ServerCommand{});\n        const auto prepared = entry_->runner_->prepare_load',
            "C7aNoStartingBeforeArtifactAdmission",
        ),
        (
            "load-log-binding-missing",
            "configured_v2_backend_ops.cpp",
            "launch_compatibility({{}, false, runner_->builtin_load_log()})",
            "launch_compatibility({})",
            "C7aV2BuiltinLoadLogBound",
        ),
        (
            "load-log-owner-unsealed",
            "backend_definition_authorization.cpp",
            'if (launch.contains("builtin_load_log"))',
            "if (false)",
            "C7aLoadLogBindingSealed",
        ),
        (
            "external-load-log-escape",
            "backend_definition_execution.cpp",
            'execution::require(source.key.space == DefinitionNamespace::Builtin && container_capable(staging.normalized) &&\n                    staging.normalized.value("model_contract", "") == "halogen_hgn_v1" &&\n                    AuthorityInspection::permits_execution(*authorized, staging.normalized),',
            "execution::require(true,",
            "C7aExternalLoadLogRejected",
        ),
        (
            "budget-seam-ignored",
            "configured_v2_backend_ops.cpp",
            "if (memory_services_) return runner_->auto_context_kv_budget_gib(model, available_gib, *memory_services_);",
            "if (memory_services_) return std::nullopt;",
            "C7aEstimatorUsesEarlierBudgetInsteadOfZero",
        ),
        (
            "estimator-ignores-budget",
            "test/cpp/test_container_c7a.cpp",
            "speculative, unified_gpu, kv_budget);",
            "speculative, unified_gpu, std::nullopt);",
            "C7aEstimatorUsesEarlierBudgetInsteadOfZero",
            "src/cpp/include/lemon/auto_tune.h",
            "lemon/auto_tune.h",
        ),
        (
            "budget-reserve-dropped",
            "backend_definition_runner.cpp",
            'return available_gib - facts.at("free_reserve_gb").get<int>() - working_set;',
            "return available_gib - working_set;",
            "C7aSourceBudgetIsFiveGiB",
        ),
        (
            "budget-nullopt-dropped",
            "backend_definition_runner.cpp",
            "if (working_set <= 0.0) return std::nullopt;",
            "if (working_set < 0.0) return std::nullopt;",
            "C7aSourceContextBudget:no-working-set",
        ),
        (
            "budget-profile-ignored",
            "backend_execution_program.cpp",
            "program.model_contract = ModelContract::Ds4RocmV1;",
            "program.model_contract = ModelContract::None;",
            "C7aSourceBudgetIsFiveGiB",
        ),
        (
            "budget-owner-unsealed",
            "backend_definition_authorization.cpp",
            'if (declaration.contains("model_contract")) {',
            'if (declaration.contains("model_contract") && declaration.at("model_contract") != "ds4_rocm_v1") {',
            "C7aBudgetSelectionSealed",
        ),
        (
            "ungranted-budget",
            "backend_definition_execution.cpp",
            'if (staging.normalized.value("model_contract", "") == "ds4_rocm_v1")\n                execution::require(source.key.space == DefinitionNamespace::Builtin && container_capable(staging.normalized) &&\n                    AuthorityInspection::permits_execution(*authorized, staging.normalized),',
            'if (staging.normalized.value("model_contract", "") == "ds4_rocm_v1")\n                execution::require(true,',
            "C7aUngrantedBudgetRejected",
        ),
        (
            "budget-metadata-labels-changed",
            "backend_definition_runner.cpp",
            "if (!ds4_contract()) model.labels = result.labels;",
            "model.labels = result.labels;",
            "C7aDs4MetadataPreservesRegisteredLabels",
        ),
        (
            "budget-draft-weight-ignored",
            "backend_definition_runner.cpp",
            'const auto support = draft.empty() ? ArtifactByteFact{true, 0} : ds4_memory_fact(services.artifact_bytes(draft), "draft_bytes");',
            "const auto support = ArtifactByteFact{true, 0};",
            "C7aSourceContextBudget:draft-forces-streaming",
            "src/cpp/server/backends/configured/ds4_contract_internal.h",
            "ds4_contract_internal.h",
        ),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--rule", action="append")
    args = parser.parse_args()
    args.build, args.out, args.scratch = (
        p.resolve() for p in (args.build, args.out, args.scratch)
    )
    for path in (args.out, args.scratch, args.scratch / "temporary"):
        path.mkdir(parents=True, exist_ok=True)
    os.environ.update(
        TMPDIR=str(args.scratch / "temporary"),
        TEMP=str(args.scratch / "temporary"),
        TMP=str(args.scratch / "temporary"),
    )

    def run(name, command, cwd):
        with (args.out / (name + ".log")).open("w") as log:
            return subprocess.run(
                ["timeout", "180", *map(str, command)],
                cwd=cwd,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=185,
            )

    links = {}
    for suite in (SUITE,):
        assert (
            run(suite + ".control", [args.build / suite], REPO).returncode == 0
        ), "C7aControlMustPass"
        commands = subprocess.check_output(
            ["ninja", "-t", "commands", suite], cwd=args.build, text=True, timeout=30
        ).splitlines()
        link = shlex.split(commands[-1].removeprefix(": && ").removesuffix(" && :"))
        links[suite] = [x for x in link if not x.startswith("-Wl,--dependency-file=")]
    database = json.loads((args.build / "compile_commands.json").read_bytes())
    results = []
    for name, filename, old, new, assertion, *header in rules():
        if args.rule and name not in args.rule:
            continue
        suite = SUITE
        link = links[suite]
        assertions = assertion if isinstance(assertion, list) else [assertion]
        source = (
            REPO / filename if filename.startswith("test/") else CONFIGURED / filename
        )
        original = source.read_bytes()
        original_header = (REPO / header[0]).read_bytes() if header else None
        text = original_header.decode() if header else original.decode()
        assert text.count(old) == 1, (name, text.count(old))
        work = args.scratch / name
        work.mkdir(exist_ok=True)
        resources = work / "resources"
        if not resources.exists():
            resources.symlink_to(args.build / "resources", target_is_directory=True)
        mutant, obj, binary = work / source.name, work / "mutant.o", work / suite
        if header:
            mutated_header = work / Path(header[0]).name
            header_text = text.replace(old, new)
            if header[1] == "lemon/auto_tune.h":
                # The linker can otherwise select an unmodified weak inline copy.
                header_text = header_text.replace(
                    "inline int64_t compute_auto_context_size(",
                    "static inline int64_t compute_auto_context_size(",
                )
            mutated_header.write_text(header_text)
            mutant.write_text(
                original.decode().replace(
                    '#include "' + header[1] + '"',
                    '#include "' + str(mutated_header) + '"',
                )
            )
        else:
            mutant.write_text(text.replace(old, new))
        row = next(
            r
            for r in database
            if Path(r["file"]) == source
            and (
                "test_container_c7a.dir"
                if filename == "backend_definition_authorization.cpp"
                or filename.startswith("test/")
                else (
                    "lemonade-binding-test.dir"
                    if filename
                    in (
                        "backend_definition_execution.cpp",
                        "backend_execution_program.cpp",
                    )
                    else "lemonade-server-core.dir"
                )
            )
            in r["command"]
        )
        compile_command = shlex.split(row["command"])
        old_object = compile_command[compile_command.index("-o") + 1]
        compile_command = [
            str(mutant) if x == str(source) else str(obj) if x == old_object else x
            for x in compile_command
        ]
        compile_command.insert(1, "-I" + str(CONFIGURED))
        compile_command.insert(1, "-I" + str(REPO / "test/cpp"))
        command = [str(obj) if x == old_object else x for x in link]
        if old_object not in link:
            command.insert(command.index("-o"), str(obj))
        command[command.index("-o") + 1] = str(binary)
        if header and header[1] == "lemon/auto_tune.h":
            mutated_header.write_text(
                text.replace(
                    "inline int64_t compute_auto_context_size(",
                    "static inline int64_t compute_auto_context_size(",
                )
            )
            assert (
                run(
                    name + ".isolated-control-compile",
                    compile_command,
                    Path(row["directory"]),
                ).returncode
                == 0
            ), "IsolatedEstimatorControlCompiles"
            assert (
                run(name + ".isolated-control-link", command, args.build).returncode
                == 0
            ), "IsolatedEstimatorControlLinks"
            assert (
                run(name + ".isolated-control-run", [binary], REPO).returncode == 0
            ), "IsolatedEstimatorControlPasses"
            mutated_header.write_text(header_text)
        assert (
            run(name + ".compile", compile_command, Path(row["directory"])).returncode
            == 0
        ), (name + ": compile failure is not a kill")
        assert run(name + ".link", command, args.build).returncode == 0, (
            name + ": link failure is not a kill"
        )
        result = run(name + ".run", [binary], REPO)
        assert result.returncode == 1 and all(
            "FAIL " + label in (args.out / (name + ".run.log")).read_text()
            for label in assertions
        ), (name, result.returncode, assertion)
        assert (
            hashlib.sha256(source.read_bytes()).digest()
            == hashlib.sha256(original).digest()
        ), "ProductionSourceUntouched"
        if header:
            assert (
                REPO / header[0]
            ).read_bytes() == original_header, "ProductionHeaderUntouched"
        results.append(
            dict(
                rule=name,
                source=str(source.relative_to(REPO)),
                assertion=assertion,
                exit=result.returncode,
                caught=True,
            )
        )
        (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(name + ": caught by " + ", ".join(assertions), flush=True)
    print(f"{len(results)}/{len(results)} C7a compiled mutations caught")


if __name__ == "__main__":
    main()
