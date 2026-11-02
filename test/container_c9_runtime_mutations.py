"""Compile C9 runtime, selector and generated-capability mutations against each production leaf."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

from container_c8a_mutations import checkpoint_rules, rules as refusal_rules

REPO = Path(__file__).resolve().parents[1]
CONFIGURED = REPO / "src/cpp/server/backends/configured"
SUITE = "test_container_production"


def rules(engine):
    result = []
    if engine != "ds4_rocm":
        name, filename, old, new, _ = refusal_rules()[0]
        result.append(
            (
                name,
                CONFIGURED / filename,
                old,
                new,
                "C9RefusalBeforeAnyInstall:" + engine,
                [engine],
                None,
            )
        )
        registry = REPO / "src/cpp/server/backends/backend_registry.cpp"
        result.extend(
            [
                (
                    "selector-match",
                    registry,
                    "registration.backend_selector == backend",
                    "registration.backend_selector.empty()",
                    "C9ExactSelectorIdentity:" + engine,
                    ["--selector", engine],
                    None,
                ),
                (
                    "inactive-selector-fallback",
                    registry,
                    'throw std::runtime_error("Builtin backend selector is inactive: " + recipe + ":" + backend);',
                    "(void)backend;",
                    "C9InactiveSelectorCannotFallBack:" + engine,
                    ["--selector", engine],
                    None,
                ),
                (
                    "router-selector-propagation",
                    REPO / "src/cpp/server/router.cpp",
                    "ctx.backend_selector = backend.get<std::string>();",
                    'ctx.backend_selector = "";',
                    "C9RouterDispatchesSharedSelector:" + engine,
                    ["--selector", engine],
                    None,
                ),
            ]
        )
    else:
        names = {
            "source-checkpoint-error": "C9CheckpointStillFailsWithoutExistingAbsolute",
            "available-resolved-fallback": "C9AvailableResolvedAlwaysWins",
            "relative-checkpoint": "C9CheckpointStillFailsWithoutExistingAbsolute",
            "fallback-inventory-bypass": "C9FallbackCannotBypassArtifactInventory",
        }
        for name, filename, old, new, _ in checkpoint_rules():
            if name in names:
                result.append(
                    (
                        name,
                        CONFIGURED / filename,
                        old,
                        new,
                        names[name] + ":" + engine,
                        [engine],
                        None,
                    )
                )
    result.append(
        (
            "generated-capability-mask",
            REPO / "test/cpp/test_container_production.cpp",
            None,
            None,
            "C8GeneratedCapabilityMask:" + engine,
            [engine],
            "container_production_capabilities.h",
        )
    )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("build", "out", "scratch"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument(
        "--engine", choices=("halobox", "nathanw", "ds4_rocm"), required=True
    )
    args = parser.parse_args()
    for name in ("build", "out", "scratch"):
        setattr(args, name, getattr(args, name).resolve())
    for path in (args.out, args.scratch, args.scratch / "temporary"):
        path.mkdir(parents=True, exist_ok=True)
    env = dict(
        os.environ,
        TMPDIR=str(args.scratch / "temporary"),
        TMP=str(args.scratch / "temporary"),
        TEMP=str(args.scratch / "temporary"),
    )
    env.pop("LEMON_CONTAINER_PRODUCTION_CANDIDATE", None)

    def run(name, command, cwd=REPO):
        with (args.out / (name + ".log")).open("w") as log:
            return subprocess.run(
                ["timeout", "180", *map(str, command)],
                cwd=cwd,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=185,
            )

    invocation = [args.build / SUITE, args.engine]
    assert run("control-before", invocation).returncode == 0, "C9RuntimeControlMustPass"
    database = json.loads((args.build / "compile_commands.json").read_bytes())
    commands = subprocess.check_output(
        ["ninja", "-t", "commands", SUITE], cwd=args.build, text=True, timeout=30
    ).splitlines()
    link = [
        x
        for x in shlex.split(commands[-1].removeprefix(": && ").removesuffix(" && :"))
        if not x.startswith("-Wl,--dependency-file=")
    ]
    results = []
    for name, source, old, new, assertion, arguments, header in rules(args.engine):
        work = args.scratch / name
        work.mkdir(exist_ok=True)
        (
            (work / "resources").symlink_to(
                args.build / "resources", target_is_directory=True
            )
            if not (work / "resources").exists()
            else None
        )
        original = source.read_bytes()
        mutant = work / source.name
        if header:
            text = (args.build / "include" / header).read_text()
            before, after = text.split('if (id == "' + args.engine + '")', 1)
            line, rest = after.split("\n", 1)
            start = line.index("return ")
            end = line.index(";", start)
            line = (
                line[:start]
                + (
                    "return lemon::CAP_EMBEDDINGS"
                    if args.engine == "ds4_rocm"
                    else "return 0"
                )
                + line[end:]
            )
            (work / header).write_text(
                before + 'if (id == "' + args.engine + '")' + line + "\n" + rest
            )
            mutant.write_bytes(original)
        else:
            text = original.decode()
            assert text.count(old) == 1, (name, text.count(old))
            mutant.write_text(text.replace(old, new))
        target = (
            "test_container_production.dir"
            if source.name == "test_container_production.cpp"
            else "lemonade-server-core.dir"
        )
        row = next(
            r for r in database if Path(r["file"]) == source and target in r["command"]
        )
        compile_command = shlex.split(row["command"])
        old_object = compile_command[compile_command.index("-o") + 1]
        obj, binary = work / "mutant.o", work / SUITE
        compile_command = [
            str(mutant) if x == str(source) else str(obj) if x == old_object else x
            for x in compile_command
        ]
        compile_command[1:1] = [
            "-I" + str(work),
            "-I" + str(source.parent),
            "-I" + str(CONFIGURED),
            "-I" + str(REPO / "test/cpp"),
        ]
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
        result = run(name + ".run", [binary, *arguments])
        assert (
            result.returncode == 1
            and "FAIL " + assertion in (args.out / (name + ".run.log")).read_text()
        ), (name, result.returncode, assertion)
        assert (
            hashlib.sha256(source.read_bytes()).digest()
            == hashlib.sha256(original).digest()
        ), "C9ProductionSourceUntouched"
        results.append(
            dict(
                engine=args.engine, rule=name, assertion=assertion, exit=1, caught=True
            )
        )
        (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(name, "caught by", assertion, flush=True)
    assert (
        run("control-after", invocation).returncode == 0
    ), "C9RuntimeControlRemainsGreen"


if __name__ == "__main__":
    main()
