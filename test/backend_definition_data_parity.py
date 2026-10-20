"""Compare complete descriptor data and ABI metadata in three compiler views."""

import argparse
import difflib
import json
from pathlib import Path
import shlex
import subprocess

REPO = Path(__file__).resolve().parents[1]
CONFIGURED = Path("src/cpp/server/backends/configured")


def baseline(commit, path):
    return subprocess.check_output(
        ["git", "show", f"{commit}:{path}"], cwd=REPO, text=True
    )


def compiler_inputs(build):
    database = json.loads((build / "compile_commands.json").read_text())
    entry = next(
        row
        for row in database
        if row["file"].endswith("test_backend_definition_descriptor.cpp")
    )
    command = shlex.split(entry["command"])
    includes = []
    index = 1
    while index < len(command):
        token = command[index]
        if token in ("-I", "-isystem", "-iquote"):
            includes.extend(command[index : index + 2])
            index += 2
        else:
            if token.startswith(("-I", "-isystem", "-std=")):
                includes.append(token)
            index += 1
    links = subprocess.check_output(
        [
            "ninja",
            "-C",
            str(build),
            "-t",
            "commands",
            "test_backend_definition_descriptor",
        ],
        text=True,
    ).splitlines()[-1]
    libraries = [
        str((build / token).resolve()) if not Path(token).is_absolute() else token
        for token in shlex.split(links)
        if token.endswith(".a")
    ]
    return command[0], includes, libraries


def compile_views(args):
    protected = [
        "cmake/BackendDescriptorFromDefinition.cmake",
        "src/cpp/include/lemon/backends/backend_descriptor.h",
        "src/cpp/server/backends/configured/configured_backend_entry.cpp",
        "src/cpp/server/backends/backend_option_contract.cpp",
        "src/cpp/resources/backends",
    ]
    subprocess.run(
        ["git", "diff", "--exit-code", args.baseline_commit, "--", *protected],
        cwd=REPO,
        check=True,
        capture_output=True,
    )
    compiler, includes, libraries = compiler_inputs(args.build_dir)
    runtime = baseline(
        args.baseline_commit, str(CONFIGURED / "configured_backend_entry.cpp")
    )
    start = runtime.index("BackendDescriptor descriptor_from(")
    runtime = runtime[start : runtime.index("\n}\n}", start) + 2]
    test = (REPO / "test/cpp/test_backend_definition_descriptor.cpp").read_text()
    start = test.index("Json dump(")
    dumper = test[start : test.index("\nJson read(", start)]
    source = (
        '#include "backend_definition_exports_internal.h"\n'
        '#include "backend_descriptors_generated.h"\n'
        '#include "lemon/backends/backend_option_contract.h"\n'
        '#include "lemon/backends/configured/backend_definition.h"\n'
        '#include "lemon/backends/configured/backend_definition_descriptor.h"\n'
        '#include "lemon/backends/configured/install_context.h"\n'
        "#include <fstream>\n#include <iostream>\n"
        "namespace lemon::backends {\n"
        'PlatformInfo native_platform() { return {DescriptorPlatform::native().os,"x86_64",""}; }\n'
        + runtime
        + "\n}\nusing namespace lemon;\nusing namespace lemon::backends;\n"
        "using Json = nlohmann::json;\n" + dumper + r"""
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    Json output = {{"descriptors", Json::object()}, {"mapper", Json::array()}};
    for (const auto* descriptor : all_generated_descriptors()) output["descriptors"][descriptor->recipe] = dump(*descriptor);
    const auto inputs = Json::parse(std::ifstream(argv[1]));
    for (const auto& input : inputs) {
        BackendDefinition definition;
        definition.records = input.at("declaration");
        definition.recipe = definition.records.at("recipe");
        const auto external = input.at("external").get<bool>();
        definition.origin = external ? DefinitionOrigin::External : DefinitionOrigin::Builtin;
        const auto expected = dump(descriptor_from(definition));
        const auto actual = dump(descriptor_from_declaration(definition.records,
            external ? DefinitionNamespace::External : DefinitionNamespace::Builtin, DescriptorPlatform::native()));
        if (expected != actual) return 1;
        output["mapper"].push_back(actual);
    }
#ifdef LEMON_DUMP_ABI
    output["registry"] = {{"exports", installed_builtin_exports().exports().size()},
        {"digest", installed_builtin_exports().digest()}, {"abis", v2::hook_abi_json(installed_hook_abis())},
        {"abi_digest", installed_hook_abi_digest()}};
#endif
    std::cout << output.dump() << '\n';
}
"""
    )
    (args.out / "driver.cpp").write_text(source)
    old_mapper = baseline(
        args.baseline_commit, str(CONFIGURED / "backend_definition_descriptor.cpp")
    )
    (args.out / "before-mapper.cpp").write_text(old_mapper)
    inputs = [
        {"declaration": json.loads(path.read_text()), "external": False}
        for path in sorted((REPO / "src/cpp/resources/backends").glob("*.json"))
    ]
    for value in (None, 1.25, [True, {"a": 'quote"\\é'}]):
        inputs.append(
            {
                "declaration": {
                    "recipe": "synthetic",
                    "metadata": {
                        "display_name": "Synthetic",
                        "binary": {"windows": "a.exe", "linux": "a", "macos": "b"},
                        "options": [{"name": "fork_args", "default_value": value}],
                    },
                },
                "external": True,
            }
        )
    (args.out / "inputs.json").write_text(json.dumps(inputs, ensure_ascii=False))
    flags = {
        "native": [],
        "windows": ["-U__linux__", "-D_WIN32"],
        "macos": ["-U__linux__", "-D__APPLE__"],
    }
    for view in args.views:
        for side in ("before", "after"):
            mapper = (
                args.out / "before-mapper.cpp"
                if side == "before"
                else REPO / CONFIGURED / "backend_definition_descriptor.cpp"
            )
            command = [
                compiler,
                "-O1",
                *includes,
                *flags[view],
                "-I" + str(REPO / CONFIGURED),
                *(["-DLEMON_DUMP_ABI"] if side == "after" else []),
                str(args.out / "driver.cpp"),
                str(mapper),
                str(REPO / "src/cpp/server/backends/backend_option_contract.cpp"),
                *libraries,
                "-o",
                str(args.out / f"{side}-{view}"),
            ]
            with (args.out / f"{side}-{view}.build.log").open("w") as log:
                subprocess.run(
                    command,
                    cwd=args.build_dir,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    check=True,
                    timeout=120,
                )


