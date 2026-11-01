"""Require normal exit 1 and a named failing assertion for each C7b mutation."""

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
    header = "src/cpp/server/backends/configured/backend_diagnostics_internal.h"
    return [
        (
            "external-diagnostics",
            "backend_definition_execution.cpp",
            'if (staging.normalized["launch"].contains("diagnostics"))\n                execution::require(source.key.space == DefinitionNamespace::Builtin && container_capable(staging.normalized) &&\n                    AuthorityInspection::permits_execution(*authorized, staging.normalized),',
            'if (staging.normalized["launch"].contains("diagnostics"))\n                execution::require(true,',
            "C7bExternalDiagnosticsRejected",
        ),
        (
            "ungranted-diagnostics",
            "backend_definition_execution.cpp",
            'if (staging.normalized["launch"].contains("diagnostics"))\n                execution::require(source.key.space == DefinitionNamespace::Builtin && container_capable(staging.normalized) &&\n                    AuthorityInspection::permits_execution(*authorized, staging.normalized),',
            'if (staging.normalized["launch"].contains("diagnostics"))\n                execution::require(source.key.space == DefinitionNamespace::Builtin && container_capable(staging.normalized) && true,',
            "C7bUngrantedDiagnosticsRejected",
        ),
        (
            "local-builtin-diagnostics",
            "backend_definition_execution.cpp",
            'if (staging.normalized["launch"].contains("diagnostics"))\n                execution::require(source.key.space == DefinitionNamespace::Builtin && container_capable(staging.normalized) &&\n                    AuthorityInspection::permits_execution(*authorized, staging.normalized),',
            'if (staging.normalized["launch"].contains("diagnostics"))\n                execution::require(source.key.space == DefinitionNamespace::Builtin && true &&\n                    AuthorityInspection::permits_execution(*authorized, staging.normalized),',
            "C7bLocalBuiltinDiagnosticsRejected",
        ),
        (
            "diagnostics-unsealed",
            "backend_definition_authorization.cpp",
            'if (launch.contains("diagnostics")) {',
            "if (false) {",
            "C7bDiagnosticFieldSealed:format",
        ),
        (
            "producer-unsealed",
            "backend_definition_authorization.cpp",
            '{"custom_args", custom}',
            '{"custom_args", nlohmann::json()}',
            "C7bDiagnosticOptionProducerSealed",
        ),
        (
            "arbitrary-format",
            "test/cpp/test_container_c7a.cpp",
            "require(format.size() <= 256 && std::find(diagnostic_formats().begin(), diagnostic_formats().end(), format) != diagnostic_formats().end(),",
            "require(true,",
            "C7bArbitraryFormatRejected",
            header,
            "backend_diagnostics_internal.h",
        ),
        (
            "unbounded-fact",
            "test/cpp/test_container_c7a.cpp",
            "require(std::find(facts.begin(), facts.end(), fact) != facts.end(),",
            "require(true,",
            "C7bFactOutsideClosedSetRejected",
            header,
            "backend_diagnostics_internal.h",
        ),
        (
            "wrong-phase",
            "test/cpp/test_container_c7a.cpp",
            'require(text != "port:integer" || rank >= 5,',
            "require(true,",
            "C7bFactBeforeItsPhaseRejected",
            header,
            "backend_diagnostics_internal.h",
        ),
        (
            "wrong-order",
            "test/cpp/test_container_c7a.cpp",
            "require(rank >= previous,",
            "require(true,",
            "C7bDiagnosticOrderRejected",
            header,
            "backend_diagnostics_internal.h",
        ),
        (
            "record-bound",
            "test/cpp/test_container_c7a.cpp",
            "require(records.size() <= 32,",
            "require(true,",
            "C7bRecordCountBounded",
            header,
            "backend_diagnostics_internal.h",
        ),
        (
            "id-bound",
            "test/cpp/test_container_c7a.cpp",
            "!id.empty() && id.size() <= 64 && ids.insert(id).second",
            "!id.empty() && ids.insert(id).second",
            "C7bDiagnosticIdBounded",
            header,
            "backend_diagnostics_internal.h",
        ),
        (
            "phase-domain",
            "test/cpp/test_container_c7a.cpp",
            "require(position != phases.end(),",
            "require(true,",
            "C7bUnknownPhaseRejected",
            header,
            "backend_diagnostics_internal.h",
        ),
        (
            "severity-domain",
            "test/cpp/test_container_c7a.cpp",
            'levels.count(record.at("level")) && categories.count(record.at("category"))',
            'categories.count(record.at("category"))',
            "C7bUnknownLevelRejected",
            header,
            "backend_diagnostics_internal.h",
        ),
        (
            "condition-domain",
            "test/cpp/test_container_c7a.cpp",
            'require(condition == "always" ||',
            'require(true || condition == "always" ||',
            "C7bUnknownConditionRejected",
            header,
            "backend_diagnostics_internal.h",
        ),
        (
            "category-domain",
            "test/cpp/test_container_c7a.cpp",
            'levels.count(record.at("level")) && categories.count(record.at("category"))',
            'levels.count(record.at("level"))',
            "C7bCategoryDomainClosed",
            header,
            "backend_diagnostics_internal.h",
        ),
        (
            "duplicate-id",
            "test/cpp/test_container_c7a.cpp",
            "!id.empty() && id.size() <= 64 && ids.insert(id).second",
            "!id.empty() && id.size() <= 64",
            "C7bDuplicateDiagnosticRejected",
            header,
            "backend_diagnostics_internal.h",
        ),
        (
            "diagnostic-line-bound",
            "backend_definition_runner.cpp",
            "if (line.size() + text.size() > 32768)",
            "if (false)",
            "C7bDiagnosticLineByteBound",
        ),
        (
            "mixed-bindings",
            "test/cpp/test_container_c7a.cpp",
            'require(!launch.contains("diagnostics") || !launch.contains("builtin_load_log"),',
            "require(true,",
            "C7bMixedBindingsRejected",
            header,
            "backend_diagnostics_internal.h",
        ),
        (
            "mixed-generic-logs",
            "test/cpp/test_container_c7a.cpp",
            'require(!launch.contains("diagnostics") || launch.value("load_log_policy", "none") == "none",',
            "require(true,",
            "C7bMixedGenericLogsRejected",
            header,
            "backend_diagnostics_internal.h",
        ),
        (
            "diagnostic-keying",
            "backend_definition_edits.cpp",
            '"/launch/diagnostics", ',
            "",
            "C7bDiagnosticsAreKeyedRecords",
        ),
        (
            "ds4-image-fact",
            "backend_definition_runner.cpp",
            'values["diagnostic_facts"]["image"] = plan.container->image;',
            'values["diagnostic_facts"]["image"] = "wrong-image";',
            "C7bExactOrderedDiagnostics:ds4",
        ),
        (
            "halogen-bytes-changed",
            "backend_definition_runner.cpp",
            "text = std::to_string(command.port);",
            'text = std::to_string(command.port + (program_.launch.records.contains("diagnostics") ? 1 : 0));',
            "C7bHalogenC7aByteAndOrderIdentity",
        ),
        (
            "installation-phase-skipped",
            "backend_definition_runner.cpp",
            'if (plan.container && records.contains("diagnostics")) {',
            "if (false) {",
            "C7bInstallFailureDiagnosticAdmission:halobox",
        ),
        (
            "installation-repeated",
            "backend_definition_runner.cpp",
            "container_installed = true;",
            "container_installed = false;",
            "C7bContainerImageInstalledOnce:ds4",
        ),
        (
            "success-lost",
            "configured_server.cpp",
            "if (log && log->success) log->success(category, prepared.session->frame.values, plan.command);",
            "",
            [
                "C7bExactOrderedDiagnostics:rocmfpx",
                "C7bExactOrderedDiagnostics:halobox",
                "C7bExactOrderedDiagnostics:nathanw",
            ],
        ),
        (
            "failure-lost",
            "configured_server.cpp",
            "if (log && log->failure) log->failure(category, prepared.session->frame.values, plan.command);",
            "",
            "C7bFailurePhaseAfterStarting",
        ),
        (
            "diagnostic-fact-bound",
            "backend_definition_runner.cpp",
            "if (text.size() > 16384)",
            "if (false)",
            "C7bDiagnosticFactByteBound",
        ),
        (
            "custom-condition-ignored",
            "backend_definition_runner.cpp",
            'if (condition != "always" && !facts.at(condition).get<bool>()) continue;',
            'if (condition != "always" && condition != "custom_present" && !facts.at(condition).get<bool>()) continue;',
            "C7bEmptyCustomDiagnosticsSuppressed:halobox",
        ),
        (
            "rewrite-domain-unsealed",
            "backend_definition_authorization.cpp",
            'if (operation.at("request").contains("rewrite_domain"))',
            "if (false)",
            "C7bRewriteDomainSealed",
        ),
        (
            "external-rewrite",
            "backend_definition_execution.cpp",
            'for (const auto& operation : staging.normalized.at("operations")) if (operation.at("request").contains("rewrite_domain"))\n                execution::require(source.key.space == DefinitionNamespace::Builtin && container_capable(staging.normalized) &&\n                    AuthorityInspection::permits_execution(*authorized, staging.normalized),',
            'for (const auto& operation : staging.normalized.at("operations")) if (operation.at("request").contains("rewrite_domain"))\n                execution::require(true,',
            "C7bExternalRewriteDomainRejected",
        ),
        (
            "rewrite-arbitrary-paths",
            "backend_definition_runner.cpp",
            'op.rewrite_domain == execution::RewriteDomain::ChatCompletionsSubstring && path.find("chat/completions") != std::string::npos',
            "op.rewrite_domain == execution::RewriteDomain::ChatCompletionsSubstring",
            "C7bExactRewriteWire:ds4/arbitrary/unrelated",
        ),
        (
            "rewrite-model-cell",
            "backend_definition_runner.cpp",
            "(op.description.model_types & operation_model_type_mask(model_type)) &&",
            "",
            "C7bRewriteDomainBoundToModelCell",
        ),
        (
            "rewrite-extra-hooks",
            "backend_execution_program.cpp",
            'request.value("hooks", Json::array()).empty()',
            "true",
            "C7bRewriteRejectsAdditionalRequestHooks",
        ),
        (
            "rewrite-query-lost",
            "backend_execution_program.cpp",
            "op.rewrite_domain = RewriteDomain::ChatCompletionsSubstring;",
            "op.rewrite_domain = RewriteDomain::CanonicalPath;",
            "C7bExactRewriteWire:ds4/v1/chat/completions?probe=1",
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
        ), "C7bControlMustPass"
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
            if header[1] == "backend_diagnostics_internal.h":
                header_text = header_text.replace("inline ", "static inline ")
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
                    else (
                        "lemonade-backend-definition-v2.dir"
                        if filename == "backend_definition_edits.cpp"
                        else "lemonade-server-core.dir"
                    )
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
    print(f"{len(results)}/{len(results)} C7b compiled mutations caught")


if __name__ == "__main__":
    main()
