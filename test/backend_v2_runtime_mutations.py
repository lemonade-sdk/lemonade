import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

RUNNER = "src/cpp/server/backends/configured/backend_definition_runner.cpp"
SERVICES = "src/cpp/server/backends/configured/backend_runtime_services.cpp"


def mutations():
    return [
        (
            "chain-order",
            RUNNER,
            "program_.hooks[a].order < program_.hooks[b].order",
            "program_.hooks[a].order > program_.hooks[b].order",
            "definition_runner",
            "RequestChainsFollowSealedOrder",
        ),
        (
            "effect",
            SERVICES,
            "if (std::find(effects_.begin(), effects_.end(), effect) == effects_.end())",
            "if (false)",
            "runtime_services",
            "ServiceCannotEscapeEffectSet",
        ),
        (
            "active-slot",
            SERVICES,
            " || frame_.active_slot != slot_",
            "",
            "runtime_services",
            "ServiceOutsideBoundSlotRejects",
        ),
        (
            "thread",
            SERVICES,
            "thread_ != std::this_thread::get_id() || ",
            "",
            "runtime_services",
            "ServiceOnForeignThreadRejects",
        ),
        (
            "executable",
            SERVICES,
            "frame_.executable != executable_ || frame_.nonce != nonce_ ||",
            "frame_.nonce != nonce_ ||",
            "runtime_services",
            "ServiceOnForeignExecutableRejects",
        ),
        (
            "owner",
            SERVICES,
            "if (owner_.owner_kind != kind || owner_.owner_id != owner || owner_.stage != stage)",
            "if (false)",
            "runtime_services",
            "WrongOwnerRejects",
        ),
        (
            "nested",
            SERVICES,
            "if (frame_.active_slot) throw",
            "if (false) throw",
            "runtime_services",
            "NestedSlotRejects",
        ),
        (
            "allowance",
            SERVICES,
            "if (allowance > remaining_)",
            "if (false)",
            "runtime_services",
            "ServiceAllowanceIsBounded",
        ),
        (
            "facts",
            SERVICES,
            "if (input.producer ? !frame_.produced.count(*input.producer) : !frame_.facts.count(input.fact))",
            "if (false)",
            "runtime_services",
            "MissingProducerIsNotLazilyInvoked",
        ),
        (
            "produced-facts",
            SERVICES,
            "input.producer ? !frame_.produced.count(*input.producer)",
            "input.producer ? false",
            "runtime_services",
            "MissingProducedFactDoesNotInvokeBackendLazily",
        ),
        (
            "cancellation",
            SERVICES,
            "    check_cancelled(cancel_);",
            "",
            "runtime_services",
            "EveryEffectChecksCancellation",
        ),
        (
            "nonce",
            SERVICES,
            "nonce(invocation_nonce())",
            "nonce(1)",
            "runtime_services",
            "InvocationNoncesAreUnique",
        ),
        (
            "resource",
            SERVICES,
            "if (std::find(supported_.begin(), supported_.end(), backend) == supported_.end())",
            "if (false)",
            "runtime_services",
            "SelectionCannotEscapeBackendScope",
        ),
        (
            "runtime-hook-id-redirect",
            RUNNER,
            "hook.slot.owner_kind == kind && hook.slot.owner_id == owner && hook.slot.stage == stage",
            "hook.slot.stage == stage && hook.slot.hook_id == program_.hooks.front().slot.hook_id",
            "definition_runner",
            "RunnerEndToEndDispatchAndFrame",
        ),
        (
            "response-cell",
            RUNNER,
            'slots(SlotOwnerKind::Operation, op.id, "response")',
            'slots(SlotOwnerKind::Operation, program_.operations.front().id, "response")',
            "definition_runner",
            "ResponseChainStaysOnSelectedCell",
        ),
        (
            "request-frame",
            RUNNER,
            'frame.data.values["request"] = request;',
            'frame.data.values["request"] = json::object();',
            "definition_runner",
            "RunnerEndToEndDispatchAndFrame",
        ),
        (
            "raw-stream",
            RUNNER,
            "if (original_body && op.identity && op.remove.empty() && op.set.empty() && chain.empty())",
            "if (false)",
            "definition_runner",
            "EmptyStreamingChainRemainsEmptyAndRaw",
        ),
        (
            "stream-chain",
            RUNNER,
            'const auto chain = slots(SlotOwnerKind::Operation, op.id, "request");',
            'const auto chain = delivery == OperationDelivery::Streaming ? std::vector<std::size_t>{} : slots(SlotOwnerKind::Operation, op.id, "request");',
            "definition_runner",
            "StreamingRequestChainUsesBoundCell",
        ),
        (
            "explicit-empty",
            RUNNER,
            'if (existing != frame.data.options.end() && *existing == "" && frame.explicit_options.count(name)) return;',
            "",
            "runtime_services",
            "RuntimeDefaultsPreserveExplicitEmptyAndNull",
        ),
        (
            "explicit-null",
            RUNNER,
            "if (existing != frame.data.options.end() && existing->is_null()) return;",
            'if (existing != frame.data.options.end() && existing->is_null()) frame.data.options[name] = "";',
            "runtime_services",
            "RuntimeDefaultsPreserveExplicitEmptyAndNull",
        ),
        (
            "merge-args",
            RUNNER,
            '(item.respect_merge_args && !frame.data.options.value("merge_args", true))',
            "false",
            "runtime_services",
            "RuntimeDefaultsRespectMergeArgs",
        ),
        (
            "default-stage",
            RUNNER,
            "item.stage != stage || ",
            "",
            "runtime_services",
            "DefaultsApplyAtDeclaredStage",
        ),
        (
            "context-floor",
            RUNNER,
            "if (floor != launch.context_floor.end())",
            "if (false)",
            "definition_runner",
            "ContextFloorDoesNotMutateCaller",
        ),
        (
            "spec-ownership",
            RUNNER,
            'if (spec.executable_definition.get() != executable_.get() || spec.definition ||\n        spec.public_recipe != declaration()["recipe"] || spec.pin_recipe != spec.public_recipe)',
            "if (false)",
            "definition_runner",
            "LeafSpecOwnershipBeforeEffects",
        ),
        (
            "adapter-kind",
            RUNNER,
            "if (!installed ||",
            "if (false ||",
            "runtime_boundary",
            "UnsupportedAdapterHasNoEntry",
        ),
        (
            "adapter-mask",
            RUNNER,
            "!description.model_types || (description.model_types & ~k_all_operation_model_types) ||",
            "false ||",
            "runtime_boundary",
            "UnsupportedAdapterHasNoEntry",
        ),
        (
            "adapter-method",
            RUNNER,
            'op.method != (operation == BackendOperation::Slots ? "GET" : "POST") ||',
            "false ||",
            "runtime_boundary",
            "UnsupportedAdapterHasNoEntry",
        ),
        (
            "adapter-port",
            RUNNER,
            '!port || op.port != readiness["port"] ||',
            "false ||",
            "runtime_boundary",
            "UnsupportedAdapterHasNoEntry",
        ),
        (
            "adapter-endpoint",
            RUNNER,
            "!endpoint(op.path) ||",
            "false ||",
            "runtime_boundary",
            "UnsupportedAdapterHasNoEntry",
        ),
        (
            "adapter-timeout",
            RUNNER,
            "op.timeout_seconds > static_cast<std::uint64_t>(std::numeric_limits<long>::max())",
            "false",
            "runtime_boundary",
            "UnsupportedAdapterHasNoEntry",
        ),
    ]