def run_views(args):
    registry = None
    for view in args.views:
        results = {}
        for side in ("before", "after"):
            process = subprocess.run(
                [str(args.out / f"{side}-{view}"), str(args.out / "inputs.json")],
                capture_output=True,
                text=True,
                check=True,
                timeout=30,
            )
            (args.out / f"{side}-{view}.json").write_text(process.stdout)
            results[side] = json.loads(process.stdout)
        current_registry = results["after"].pop("registry")
        if registry is None:
            registry = current_registry
        assert registry == current_registry
        before = json.dumps(results["before"], indent=2, sort_keys=True) + "\n"
        after = json.dumps(results["after"], indent=2, sort_keys=True) + "\n"
        difference = "".join(
            difflib.unified_diff(before.splitlines(True), after.splitlines(True))
        )
        (args.out / f"{view}.diff").write_text(difference)
        assert not difference, view
        assert len(results["after"]["descriptors"]) == 17
        assert len(results["after"]["mapper"]) == 11
        print(
            f"{view}: 17 full descriptors, 8 production and 3 synthetic mapper cases identical"
        )
    assert registry["exports"] == 0 and len(registry["abis"]) == 12
    (args.out / "registry.json").write_text(json.dumps(registry, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--baseline-commit", required=True)
    parser.add_argument(
        "--views", nargs="+", choices=["native", "windows", "macos"], required=True
    )
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--mode", choices=["all", "compile", "run"], default="all")
    args = parser.parse_args()
    args.build_dir = args.build_dir.resolve()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    if args.mode != "run":
        compile_views(args)
    if args.mode != "compile":
        run_views(args)


if __name__ == "__main__":
    main()
