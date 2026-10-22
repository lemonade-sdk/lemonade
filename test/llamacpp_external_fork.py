"""CPU startup-to-subprocess proof with an uninjected production control."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

REPO = Path(__file__).resolve().parents[1]


def run(command, env, log, timeout=60):
    with log.open("w") as output:
        process = subprocess.Popen(
            command,
            stdout=output,
            stderr=subprocess.STDOUT,
            env=env,
            start_new_session=True,
        )
        try:
            result = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise
    return result, log.read_text()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument(
        "--out", type=Path, default=REPO / ".synagent/artifacts/backend-v2-4g"
    )
    parser.add_argument(
        "--scratch", type=Path, default=REPO / ".synagent/scratch/backend-v2-4g"
    )
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    scratch = args.scratch / f"product-{time.time_ns()}"
    scratch.mkdir(parents=True)
    env = dict(
        os.environ,
        TMPDIR=str(scratch),
        TMP=str(scratch),
        TEMP=str(scratch),
        GGML_DISABLE_VULKAN="parent-sentinel",
    )
    result, output = run(
        [str(args.build / "test_llamacpp_external_fork")], env, args.out / "product.log"
    )
    assert result == 0, (
        "ExternalForkDeclarationToProcessExact",
        result,
        output[-5000:],
    )
    process = json.loads(
        (scratch / "external-fork/successful-process.json").read_bytes()
    )
    expected_hash = hashlib.sha256(
        (args.build / "test_external_fake_engine").read_bytes()
    ).hexdigest()
    assert (
        process["process"]["sha256"] == expected_hash
    ), "ExternalForkExecutableHashExact"
    assert process["process"]["path"] == str(
        scratch / "external-fork/research-llama-server"
    ), "ExternalForkNeverUsesBuiltinExecutable"
    directory = scratch / "external-fork"
    with socket.socket() as bound:
        bound.bind(("127.0.0.1", 0))
        port = bound.getsockname()[1]
    result, output = run(
        [
            str(args.build / "lemond"),
            str(scratch / "cache"),
            str(directory),
            "--host",
            "127.0.0.1",
            "--port",
            str(port),
            "--no-broadcast",
            "--log-file",
            "disabled",
        ],
        env,
        args.out / "uninjected-production.log",
        timeout=15,
    )
    assert result != 0 and "unexported_builtin" in output, (
        "ProductionIdenticalForkIsUnexported",
        result,
        output[-3000:],
    )
    database = json.loads((args.build / "compile_commands.json").read_bytes())
    injected = [
        row for row in database if "LEMON_BACKEND_FORK_TESTING" in row["command"]
    ]
    assert len(injected) == 3 and all(
        "test_llamacpp_external_fork.dir" in row["command"] for row in injected
    ), "ProductionCompileDefinitionsExcludeInjection"
    symbols = subprocess.check_output(
        [
            "nm",
            "-C",
            str(args.build / "lemond"),
            str(args.build / "liblemonade-backend-definition-v2.a"),
        ],
        text=True,
        timeout=30,
    )
    assert (
        "test_selected_builtin_exports" not in symbols
        and "external_fixture::" not in symbols
    ), "ProductionHasNoExportInjectionSymbols"
    assert (
        b"external-fork-cpu-4g-73f6" not in (args.build / "lemond").read_bytes()
    ), "ProductionContainsNoTestContractOrEngine"
    record = dict(
        cpu_only=True,
        hardware_queue_acquired=False,
        process=process,
        uninjected_result=result,
        identical_fork_sha256=hashlib.sha256(
            (directory / "backends.d/research-llama.json").read_bytes()
        ).hexdigest(),
        production_injection_symbols=False,
        injection_definitions=len(injected),
        scratch=str(scratch),
    )
    (args.out / "product.json").write_text(json.dumps(record, indent=2) + "\n")
    print(
        "LlamaCppExternalForkTest: real process, exact transcript, negative controls and production isolation passed"
    )


if __name__ == "__main__":
    main()
