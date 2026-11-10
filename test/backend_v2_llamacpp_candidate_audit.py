"""Mutation-check the partial llama.cpp candidate's declaration assertions."""

import argparse
import copy
import json
import os
from pathlib import Path
import subprocess

REPO = Path(__file__).resolve().parents[1]
FIXTURE = REPO / "test/fixtures/backend-v2/llamacpp.candidate.json"


def replace(document, pointer, value):
    parts = pointer.strip("/").split("/")
    target = document
    for part in parts[:-1]:
        target = target[int(part)] if isinstance(target, list) else target[part]
    key = int(parts[-1]) if isinstance(target, list) else parts[-1]
    target[key] = value


def witnesses(source):
    root = "/merge"
    operation = root + "/operations"
    launch = root + "/launch"
    alias = {"id": "llamacpp.max_tokens_alias", "params": {}}
    sanitizer = {"id": "llamacpp.tool_schema_limits", "params": {}}
    normalization = {"id": "llamacpp.client_model", "params": {}}
    yield "hook-order", operation + "/0/request/hooks", [
        sanitizer,
        alias,
    ], "CandidateRequestChainExact chat.buffered"
    yield "streaming-completion-chain", operation + "/4/request/hooks", [
        alias
    ], "CandidateRequestChainExact completion.streaming"
    yield "normalization-placement", operation + "/0/response/hooks", [], "CandidateResponseNormalizationStageExact chat.buffered"
    yield "context-floor", launch + "/context_floor/embedding", 4096, "CandidateEmbeddingContextFloorExact"
    yield "reserved-alias", launch + "/argv/8/aliases", [
        "-mm"
    ], "CandidateReservedAliasesExact 8"
    yield "default-stage", launch + "/custom_args/defaults/2/stage", "launch", "CandidateRuntimeDefaultOrderConditionsAndStagesExact"
    yield "descriptor-field", root + "/metadata/arg_variants", [
        "rocm",
        "vulkan",
        "cuda",
        "cpu",
    ], "CandidateAllDescriptorFieldsAndTypesExact"
    yield "descriptor-json-type", root + "/metadata/options/0/default_value", 0, "CandidateAllDescriptorFieldsAndTypesExact"
    yield "support-row-order", root + "/metadata/support", list(
        reversed(source["merge"]["metadata"]["support"])
    ), "CandidateAllDescriptorFieldsAndTypesExact"
    yield "rocm-gate", root + "/metadata/support/4/arch_gates/gfx950/channels", [
        "nightly"
    ], "CandidateAllDescriptorFieldsAndTypesExact"
    yield "server-log-policy", root + "/metadata/log_level_policy", "debug_flag", "CandidateServerMetadataExact"
    yield "stable-option-id", root + "/metadata/options/0/id", "changed", "CandidateStableOptionIdsExact"
    yield "normalization-on-stream", operation + "/4/response", {
        "hooks": [normalization]
    }, "CandidateResponseNormalizationStageExact completion.streaming"
    yield "missing-cell", operation, source["merge"]["operations"][
        :-1
    ], "CandidateAll11CellsExact"
    yield "prepare-order", launch + "/prepare_order", [
        "backend",
        "hook",
        "install",
        "artifacts",
        "ports",
        "program",
    ], "CandidatePreparationOrderExact"
    yield "inactive-reservation", launch + "/argv/3/reserve", "when_active", "CandidateInactiveAliasesRemainReserved"
    yield "argv-order", launch + "/argv", list(
        reversed(source["merge"]["launch"]["argv"])
    ), "CandidateArgvOrderExact"
    yield "debug-reserves-lv", launch + "/argv/7/flag", "-lv", "CandidateResolvesThroughV2Schema"
    yield "custom-error-prefix", launch + "/custom_args/error_prefix", "Invalid arguments", "CandidateCustomArgumentPolicyExact"
    yield "main-load-strategy", root + "/artifacts/main/strategy", "hook", "CandidateLoadUsesResolvedMainAndTypedManagementContract"
    yield "wrong-artifact-contract", root + "/artifacts/main/hook/id", "llamacpp.gguf", "CandidateLoadUsesResolvedMainAndTypedManagementContract"
    yield "missing-release", root + "/install/cases", source["merge"]["install"][
        "cases"
    ][:-1], "CandidateAll14ReleaseRowsPresent"
    yield "windows-machine-filter", root + "/install/cases/4/machine_arch", [
        "x86_64"
    ], "CandidateWindowsAssetsDoNotRestrictMachineArchitecture"
    for index, cell in enumerate(source["merge"]["operations"]):
        yield "cell-path-" + cell[
            "id"
        ], operation + f"/{index}/transport/path", "/changed", "CandidateCellSignatureExact " + cell[
            "id"
        ]
    for index, arg in enumerate(source["merge"]["launch"]["argv"]):
        field, value = (
            ("args", ["-lv", "3"]) if arg["id"] == "debug" else ("flag", "--changed")
        )
        yield "argv-value-" + arg[
            "id"
        ], launch + f"/argv/{index}/{field}", value, "CandidateAllArgvRecordsExact"
    for index, row in enumerate(source["merge"]["install"]["cases"]):
        pointer = root + f"/install/cases/{index}"
        yield "release-filename-" + row[
            "id"
        ], pointer + "/filename", "wrong", "CandidateReleaseSelectorsAndFilenameExact " + row[
            "id"
        ]
        yield "release-repo-" + row[
            "id"
        ], pointer + "/repo", "wrong/repo", "CandidateReleaseRepositoryExact " + row[
            "id"
        ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument(
        "--scratch",
        type=Path,
        default=REPO / ".synagent/scratch/backend-v2-4e2/mutations",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=REPO / ".synagent/artifacts/backend-v2-4e2/mutations",
    )
    args = parser.parse_args()
    args.scratch = args.scratch.resolve()
    args.out = args.out.resolve()
    for directory in (args.scratch, args.out):
        directory.relative_to(REPO)
        directory.mkdir(parents=True, exist_ok=True)
    temporary = args.scratch / "tmp"
    temporary.mkdir(exist_ok=True)
    environment = dict(
        os.environ, TMPDIR=str(temporary), TMP=str(temporary), TEMP=str(temporary)
    )
    executable = args.build.resolve() / "test_backend_definition_descriptor"
    control = subprocess.run(
        ["timeout", "120s", str(executable)],
        capture_output=True,
        text=True,
        env=environment,
        check=True,
    )
    (args.out / "control.log").write_text(control.stdout + control.stderr)
    source = json.loads(FIXTURE.read_text())
    results = []
    for name, pointer, value, assertion in witnesses(source):
        document = copy.deepcopy(source)
        replace(document, pointer, value)
        mutant = args.scratch / (name + ".json")
        mutant.write_text(json.dumps(document, indent=2) + "\n")
        result = subprocess.run(
            ["timeout", "120s", str(executable), "--candidate", str(mutant)],
            capture_output=True,
            text=True,
            env=environment,
        )
        output = result.stdout + result.stderr
        (args.out / (name + ".log")).write_text(output)
        caught = result.returncode == 1 and "FAIL " + assertion in output
        results.append(
            {
                "rule": name,
                "pointer": pointer,
                "breakage": value,
                "assertion": assertion,
                "returncode": result.returncode,
                "caught": caught,
            }
        )
        (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        assert caught, (name, result.returncode, output)
    print(
        f"{len(results)}/{len(results)} declaration mutants caught by named assertions; control passed"
    )
    print(
        "This declaration audit does not cover runtime behavior; use backend_v2_llamacpp_runtime_mutations.py for the implemented runtime rules. Use backend_v2_llamacpp_diff_mutations.py for the deliberate-diff gate."
    )


if __name__ == "__main__":
    main()
