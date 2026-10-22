"""Compare baseline code generation and mutation-check frozen wrapper fixtures."""

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

REPO = Path(__file__).resolve().parents[1]
BASELINE = "cc73edbeb800589a62478b963e8a0cf2d920357d"
SERVER = "src/cpp/server/backends/llamacpp/llamacpp_server.cpp"
SOURCES = [
    SERVER,
    "src/cpp/server/backends/llamacpp/llamacpp_request.cpp",
    "src/cpp/server/backends/backend_utils.cpp",
    "src/cpp/server/runtime_config.cpp",
]


def run(command, cwd, log):
    process = subprocess.run(
        command, cwd=cwd, capture_output=True, text=True, check=False
    )
    log.write_text(process.stdout + process.stderr)
    return process


def link_command(build, target):
    lines = subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "commands", target], text=True
    ).splitlines()
    command = shlex.split(lines[-1])
    if command[:2] == [":", "&&"]:
        command = command[2:]
    if "&&" in command:
        command = command[: command.index("&&")]
    return [arg for arg in command if not arg.startswith("-Wl,--dependency-file=")]


def compile_source(entry, source, output, root):
    command = shlex.split(entry["command"])
    command[command.index("-o") + 1] = str(output)
    command[command.index("-c") + 1] = str(source)
    command.extend(["-iquote", str(Path(entry["file"]).parent)])
    command.append(f"-fmacro-prefix-map={root}={REPO}")
    return command


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def equality(args, database):
    overlay = args.out / "baseline"
    paths = SOURCES + ["src/cpp/server/backends/llamacpp/llamacpp_system_utils.h"]
    for path in paths:
        destination = overlay / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(
            subprocess.check_output(["git", "show", f"{BASELINE}:{path}"], cwd=REPO)
        )
    replacements = {}
    result = {"baseline": BASELINE, "objects": {}, "binaries": {}}
    for path in SOURCES:
        entry = next(
            row
            for row in database
            if row["file"] == str(REPO / path)
            and "lemonade-server-core.dir" in row["command"]
        )
        output = overlay / (Path(path).stem + ".o")
        process = run(
            compile_source(entry, overlay / path, output, overlay),
            args.build,
            args.out / (Path(path).stem + "-baseline.log"),
        )
        if process.returncode:
            raise RuntimeError(f"Baseline compilation failed: {path}")
        original = shlex.split(entry["command"])
        object_path = original[original.index("-o") + 1]
        replacements[object_path] = str(output)
        actual = args.build / object_path
        result["objects"][path] = {
            "baseline_sha256": digest(output),
            "actual_sha256": digest(actual),
            "identical": output.read_bytes() == actual.read_bytes(),
        }
        if not result["objects"][path]["identical"]:
            before = subprocess.check_output(["nm", "-S", "-C", str(output)], text=True)
            after = subprocess.check_output(["nm", "-S", "-C", str(actual)], text=True)
            (args.out / (Path(path).stem + "-symbols-before.txt")).write_text(before)
            (args.out / (Path(path).stem + "-symbols-after.txt")).write_text(after)
    for target in ["lemond", "lemonade"]:
        command = link_command(args.build, target)
        command = [replacements.get(arg, arg) for arg in command]
        binary = args.out / (target + "-baseline")
        command[command.index("-o") + 1] = str(binary)
        process = run(command, args.build, args.out / (target + "-baseline-link.log"))
        if process.returncode:
            raise RuntimeError(f"Baseline link failed: {target}")
        actual = args.build / target
        result["binaries"][target] = {
            "baseline_sha256": digest(binary),
            "actual_sha256": digest(actual),
            "identical": binary.read_bytes() == actual.read_bytes(),
        }
    (args.out / "equality.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)
    if not all(
        item["identical"]
        for group in ["objects", "binaries"]
        for item in result[group].values()
    ):
        raise RuntimeError(
            "Production code generation differs from the frozen baseline"
        )


def mutations(args, database):
    target = "test_llamacpp_oracle_linux_x64"
    entry = next(
        row
        for row in database
        if row["file"] == str(REPO / SERVER) and target + ".dir" in row["command"]
    )
    source = (REPO / SERVER).read_text()
    install = next(
        line
        for line in source.splitlines(True)
        if "LLAMA_EFFECT(backend_manager_->install_backend" in line
    )
    cases = {
        "flag-spelling": source.replace('"--metrics"', '"--metricz"', 1),
        "parallel-default": source.replace('"--parallel 1"', '"--parallel 2"', 1),
        "hook-order": source.replace(
            "llamacpp::sanitize_tool_schema_limits(\n                            llamacpp::with_max_tokens_alias(request))",
            "llamacpp::with_max_tokens_alias(\n                            llamacpp::sanitize_tool_schema_limits(request))",
            1,
        ),
        "error-message": source.replace(
            "GGUF file not found for checkpoint:", "GGUF missing for checkpoint:", 1
        ),
        "reserved-alias": source.replace(
            'std::vector<std::string>{"-dev"}', 'std::vector<std::string>{"-device"}', 1
        ),
        "context-floor": source.replace(
            "ctx_size = EMBEDDING_CTX_SIZE;", "ctx_size = EMBEDDING_CTX_SIZE + 1;", 1
        ),
        "readiness-endpoint": source.replace(
            "command.port = port_;",
            'command.port = port_;\n    command.ready_endpoint = "/ready";',
            1,
        ),
        "failure-order": source.replace(install, "", 1).replace(
            "    if (!gguf_path.empty()) {",
            install + "\n    if (!gguf_path.empty()) {",
            1,
        ),
    }
    results = {}
    original = shlex.split(entry["command"])
    object_path = original[original.index("-o") + 1]
    for name, mutated in cases.items():
        if mutated == source:
            raise RuntimeError(f"Mutation no longer applies: {name}")
        directory = args.out / name
        directory.mkdir(exist_ok=True)
        path = directory / "llamacpp_server.cpp"
        path.write_text(mutated)
        obj = directory / "mutant.o"
        process = run(
            compile_source(entry, path, obj, directory),
            args.build,
            directory / "compile.log",
        )
        if process.returncode:
            raise RuntimeError(f"Mutation failed to compile: {name}")
        command = link_command(args.build, target)
        command = [str(obj) if arg == object_path else arg for arg in command]
        binary = directory / "mutant"
        command[command.index("-o") + 1] = str(binary)
        process = run(command, args.build, directory / "link.log")
        if process.returncode:
            raise RuntimeError(f"Mutation failed to link: {name}")
        process = run([str(binary)], args.build, directory / "test.log")
        results[name] = {
            "returncode": process.returncode,
            "caught": process.returncode == 1,
            "suite": target,
        }
        (args.out / "mutations.json").write_text(json.dumps(results, indent=2) + "\n")
        print(name, results[name], flush=True)
        if process.returncode != 1:
            raise RuntimeError(f"Mutation was not caught by a fixture mismatch: {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["equality", "mutations"])
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    database = json.loads((args.build / "compile_commands.json").read_text())
    if args.mode == "equality":
        equality(args, database)
    else:
        mutations(args, database)


if __name__ == "__main__":
    main()
