"""Kill each remaining prerequisite mutation at its named runtime assertion."""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

REPO = Path(__file__).resolve().parents[1]
CONFIGURED = REPO / "src/cpp/server/backends/configured"
RUNNER = CONFIGURED / "backend_definition_runner.cpp"
SUITE = "test_container_prerequisites"


def rules():
    return [
        (
            "ds4-memory-phase-order-ignored",
            CONFIGURED / "backend_execution_program.cpp",
            'std::find(order.begin(), order.end(), "backend") < std::find(order.begin(), order.end(), "artifacts")',
            "true",
            "Ds4ClosedContract:memory-before-backend",
        ),
        (
            "ds4-missing-memory-defaulted",
            RUNNER,
            '#include "ds4_contract_internal.h"',
            None,
            [
                "Ds4MissingMemoryFactRejected:main",
                "Ds4MissingMemoryFactRejected:free-known",
            ],
            "ds4_contract_internal.h",
            'if (!value) throw std::runtime_error(std::string("Missing DS4 memory fact: ") + name);',
            "if (!value) return T{};",
        ),
        (
            "ds4-memory-pool-ignored",
            RUNNER,
            '#include "ds4_contract_internal.h"',
            None,
            "Ds4SourceMemoryPlan:resident-at-threshold",
            "ds4_contract_internal.h",
            "const auto pool = available ? (std::max)(vram, window) : 0.0;",
            "const auto pool = available ? vram : 0.0;",
        ),
        (
            "ds4-draft-weight-ignored",
            RUNNER,
            '#include "ds4_contract_internal.h"',
            None,
            "Ds4SourceMemoryPlan:draft-counts-for-residency",
            "ds4_contract_internal.h",
            'const auto support = draft.empty() ? ArtifactByteFact{true, 0} : ds4_memory_fact(services.artifact_bytes(draft), "draft_bytes");',
            "const auto support = ArtifactByteFact{true, 0};",
        ),
        (
            "ds4-residency-forced-streaming",
            RUNNER,
            '#include "ds4_contract_internal.h"',
            None,
            "Ds4SourceMemoryPlan:resident-at-threshold",
            "ds4_contract_internal.h",
            "const bool resident = pool > 0.0 && weights > 0.0 && pool >= weights + 16;",
            "const bool resident = false;",
        ),
        (
            "ds4-buffered-rewrite-dropped",
            RUNNER,
            "if (op.content_transform == execution::ContentTransform::Ds4NoThinkV1) body = ds4_without_no_think_prefix(std::move(body));",
            "if (op.content_transform == execution::ContentTransform::Ds4NoThinkV1 && delivery == OperationDelivery::Streaming) body = ds4_without_no_think_prefix(std::move(body));",
            "Ds4BufferedRewriteWireBodyAllUserMessages",
        ),
        (
            "ds4-streaming-rewrite-dropped",
            RUNNER,
            "if (op.content_transform == execution::ContentTransform::Ds4NoThinkV1) body = ds4_without_no_think_prefix(std::move(body));",
            "if (op.content_transform == execution::ContentTransform::Ds4NoThinkV1 && delivery == OperationDelivery::Buffered) body = ds4_without_no_think_prefix(std::move(body));",
            "Ds4StreamingRewriteWireBodyAllUserMessages",
        ),
        (
            "ds4-malformed-json-passthrough-broken",
            RUNNER,
            "if (passthrough && parsed.is_discarded()) return",
            "if (passthrough && parsed.is_discarded() && op.content_transform == execution::ContentTransform::None) return",
            "Ds4MalformedJsonStreamingPassthrough:{",
        ),
        (
            "ds4-last-message-only",
            RUNNER,
            '#include "ds4_contract_internal.h"',
            None,
            "Ds4BufferedRewriteWireBodyAllUserMessages",
            "ds4_contract_internal.h",
            'for (auto& message : request["messages"]) {',
            'for (auto& message : request["messages"]) { if (&message != &request["messages"].back()) continue;',
        ),
        (
            "ds4-other-roles-rewritten",
            RUNNER,
            '#include "ds4_contract_internal.h"',
            None,
            "Ds4BufferedRewriteWireBodyAllUserMessages",
            "ds4_contract_internal.h",
            'message.value("role", "") == "user"',
            "true",
        ),
        (
            "ds4-content-owner-unsealed",
            CONFIGURED / "backend_definition_authorization.cpp",
            '    if (!transforms.empty()) result["content_transforms"] = std::move(transforms);',
            "",
            "Ds4ContentTransformIsSealed",
        ),
        (
            "ds4-memory-owner-unsealed",
            CONFIGURED / "backend_definition_authorization.cpp",
            '{"execution", launch.at("execution")}',
            '{"execution", [&] { auto value = launch.at("execution"); for (auto& item : value["cases"]) item.erase("memory_plan"); return value; }()}',
            "Ds4MemoryPlanIsSealed",
        ),
        (
            "halogen-management-defaults-lost",
            RUNNER,
            'std::string DefinitionRunner::validate_file(const std::string& path) const {\n    if (halogen_contract()) return "";',
            "std::string DefinitionRunner::validate_file(const std::string& path) const {",
            "HalogenManagementDefaultsNeedNoArtifactHook",
        ),
        (
            "halogen-native-metadata-ignored",
            RUNNER,
            "if (halogen_contract()) { model.max_context_window = 262144; return; }",
            "if (halogen_contract()) { return; }",
            "HalogenNativeContextMetadata",
        ),
        (
            "halogen-conditional-environment-ignored",
            RUNNER,
            "        if (assignment.condition && !evaluate(*assignment.condition, frame).value().get<bool>()) continue;",
            "",
            "HalogenTextBundleOmitsVision",
        ),
        (
            "exclusive-host-unenforced",
            REPO / "src/cpp/server/router.cpp",
            "        enforce_host_exclusivity_locked(slot_policy_for_recipe(model_info.recipe), canonical_model_name, requested_residency_class);",
            "",
            "RouterSecondExclusiveAdmissionRefused",
        ),
        (
            "host-exclusion-one-way",
            REPO / "src/cpp/server/router.cpp",
            "(incoming != SlotPolicy::ExclusiveHost && policy != SlotPolicy::ExclusiveHost)",
            "(incoming != SlotPolicy::ExclusiveHost)",
            "RouterLocalAdmissionEvictsExclusive",
        ),
        (
            "context-only-at-launch",
            RUNNER,
            '        if (!value.is_number_integer() || value.get<int>() <= 0) options.set_option("ctx_size", 262144);',
            "",
            "HalogenContextBeforeRouterMemoryAdmission",
        ),
        (
            "hgn-bundle-selection-ignored",
            RUNNER,
            '#include "halogen_model_contract_internal.h"',
            None,
            "HgnBundleExactSourceSelection",
            'if (file == variant || hgn_suffix(file, "-ngram.hgn") || hgn_suffix(file, "-vision.hgn") ||\n            file.rfind("tokenizer/", 0) == 0)',
            "if (file == variant)",
        ),
        (
            "halogen-extra-ignored",
            RUNNER,
            '#include "halogen_model_contract_internal.h"',
            None,
            "HalogenMissingTableExtraRejected",
            'const auto table = model.extra<std::string>("halogen_lookup_table", "");',
            'const std::string table = "table-ngram.hgn";',
        ),
        (
            "halogen-host-check-ignored",
            RUNNER,
            "                    if (halogen_contract()) check_halogen_host(*plan.container, services);",
            "",
            "HalogenHostFailure:svm-missing",
        ),
        (
            "halogen-cwsr-check-ignored",
            RUNNER,
            '#include "halogen_model_contract_internal.h"',
            None,
            "HalogenHostFailure:cwsr-wrong",
            'if (!reported || values["cwsr_size"] != values["ctl_stack_size"] + (values["simd_count"] / 2) * 479232ULL)',
            "if (false)",
        ),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("build", "out", "scratch"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--rule", action="append")
    args = parser.parse_args()
    args.build, args.out, args.scratch = (
        p.resolve() for p in (args.build, args.out, args.scratch)
    )
    for path in (args.out, args.scratch, args.scratch / "temporary"):
        path.mkdir(parents=True, exist_ok=True)
    os.environ.update(
        TMPDIR=str(args.scratch / "temporary"),
        TMP=str(args.scratch / "temporary"),
        TEMP=str(args.scratch / "temporary"),
    )

    def run(name, command, cwd):
        with (args.out / (name + ".log")).open("w") as log:
            result = subprocess.run(
                ["timeout", "180", *map(str, command)],
                cwd=cwd,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=185,
            )
        return result.returncode

    assert (
        run("control", [args.build / SUITE], REPO) == 0
    ), "PrerequisiteControlMustPass"
    commands = subprocess.check_output(
        ["ninja", "-t", "commands", SUITE], cwd=args.build, text=True, timeout=30
    ).splitlines()
    link = shlex.split(commands[-1].removeprefix(": && ").removesuffix(" && :"))
    link = [x for x in link if not x.startswith("-Wl,--dependency-file=")]
    database = json.loads((args.build / "compile_commands.json").read_bytes())
    results = []
    for name, source, old, new, assertion, *header_change in rules():
        if args.rule and name not in args.rule:
            continue
        original = source.read_bytes()
        content = original.decode()
        assert content.count(old) == 1, (name, content.count(old))
        work = args.scratch / name
        work.mkdir(exist_ok=True)
        resources = work / "resources"
        if not resources.exists():
            resources.symlink_to(args.build / "resources", target_is_directory=True)
        if header_change:
            if len(header_change) == 3:
                header_name, a, b = header_change
            else:
                header_name, (a, b) = "halogen_model_contract_internal.h", header_change
            header = CONFIGURED / header_name
            header_text = header.read_text()
            assert header_text.count(a) == 1
            (work / header.name).write_text(header_text.replace(a, b))
            new = '#include "' + str(work / header.name) + '"'
        mutant, obj, binary = work / source.name, work / "mutant.o", work / SUITE
        mutant.write_text(content.replace(old, new))
        candidates = [r for r in database if Path(r["file"]) == source]
        row = next(
            (r for r in candidates if "lemonade-server-core.dir" in r["command"]), None
        )
        if row is None:
            row = next((r for r in candidates if SUITE + ".dir" in r["command"]), None)
        if row is None:
            row = next(
                r for r in candidates if "lemonade-binding-test.dir" in r["command"]
            )
        command = shlex.split(row["command"])
        old_object = command[command.index("-o") + 1]
        command = [
            str(mutant) if x == str(source) else str(obj) if x == old_object else x
            for x in command
        ]
        command.insert(1, "-I" + str(source.parent))
        assert run(name + ".compile", command, Path(row["directory"])) == 0, (
            name + ": compile failure is not a kill"
        )
        command = [str(obj) if x == old_object else x for x in link]
        if old_object not in link:
            command.insert(command.index("-o"), str(obj))
        command[command.index("-o") + 1] = str(binary)
        assert run(name + ".link", command, args.build) == 0, (
            name + ": link failure is not a kill"
        )
        code = run(name + ".run", [binary], REPO)
        assertions = assertion if isinstance(assertion, list) else [assertion]
        assert code == 1 and all(
            "FAIL " + label in (args.out / (name + ".run.log")).read_text()
            for label in assertions
        ), (name, code, assertion)
        assert source.read_bytes() == original, "ProductionSourceUntouched"
        results.append(dict(rule=name, assertion=assertion, exit=code, caught=True))
        (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(name, "caught by", assertion, flush=True)


if __name__ == "__main__":
    main()
