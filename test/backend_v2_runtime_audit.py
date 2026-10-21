import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    scratch = args.scratch.resolve()
    if scratch == Path("/tmp") or Path("/tmp") in scratch.parents:
        raise ValueError("Runtime evidence must use persistent project storage")
    scratch.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(scratch), TMP=str(scratch), TEMP=str(scratch))
    private = root / "src/cpp/server/backends/configured"
    allowed = {
        "backend_definition_binding.cpp",
        "backend_definition_runner_internal.h",
    }
    for path in private.rglob("*"):
        if path.suffix in {".cpp", ".h"}:
            text = path.read_text()
            if "binding::Inspection::hooks(" in text and path.name not in allowed:
                raise AssertionError(
                    f"Bound target inspection outside runner/binder: {path}"
                )
    commands = json.loads((build / "compile_commands.json").read_text())
    entry = next(
        x
        for x in commands
        if x["file"] == str(private / "backend_definition_runner.cpp")
    )
    base = shlex.split(entry["command"])
    for flag in ("-MT", "-MF", "-o"):
        if flag in base:
            index = base.index(flag)
            del base[index : index + 2]
    base = [x for x in base if x != "-MD"]
    for name, statement in {
        "target-inspection": "auto callable = &lemon::backends::binding::Inspection::hooks;",
        "permit-construction": 'using namespace lemon::backends; void escape(runtime::Frame& frame, const ExecutableDefinition& executable, const execution::SymbolicHook& hook) { runtime::InvocationGuard permit(frame, executable, 0, hook, SlotOwnerKind::Root, "backend", "backend", {}); }',
        "entry-construction": "using namespace lemon::backends; void escape(std::shared_ptr<const ExecutableDefinition> executable) { ConfiguredBackendEntry entry(executable); }",
    }.items():
        source = scratch / (name + ".cpp")
        source.write_text(
            '#include "backend_definition_runner_internal.h"\n#include "lemon/backends/configured/configured_backend_entry.h"\n'
            + statement
            + "\n"
        )
        command = base.copy()
        command[command.index("-c") + 1] = str(source)
        command += ["-I", str(private), "-fsyntax-only"]
        result = subprocess.run(
            command,
            cwd=build,
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )
        (scratch / (name + ".log")).write_text(result.stdout + result.stderr)
        assert result.returncode != 0 and "private" in result.stderr, name
    for name in (
        "backend_runtime_services.cpp",
        "backend_definition_runner.cpp",
        "configured_server.cpp",
    ):
        obj = (
            build
            / "CMakeFiles/lemonade-server-core.dir/src/cpp/server/backends/configured"
            / (name + ".o")
        )
        symbols = subprocess.check_output(["nm", "-C", str(obj)], text=True)
        assert (
            "TestAccess" not in symbols
            and "test_targets" not in symbols
            and "observe_entry" not in symbols
        ), name
    print(
        "Production runtime has no test injection; bound target inspection and permit/entry construction reject outside private consumers"
    )


if __name__ == "__main__":
    main()