def run(command, cwd, log, env, timeout=120):
    with log.open("w") as output:
        try:
            return subprocess.run(
                command,
                cwd=cwd,
                env=env,
                stdout=output,
                stderr=subprocess.STDOUT,
                timeout=timeout,
                check=False,
            ).returncode
        except subprocess.TimeoutExpired:
            return 124


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--unit", choices=["unit1"], required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build, scratch, out = (
        p.resolve() for p in (args.build_dir, args.scratch, args.out)
    )
    for path in (scratch, out):
        if path == Path("/tmp") or Path("/tmp") in path.parents:
            raise ValueError("Runtime evidence must use persistent project storage")
        path.mkdir(parents=True, exist_ok=True)
    temp = scratch / "temp"
    temp.mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp))
    commands = json.loads((build / "compile_commands.json").read_text())
    results = []
    for mutation in mutations():
        name, relative, before, after, suite, assertion, *extra = mutation
        directory = scratch / name
        directory.mkdir(exist_ok=True)
        source = root / relative
        text = source.read_text()
        if before not in text:
            raise ValueError(f"Missing mutation anchor: {name}")
        text = text.replace(before, after)
        if extra:
            text = text.replace(extra[0], extra[1])
        copied = directory / source.name
        copied.write_text(text)
        record = {
            "rule": name,
            "assertion": assertion,
            "source_sha256": hashlib.sha256(copied.read_bytes()).hexdigest(),
        }
        entry = next(x for x in commands if x["file"] == str(source))
        command = shlex.split(entry["command"])
        original_object = command[command.index("-o") + 1]
        command[command.index("-o") + 1] = str(directory / "mutant.o")
        command[command.index("-c") + 1] = str(copied)
        for flag in ("-MT", "-MF"):
            if flag in command:
                index = command.index(flag)
                del command[index : index + 2]
        command = [x for x in command if x != "-MD"]
        command += ["-I", str(source.parent), "-O0"]
        record["compile"] = run(command, build, directory / "compile.log", env)
        if record["compile"] == 0:
            target = "test_backend_" + suite
            lines = subprocess.check_output(
                ["ninja", "-C", str(build), "-t", "commands", target], text=True
            ).splitlines()
            link = shlex.split(lines[-1])
            if link[:2] == [":", "&&"]:
                link = link[2:-2]
            link = [
                str(directory / "mutant.o") if x == original_object else x
                for x in link
                if not x.startswith("-Wl,--dependency-file=")
            ]
            link[link.index("-o") + 1] = str(directory / target)
            record["link"] = run(link, build, directory / "link.log", env)
            if record["link"] == 0:
                record["run"] = run(
                    [str(directory / target)], root, directory / "run.log", env, 60
                )
                record["caught"] = (
                    record["run"] not in (0, 124)
                    and "FAIL " + assertion in (directory / "run.log").read_text()
                )
        results.append(record)
        (out / "mutations.json").write_text(
            json.dumps(
                {"coverage": "partial-4d1-runner", "mutations": results}, indent=2
            )
            + "\n"
        )
        print(
            f"{name}: {'caught' if record.get('caught') else 'NOT CAUGHT'}", flush=True
        )
    if not all(x.get("caught") for x in results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
