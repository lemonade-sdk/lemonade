"""Compile isolated container runtime mutants and require named assertion failures."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

REPO = Path(__file__).resolve().parents[1]
SOURCE = REPO / "src/cpp/server/utils/container_manager.cpp"

MUTATIONS = [
    (
        "mutable-run-image",
        "    require_image(spec.image);",
        "",
        "Mutable or untrusted image cannot reach run",
    ),
    ("pull-always", '"--pull=never"', '"--pull=always"', "Run never pulls"),
    ("cap-drop", '        "--cap-drop=all",\n', "", "All capabilities dropped"),
    (
        "extra-capability",
        '        "--cap-drop=all",',
        '        "--cap-drop=all", "--cap-add", "SYS_ADMIN",',
        "Only granted capabilities added",
    ),
    (
        "no-new-privileges",
        '        "--security-opt=no-new-privileges",\n',
        "",
        "No new privileges",
    ),
    (
        "seccomp-unconfined",
        '        "--pull=never",',
        '        "--pull=never", "--security-opt=seccomp=unconfined",',
        "Default seccomp and no privileged mode",
    ),
    (
        "non-internal-network",
        '"network", "create", "--internal", "--label", kLabel',
        '"network", "create", "--label", kLabel',
        "Labelled internal network created",
    ),
    (
        "namespace",
        "return std::regex_match(image, repository);",
        "return !image.empty();",
        "Rejected image repository docker.io/other/toolbox",
    ),
    (
        "docker-desktop",
        'info.output.find("Docker Desktop") != std::string::npos',
        "false",
        "Unsupported Docker rejected before run",
    ),
    (
        "docker-rootless",
        'info.output.find("name=rootless") != std::string::npos',
        "false",
        "Unsupported Docker rejected before run",
    ),
    (
        "rootful-cni",
        '!\u002arootless && backend == "cni"',
        "false",
        "Unsafe or unknown Podman mode rejected",
    ),
    (
        "video-grant",
        'const std::string gid = host_.group_id("render");',
        'const std::string video = host_.group_id("video");\n'
        '            argv.insert(argv.end(), {"--group-add", video});\n'
        '            const std::string gid = host_.group_id("render");',
        "Render only explicit GPU grant",
    ),
    (
        "wrong-network",
        "const auto& attachment = networks.at(network);",
        "const auto& attachment = networks.begin().value();",
        "Wrong network address ignored",
    ),
    (
        "ambiguous-inspection",
        "!value.is_array() || value.size() != 1",
        "!value.is_array() || value.empty()",
        "Ambiguous inspection array rejected",
    ),
    (
        "ambiguous-address",
        '!attachment.value("GlobalIPv6Address", std::string()).empty()',
        "false",
        "Ambiguous dual address rejected",
    ),
    (
        "duplicate-inspection-key",
        "!keys.back().insert(parsed.get<std::string>()).second",
        "false",
        "Duplicate inspection keys rejected",
    ),
    (
        "writable-mount",
        'mount.source + ":" + mount.destination + ":ro,z"',
        'mount.source + ":" + mount.destination + ":rw,z"',
        "Read only SELinux mount",
    ),
    (
        "unlabelled-network",
        '"network", "create", "--internal", "--label", kLabel',
        '"network", "create", "--internal"',
        "Labelled internal network created",
    ),
    (
        "setup-video-grant",
        '"sudo usermod -aG render " + member',
        '"sudo usermod -aG video,render " + member',
        "Setup requests render only",
    ),
    (
        "wrong-container",
        'actual_name != name && actual_name != "/" + name',
        "false",
        "Wrong container rejected",
    ),
    (
        "unowned-container",
        'container.at("Config").at("Labels").value(kLabel, std::string("unowned")) != ""',
        "false",
        "Unlabelled container rejected",
    ),
    (
        "loopback-address",
        "parts[0] == 127",
        "false",
        "Invalid or missing address rejected 127.0.0.1",
    ),
    (
        "mutable-pin-syntax",
        "[0-9a-f]{64}",
        "[0-9a-fA-F]{64}",
        "Malformed pin rejected",
    ),
    (
        "missing-container-remove",
        "attempt([&] { remove_container(name); });",
        "",
        "Stop removes container and owned network",
    ),
]


def run(command, cwd, log, timeout=120):
    with log.open("w") as output:
        return subprocess.run(
            command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT, timeout=timeout
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument(
        "--scratch",
        type=Path,
        default=REPO / ".synagent/scratch/container-v2/mutations",
    )
    parser.add_argument(
        "--out", type=Path, default=REPO / ".synagent/artifacts/container-v2/mutations"
    )
    args = parser.parse_args()
    args.build, args.scratch, args.out = (
        p.resolve() for p in (args.build, args.scratch, args.out)
    )
    for path in (args.scratch, args.out):
        assert not path.is_relative_to(Path("/tmp")), "Persistent disk paths required"
        path.mkdir(parents=True, exist_ok=True)
    temporary = args.scratch / "tmp"
    temporary.mkdir(exist_ok=True)
    os.environ["TMPDIR"] = str(temporary)
    source = SOURCE.read_text()
    digest = hashlib.sha256(SOURCE.read_bytes()).hexdigest()
    database = json.loads((args.build / "compile_commands.json").read_bytes())
    row = next(r for r in database if Path(r["file"]) == SOURCE)
    compile_base = shlex.split(row["command"])
    object_name = compile_base[compile_base.index("-o") + 1]
    commands = subprocess.check_output(
        ["ninja", "-C", str(args.build), "-t", "commands", "test_container_manager"],
        text=True,
        timeout=30,
    ).splitlines()
    link_line = commands[-1].removeprefix(": && ").removesuffix(" && :")
    link_base = shlex.split(link_line)
    assert object_name in link_base, "Actual runtime object must be replaced"
    control = run(
        ["timeout", "60", str(args.build / "test_container_manager")],
        REPO,
        args.out / "control.log",
    )
    assert control.returncode == 0, "Control must pass before mutations"
    results = []
    for name, old, new, assertion in MUTATIONS:
        assert source.count(old) == 1, (name, source.count(old))
        work = args.scratch / name
        work.mkdir(exist_ok=True)
        mutated = work / "container_manager.cpp"
        mutated.write_text(source.replace(old, new))
        obj, executable = work / "runtime.o", work / "test_container_manager"
        compile_command = [
            str(mutated) if x == str(SOURCE) else str(obj) if x == object_name else x
            for x in compile_base
        ]
        for flag in ("-MF", "-MT"):
            if flag in compile_command:
                i = compile_command.index(flag)
                del compile_command[i : i + 2]
        built = run(
            compile_command, Path(row["directory"]), args.out / f"{name}.compile.log"
        )
        assert built.returncode == 0, f"{name}: compilation is not a mutation kill"
        link_command = [str(obj) if x == object_name else x for x in link_base]
        link_command[link_command.index("-o") + 1] = str(executable)
        linked = run(link_command, args.build, args.out / f"{name}.link.log")
        assert linked.returncode == 0, f"{name}: linking is not a mutation kill"
        log = args.out / f"{name}.run.log"
        result = run(["timeout", "60", str(executable)], REPO, log)
        caught = result.returncode == 1 and "FAIL " + assertion in log.read_text()
        results.append(
            dict(rule=name, exit=result.returncode, assertion=assertion, caught=caught)
        )
        (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(name, "caught" if caught else "SURVIVED", assertion, flush=True)
        assert caught, f"{name}: expected named assertion did not fail"
    assert hashlib.sha256(SOURCE.read_bytes()).hexdigest() == digest
    print(f"{len(results)}/{len(results)} independently compiled mutants caught")


if __name__ == "__main__":
    main()
