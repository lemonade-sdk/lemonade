"""Compile independent HRX regressions without changing the production checkout."""

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

from llamacpp_frozen_control import REPO
from llamacpp_oracle_audit import link_command

RUNNER = "src/cpp/server/backends/configured/backend_definition_runner.cpp"
POLICY = "src/cpp/server/backends/configured/backend_llamacpp_policy.cpp"


def run(command, cwd, log):
    result = subprocess.run(
        command, cwd=cwd, capture_output=True, text=True, timeout=180
    )
    log.write_text(result.stdout + result.stderr)
    return result


def probe(args, name, source, old, new, assertion, target="test_hrx_oracle"):
    work = args.scratch / name
    work.mkdir(parents=True, exist_ok=True)
    text = (REPO / source).read_text()
    assert text.count(old) == 1, (name, text.count(old))
    path = work / Path(source).name
    path.write_text(text.replace(old, new, 1))
    entry = next(row for row in args.database if row["file"] == str(REPO / source))
    command = shlex.split(entry["command"])
    original = command[command.index("-o") + 1]
    obj = work / Path(original).name
    command[command.index("-o") + 1] = str(obj)
    command[command.index("-c") + 1] = str(path)
    command = ["-O0" if arg.startswith("-O") else arg for arg in command]
    command.extend(["-iquote", str((REPO / source).parent)])
    result = run(command, args.build, work / "compile.log")
    assert result.returncode == 0, (name, "compile", result.stderr[-2000:])
    command = link_command(args.build, target)
    if original in command:
        command[command.index(original)] = str(obj)
    else:
        archive = "liblemonade-backend-definition-v2.a"
        assert archive in command
        replacement = work / archive
        shutil.copyfile(args.build / archive, replacement)
        result = run(
            ["ar", "r", str(replacement), str(obj)], args.build, work / "archive.log"
        )
        assert result.returncode == 0, (name, "archive")
        command = [str(replacement) if arg == archive else arg for arg in command]
    binary = work / "probe"
    command[command.index("-o") + 1] = str(binary)
    result = run(command, args.build, work / "link.log")
    assert result.returncode == 0, (name, "link", result.stderr[-2000:])
    resources = work / "resources"
    if not resources.exists():
        resources.symlink_to(args.build / "resources", target_is_directory=True)
    result = run(["timeout", "120", str(binary)], args.build, work / "run.log")
    output = result.stdout + result.stderr
    assert result.returncode == 1 and "FAIL " + assertion in output, (
        name,
        result.returncode,
        assertion,
        output[-3000:],
    )
    record = dict(rule=name, assertion=assertion, exit=1, compiled=True, caught=True)
    (work / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    args.results.append(record)
    (args.out / "compiled-mutations.json").write_text(
        json.dumps(args.results, indent=2) + "\n"
    )
    print(record, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument(
        "--scratch",
        type=Path,
        default=REPO / ".synagent/scratch/backend-v2-4f/mutations",
    )
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--only", nargs="*")
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.scratch = args.scratch.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    args.scratch.mkdir(parents=True, exist_ok=True)
    temp = args.scratch / "tmp"
    temp.mkdir(exist_ok=True)
    os.environ.update(TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp))
    args.database = json.loads((args.build / "compile_commands.json").read_bytes())
    args.results = []
    launch = "HRX builds the complete managed argv and --parallel default:"
    entries = [
        (
            "parent-policy-reused",
            POLICY,
            "hrx.owners = policy.owners;",
            'hrx = policy; hrx.key.id = "hrx";',
            "HrxHasIndependentFixedLeafPins",
            "test_backend_definition_builtin_authorization",
        ),
        (
            "main-owner-policy-copied",
            POLICY,
            'restricted.owner_semantics_digest = "sha256:6e0097ab682dbe72aaae22f86d944afc77cabd89b8819b55d25a5107f3577103";',
            "restricted.owner_semantics_digest = slot.owner_semantics_digest;",
            "HrxMainRequiredSemanticsAreRepinned",
            "test_backend_definition_builtin_authorization",
        ),
        (
            "runtime-noop-removed",
            RUNNER,
            "if (!runtime_hook && !runtime_default && !normalize) return;",
            "if (false) return;",
            "HrxGainedNothing:runtime resolution short circuits without normalization or writes",
        ),
        (
            "vanilla-runtime-default",
            RUNNER,
            "if (!runtime_hook && !runtime_default && !normalize) return;",
            'if (!runtime_hook && !runtime_default && !normalize) { options.set_option("llamacpp_args", "--draft-max 16"); return; }',
            "HrxGainedNothing:no vanilla DFlash/MTP/parallel runtime defaults or llamacpp_args mutation",
        ),
        (
            "device-callback-overwrites-router",
            RUNNER,
            'if (records.value("device_assignment", "select_backend") == "select_backend" && context.device_selected)',
            "if (context.device_selected)",
            "HrxGainedNothing:preassigned device unchanged",
        ),
        (
            "hrx0-overwritten",
            RUNNER,
            "for (const auto& expression : arg.values) plan.command.args.push_back(scalar(evaluate(expression, frame).value()));",
            'for (const auto& expression : arg.values) plan.command.args.push_back(arg.flag == "--device" ? "ROCm0" : scalar(evaluate(expression, frame).value()));',
            launch,
        ),
        (
            "embedding-context-floor-gained",
            RUNNER,
            "for (const auto& expression : arg.values) plan.command.args.push_back(scalar(evaluate(expression, frame).value()));",
            'for (const auto& expression : arg.values) plan.command.args.push_back(arg.flag == "--ctx-size" && context.model_info.type != ModelType::LLM ? "32768" : scalar(evaluate(expression, frame).value()));',
            "HrxGainedNothing:no embedding context floor, own launch parallel retained",
        ),
        (
            "system-probe-gained",
            RUNNER,
            "auto version = file_version;",
            'auto version = file_version;\n    if (program_.normalized.at("id") == "hrx") (void)sources.version(65536, 5000, [] { return false; });',
            "HrxGainedNothing:system probe count zero",
        ),
        (
            "loading-log-lost",
            RUNNER,
            'LOG(INFO, category.c_str()) << "Loading model: " << context.model_name << std::endl;',
            "(void)category;",
            "HRX exact load logs precede installation",
        ),
        (
            "options-log-lost",
            RUNNER,
            'LOG(DEBUG, category.c_str()) << "Per-model settings: " << context.options.to_log_string() << std::endl;',
            "(void)category;",
            "HRX exact load logs precede installation",
        ),
        (
            "startup-log-gained",
            RUNNER,
            'LOG(DEBUG, category.c_str()) << "Per-model settings: " << context.options.to_log_string() << std::endl;',
            'LOG(DEBUG, category.c_str()) << "Per-model settings: " << context.options.to_log_string() << std::endl;\n        LOG(INFO, category.c_str()) << "Starting llama-server" << std::endl;',
            "HRX exact load logs precede installation",
        ),
        (
            "platform-os-filter-lost",
            RUNNER,
            "if (required.contains(axis.first) && std::find",
            'if (axis.first != "os" && required.contains(axis.first) && std::find',
            "HRX rejects a non-Linux-x86-64 host before logging or installing: windows/x86_64",
        ),
        (
            "platform-arch-filter-lost",
            RUNNER,
            "if (required.contains(axis.first) && std::find",
            'if (axis.first != "machine_arch" && required.contains(axis.first) && std::find',
            "HRX rejects a non-Linux-x86-64 host before logging or installing: linux/aarch64",
        ),
        (
            "unbound-stream-accepted",
            RUNNER,
            'const bool legacy = policy == "llamacpp_legacy";',
            'const bool legacy = policy == "llamacpp_legacy" || policy == "bound_passthrough";',
            "HrxGainedNothing:unbound streaming endpoint rejected",
        ),
        (
            "bound-malformed-body-rejected",
            RUNNER,
            "if (passthrough && parsed.is_discarded())",
            "if (legacy && parsed.is_discarded())",
            'HRX streaming request codec on /v1/chat/completions: {"stream": not json',
        ),
        (
            "bound-sse-overwritten",
            RUNNER,
            "if (passthrough) mapped.sse = sse;",
            "if (legacy) mapped.sse = sse;",
            "HRX bound streaming preserves caller SSE and timeout",
        ),
        (
            "repository-full-params-lost",
            RUNNER,
            "if (context.phase == InstallPhase::Repository && !full) return",
            "if (context.phase == InstallPhase::Repository) return",
            "HRX installer maps the version to the exact release asset",
        ),
        (
            "main-required-bypassed",
            RUNNER,
            "const bool required = evaluate(artifact.required_when, frame).value_or(true).get<bool>();",
            'const bool required = program_.normalized.at("id") != "hrx" && evaluate(artifact.required_when, frame).value_or(true).get<bool>();',
            "HrxGainedNothing:HF options cannot bypass missing-main failure after installation",
        ),
        (
            "optional-model-launch-gained",
            RUNNER,
            'defaults(frame, "launch");',
            'defaults(frame, "launch");\n    if (!context.model_info.resolved_path("mmproj").empty()) plan.command.args.push_back("--mmproj");',
            "HrxGainedNothing:no optional model launch despite populated fields and labels",
        ),
        (
            "parallel-launch-default-lost",
            RUNNER,
            'defaults(frame, "launch");',
            "",
            launch,
        ),
        (
            "explicit-reserved-flags-lost",
            RUNNER,
            'if (records.contains("custom_args")) for (const auto& flag : records["custom_args"]["reserved"]) reserved.insert(flag);',
            "",
            "HRX rejects a managed argument after the managed lookup: -dr replacement",
        ),
        (
            "inherited-reserved-aliases-lost",
            RUNNER,
            "reserved.insert(*arg.flag); reserved.insert(arg.aliases.begin(), arg.aliases.end());",
            "reserved.insert(*arg.flag);",
            "HRX rejects a managed argument after the managed lookup: -c replacement",
        ),
    ]
    metadata = 'if (extension != ".gguf" || !std::filesystem::is_regular_file(file, error)) return;'
    entries.append(
        (
            "metadata-gate-lost",
            RUNNER,
            metadata,
            "if (false) return;",
            "HrxGainedNothing:non-GGUF metadata paths require no artifact read",
        )
    )
    entries.append(
        (
            "parallel-incorrectly-respects-merge-args",
            RUNNER,
            "if (item.stage != stage || (item.respect_merge_args &&",
            'if ((stage == "launch" && !frame.data.options.value("merge_args", true)) || item.stage != stage || (item.respect_merge_args &&',
            "HRX launch parallel remains independent of merge_args",
        )
    )
    for control in ("device_assignment", "load_log_policy", "direct_stream_policy"):
        entries.append(
            (
                "open-enum-" + control,
                "src/cpp/server/backends/configured/backend_definition_v2_contract.cpp",
                "static const auto schema = nlohmann::json::parse(k_backend_definition_v2_schema);",
                'static const auto schema = [] { auto value = nlohmann::json::parse(k_backend_definition_v2_schema); value["$defs"]["Launch"]["properties"]["'
                + control
                + '"]["enum"].push_back("unrecognized"); return value; }();',
                "ClosedRunnerPolicyRejectsUnknownValue:" + control,
                "test_backend_runtime_services",
            )
        )
    entries.append(
        (
            "implicit-common-context-lost",
            RUNNER,
            'if (!options.contains("ctx_size")) options["ctx_size"] = context.options.get_option("ctx_size");',
            "",
            "HRX implicit common context retains its default",
        )
    )
    runner = (REPO / RUNNER).read_text()
    start = runner.index('    if (records.contains("requires"))')
    middle = runner.index('    if (records.value("load_log_policy"', start)
    end = runner.index('    values = {{"options", options}', middle)
    entries.append(
        (
            "logs-before-platform-admission",
            RUNNER,
            runner[start:end],
            runner[middle:end] + runner[start:middle],
            "HRX rejects a non-Linux-x86-64 host before logging or installing: windows/x86_64",
        )
    )
    for entry in entries:
        if args.only is None or entry[0] in args.only:
            probe(args, *entry)
    if args.only is not None:
        assert set(args.only) == {
            row["rule"] for row in args.results
        }, "EveryRequestedMutationExecuted"
    print(len(args.results), "independent compiled regressions caught")


if __name__ == "__main__":
    main()
