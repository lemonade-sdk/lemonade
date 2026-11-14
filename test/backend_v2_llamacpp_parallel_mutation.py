"""Compile a shared-directory mutant and catch cross-process fixture corruption."""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

import backend_v2_llamacpp_runtime_mutations as driver


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=driver.ROOT / "build")
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve()
    scratch, out = args.scratch.resolve(), args.out.resolve()
    for directory in (scratch, out):
        directory.relative_to(driver.ROOT)
        directory.mkdir(parents=True, exist_ok=True)
    temp = scratch / "temp"
    temp.mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp))
    target = "test_llamacpp_v2_oracle_linux_x64"
    source = driver.ROOT / "test/cpp/test_llamacpp_v2_oracle.cpp"
    commands = json.loads((build / "compile_commands.json").read_text())
    row = next(
        row
        for row in commands
        if Path(row["file"]) == source and target + ".dir" in row["command"]
    )
    original = (source.parent / "llamacpp_v2_load_artifact_parity.h").read_text()
    before = 'test::make_scratch_dir("llamacpp-v2-artifacts")'
    after = '([] { const auto path = fs::temp_directory_path() / "llamacpp-v2-artifacts"; fs::remove_all(path); fs::create_directories(path); return path; }())'
    assert original.count(before) == 1
    records = []
    for arm in ("control", "shared-directory"):
        work = scratch / arm
        work.mkdir(exist_ok=True)
        copied = work / source.name
        copied.write_text(source.read_text())
        (work / "llamacpp_v2_oracle.h").write_text(
            (source.parent / "llamacpp_v2_oracle.h").read_text()
        )
        (work / "llamacpp_v2_load_artifact_parity.h").write_text(
            original if arm == "control" else original.replace(before, after)
        )
        command = shlex.split(row["command"])
        old_object = command[command.index("-o") + 1]
        obj = work / "probe.o"
        command[command.index("-o") + 1] = str(obj)
        command[command.index(str(source))] = str(copied)
        command += ["-iquote", str(source.parent)]
        command = ["-O0" if token.startswith("-O") else token for token in command]
        compiled = driver.run(command, build, work / "compile.log", env)
        assert compiled == 0
        line = subprocess.check_output(
            ["ninja", "-C", str(build), "-t", "commands", target], text=True
        ).splitlines()[-1]
        link = shlex.split(line)
        start = next(
            i
            for i, token in enumerate(link)
            if Path(token).name in ("c++", "g++", "clang++")
        )
        link = link[start:]
        if "&&" in link:
            link = link[: link.index("&&")]
        binary = work / "probe"
        link[link.index("-o") + 1] = str(binary)
        link[link.index(old_object)] = str(obj)
        linked = driver.run(link, build, work / "link.log", env)
        assert linked == 0
        processes = [
            subprocess.Popen(
                ["timeout", "120s", str(binary)],
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            for _ in range(5)
        ]
        runs = []
        for index, process in enumerate(processes):
            output = process.communicate()[0]
            (out / f"{arm}-{index}.log").write_text(output)
            runs.append(
                dict(
                    returncode=process.returncode,
                    named_failure="FAIL CompleteRawDiffEqualsSidecar:linux_x64"
                    in output,
                )
            )
        caught = (
            all(run["returncode"] == 0 for run in runs)
            if arm == "control"
            else all(run["returncode"] in (0, 1) for run in runs)
            and any(run["returncode"] == 1 and run["named_failure"] for run in runs)
        )
        records.append(
            dict(
                rule="per-process-artifact-isolation",
                arm=arm,
                before=before,
                after=after,
                compile=compiled,
                link=linked,
                runs=runs,
                caught=caught,
            )
        )
        (out / "results.json").write_text(json.dumps(records, indent=2) + "\n")
        assert caught, records[-1]
    print(
        "Parallel control 5/5 green; shared-directory mutant caught by CompleteRawDiffEqualsSidecar:linux_x64"
    )


if __name__ == "__main__":
    main()
