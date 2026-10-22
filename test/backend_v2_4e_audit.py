"""CPU-only cache, initialization, linkage and code-generation gates for units 4e.1 and 4e.2."""

import argparse
import copy
import json
import os
from pathlib import Path
import re
import subprocess
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
SCRATCH = REPO / ".synagent/scratch/backend-v2-4e1"


def run(command, **kwargs):
    return subprocess.run(command, check=True, timeout=120, **kwargs)


def initialization(args):
    results = []
    for binary, orders in (
        ("test_backend_builtin_source_data", ("source", "peer")),
        (
            "test_backend_builtin_source_cache",
            ("source", "peer", "freeze", "seeds", "registrations", "public-fallback"),
        ),
    ):
        for order in orders:
            process = run(
                [str(args.build / binary), order], capture_output=True, text=True
            )
            print(binary, order, process.stdout.strip())
            results.append({"binary": binary, "order": order, "output": process.stdout})
    if args.out:
        (args.out / "initialization.json").write_text(
            json.dumps(results, indent=2) + "\n"
        )


def linkage(args):
    rejected = re.compile(
        r"ConfiguredServer|DefinitionRunner|NativeProcess|typed::llamacpp::|"
        r"generated_builtin_seeds|builtin_registration_seeds|"
        r"observe_entry|test_program|test_targets|binding::counters|execution::counters"
    )
    for binary in ("lemonade", "test_backend_builtin_source_data"):
        listing = subprocess.check_output(
            ["nm", "-C", str(args.build / binary)], text=True
        )
        if args.out:
            (args.out / f"{binary}.symbols").write_text(listing)
        failures = [line for line in listing.splitlines() if rejected.search(line)]
        assert not failures, failures
        print(binary, "data-only dependency boundary passed")


def literals(args):
    count = 0
    for path in (args.build / "include").glob("backend*generated.h"):
        count += check_literals(path)
    for base in (args.build / "include", args.build / "test/source-cache"):
        for path in base.glob("backend*resources.h"):
            count += check_literals(path)
    print(count, "raw literals have at most 8000 content bytes")


def check_literals(path):
    data = path.read_bytes()
    matches = list(re.finditer(rb'R"([A-Za-z0-9_]*)\((.*?)\)\1"', data, re.DOTALL))
    for match in matches:
        assert len(match[2]) <= 8000, (path, len(match[2]))
    return len(matches)


def dispatch(args):
    work = args.scratch / "dispatch"
    work.mkdir(parents=True, exist_ok=True)
    script = work / "probe.cmake"
    fixture = REPO / "test/fixtures/backend-v2/source_cache.json"
    v1 = REPO / "src/cpp/resources/backends/ds4.json"
    prefix = (
        "cmake_minimum_required(VERSION 3.19)\n"
        f'include("{REPO}/cmake/BackendDefinitionResources.cmake")\n'
        f'include("{REPO}/cmake/BackendDescriptorFromDefinition.cmake")\n'
    )
    script.write_text(
        prefix
        + f'lemon_backend_descriptor_dispatch(v1 ds4 ds4 "{v1}")\n'
        + f'lemon_backend_descriptor_dispatch(v2 fixture source-cache-fixture "{fixture}")\n'
        + 'if(v1 MATCHES "builtin_resolved_source" OR NOT v1 MATCHES "d.recipe")\n'
        + ' message(FATAL_ERROR "V1DefinitionSelectsOnlyMetadataEmitter")\nendif()\n'
        + 'if(NOT v2 MATCHES "builtin_resolved_source" OR v2 MATCHES "d.recipe")\n'
        + ' message(FATAL_ERROR "V2DefinitionSelectsOnlyResolvedSourceEmitter")\nendif()\n'
    )
    run(["cmake", "-P", str(script)], capture_output=True)
    for version in (0, 3, "2", None, True, 2.0, 2.5):
        value = json.loads(fixture.read_text())
        value["schema_version"] = version
        source = work / "unknown.json"
        source.write_text(json.dumps(value))
        script.write_text(
            prefix
            + f'lemon_backend_descriptor_dispatch(out fixture source-cache-fixture "{source}")\n'
        )
        result = subprocess.run(
            ["cmake", "-P", str(script)], capture_output=True, text=True, timeout=30
        )
        assert result.returncode and "/schema_version" in result.stderr, (
            version,
            result,
        )
    print("V1 and V2 dispatch exact; UnknownEnvelopeVersionFailsConfigure (7 cases)")


def census(args):
    current = json.loads(
        subprocess.check_output(
            [
                "ctest",
                "--test-dir",
                str(args.build),
                "-L",
                "^cpp-ci$",
                "--show-only=json-v1",
            ],
            text=True,
        )
    )
    before = json.loads(args.baseline.read_text())
    names = lambda value: {
        row["name"]
        for row in value["tests"]
        if any(
            p["name"] == "LABELS" and "cpp-ci" in p["value"] for p in row["properties"]
        )
    }
    old, new = names(before), names(current)
    additions = (
        {"BackendBuiltinSourceCacheTest"}
        if args.expected_count == 145
        else {
            "LlamaCppV2Oracle_" + view + "Test"
            for view in ("linux_x64", "linux_arm64", "windows_x64", "macos_arm64")
        }
        | {"LlamaCppV2DeliberateDiffTest"}
    )
    if args.expected_count == 151:
        additions = {"LlamaCppV2ProductionSwitchTest"}
    if getattr(args, "no_additions", False):
        additions = set()
    assert args.expected_count in (145, 150, 151)
    assert len(new) == args.expected_count and len(old) == args.expected_count - len(
        additions
    ), "CppCiCountExact"
    assert new - old == additions and not old - new, "CppCiNamesExact"
    dependencies = subprocess.check_output(
        ["ninja", "-C", str(args.build), "-t", "query", "cpp-ci-tests"], text=True
    )
    targets = (
        {"test_backend_builtin_source_cache", "test_backend_builtin_source_data"}
        if args.expected_count == 145
        else {
            "test_llamacpp_v2_oracle_" + view
            for view in ("linux_x64", "linux_arm64", "windows_x64", "macos_arm64")
        }
        | {"test_llamacpp_v2_deliberate_diff"}
    )
    if args.expected_count == 151:
        targets = {"test_llamacpp_v2_production_switch"}
    if getattr(args, "no_additions", False):
        targets |= {
            Path(row["command"][0]).name
            for row in current["tests"]
            if row.get("command") and Path(row["command"][0]).name.startswith("test_")
        } | {
            "test_backend_builtin_source_data",
            "lemond",
            "lemonade",
            "llamacpp-frozen-control-build",
        }
    assert all(
        target in dependencies.split() for target in targets
    ), "CppCiExecutableDependencies"
    if args.out:
        (args.out / "cpp-ci.json").write_text(json.dumps(current, indent=2) + "\n")
        (args.out / "census.json").write_text(
            json.dumps(
                {
                    "before": len(old),
                    "after": len(new),
                    "added": sorted(new - old),
                    "dropped": sorted(old - new),
                },
                indent=2,
            )
            + "\n"
        )
        (args.out / "dependencies.txt").write_text(dependencies)
    print(
        f"cpp-ci: {len(old)} -> {len(new)}; exact additions {sorted(additions)}; nothing dropped; executable dependencies checked"
    )


def data_parity(args):
    run(
        [
            "python3",
            str(REPO / "test/backend_definition_data_parity.py"),
            "--build-dir",
            str(args.build),
            "--baseline-commit",
            args.baseline_commit,
            "--views",
            *args.views,
            "--out",
            str(args.scratch / "data-parity"),
        ]
    )


def oracle_controls(args):
    from llamacpp_frozen_control import ROOT

    run(
        [
            "python3",
            str(REPO / "test/llamacpp_oracle_audit.py"),
            "equality",
            "--build",
            str(ROOT / "build"),
            "--out",
            str(args.scratch / "oracle-controls"),
        ]
    )


def census_mutations(args):
    current = json.loads(
        subprocess.check_output(
            [
                "ctest",
                "--test-dir",
                str(args.build),
                "-L",
                "^cpp-ci$",
                "--show-only=json-v1",
            ],
            text=True,
        )
    )
    dependencies = subprocess.check_output(
        ["ninja", "-C", str(args.build), "-t", "query", "cpp-ci-tests"], text=True
    )
    executable = (
        "test_llamacpp_v2_production_switch"
        if args.expected_count == 151
        else "test_llamacpp_v2_deliberate_diff"
    )
    cases = []
    missing = copy.deepcopy(current)
    missing["tests"].remove(
        next(
            row
            for row in missing["tests"]
            if row["name"] == "LlamaCppV2DeliberateDiffTest"
        )
    )
    cases.append(("missing-registration", missing, dependencies, "CppCiCountExact"))
    extra = copy.deepcopy(current)
    extra["tests"].append(
        dict(name="ExtraTest", properties=[dict(name="LABELS", value=["cpp-ci"])])
    )
    cases.append(("extra-registration", extra, dependencies, "CppCiCountExact"))
    dropped = copy.deepcopy(current)
    next(
        row
        for row in dropped["tests"]
        if row["name"] == "BackendBuiltinSourceCacheTest"
    )["name"] = "ReplacementTest"
    cases.append(("prior-test-dropped", dropped, dependencies, "CppCiNamesExact"))
    cases.append(
        (
            "missing-executable-dependency",
            current,
            dependencies.replace(executable, "omitted"),
            "CppCiExecutableDependencies",
        )
    )
    results = []
    census(args)
    for name, value, deps, assertion in cases:
        fixture = args.scratch / (name + ".json")
        fixture.write_text(
            json.dumps(dict(actual=value, dependencies=deps), indent=2) + "\n"
        )

        def output(command, **_):
            return json.dumps(value) if command[0] == "ctest" else deps

        with mock.patch.object(subprocess, "check_output", side_effect=output):
            try:
                census(args)
            except AssertionError as error:
                assert str(error) == assertion, (name, error)
            else:
                raise AssertionError("Uncaught census mutation: " + name)
        results.append(dict(rule=name, assertion=assertion, caught=True))
    (args.out / "census-mutations.json").write_text(
        json.dumps(results, indent=2) + "\n"
    )
    print("Four census mutations caught by the actual census assertions")


def synthetic_codegen(args):
    from backend_definition_data_parity import baseline, compiler_inputs

    work = args.scratch / "synthetic-codegen"
    work.mkdir(parents=True, exist_ok=True)
    values = [None, 1.25, True, [False, {"nested": 'quote"\\é'}], 'line\nquote"\\é']
    declaration = {
        "schema_version": 1,
        "id": "synthetic-codegen",
        "recipe": "synthetic-codegen",
        "metadata": {
            "display_name": "Synthetic",
            "binary": {"windows": "a.exe", "linux": "a", "macos": "b"},
            "options": [
                {"name": "synthetic_" + str(i), "default_value": value}
                for i, value in enumerate(values)
            ],
            "config_extra": {
                "float": 1.25,
                "null": None,
                "nested": [True, {"escaped": 'quote"\\é'}],
            },
        },
    }
    data = work / "synthetic.json"
    data.write_text(json.dumps(declaration, ensure_ascii=False))
    script = work / "generate.cmake"
    script.write_text(
        'cmake_minimum_required(VERSION 3.19)\ninclude("'
        + str(REPO / "cmake/BackendDefinitionResources.cmake")
        + '")\ninclude("'
        + str(REPO / "cmake/BackendDescriptorFromDefinition.cmake")
        + '")\nlemon_backend_descriptor_dispatch(value synthetic_codegen synthetic-codegen "'
        + str(data)
        + '")\nfile(WRITE "'
        + str(work / "generated.h")
        + '" "${value}")\n'
    )
    run(["cmake", "-P", str(script)], capture_output=True)
    runtime = baseline(
        args.baseline_commit,
        "src/cpp/server/backends/configured/configured_backend_entry.cpp",
    )
    start = runtime.index("BackendDescriptor descriptor_from(")
    runtime = runtime[start : runtime.index("\n}\n}", start) + 2]
    test = (REPO / "test/cpp/test_backend_definition_descriptor.cpp").read_text()
    start = test.index("Json dump(")
    dump = test[start : test.index("\nJson read(", start)]
    source = '#include "lemon/backends/backend_option_contract.h"\n#include "lemon/backends/configured/backend_definition.h"\n#include "lemon/backends/configured/backend_definition_descriptor.h"\n#include "lemon/backends/configured/install_context.h"\n#include <fstream>\n#include <iostream>\n'
    source += (
        'namespace lemon::backends { PlatformInfo native_platform() { return {DescriptorPlatform::native().os,"x86_64",""}; }\n'
        + runtime
        + "\n}\nusing namespace lemon; using namespace lemon::backends; using Json = nlohmann::json;\n"
    )
    source += (
        dump
        + '\n#include "generated.h"\nint main(int argc, char** argv) { if(argc != 2) return 2; BackendDefinition definition; definition.records = Json::parse(std::ifstream(argv[1])); definition.recipe = definition.records.at("recipe"); definition.origin = DefinitionOrigin::Builtin; const auto expected = dump(descriptor_from(definition)); const auto actual = dump(synthetic_codegen::descriptor); if(expected.dump() != actual.dump()) return 1; std::cout << actual.dump() << "\\n"; return 0; }\n'
    )
    (work / "probe.cpp").write_text(source)
    compiler, includes, libraries = compiler_inputs(args.build)
    for view in args.views:
        flags = {
            "native": [],
            "windows": ["-U__linux__", "-D_WIN32"],
            "macos": ["-U__linux__", "-D__APPLE__"],
        }[view]
        binary = work / view
        with (work / (view + ".build.log")).open("w") as log:
            run(
                [
                    compiler,
                    "-O0",
                    *includes,
                    *flags,
                    str(work / "probe.cpp"),
                    str(
                        REPO
                        / "src/cpp/server/backends/configured/backend_definition_descriptor.cpp"
                    ),
                    str(REPO / "src/cpp/server/backends/backend_option_contract.cpp"),
                    *libraries,
                    "-o",
                    str(binary),
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
            )
        result = run([str(binary), str(data)], capture_output=True, text=True)
        (args.out / ("synthetic-" + view + ".json")).write_text(result.stdout)
        print(
            view,
            "generated fields/types match verbatim frozen mapper: floats, null, booleans, nested JSON, escapes and absent optionals",
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "gate",
        choices=[
            "initialization",
            "linkage",
            "literals",
            "dispatch",
            "mutations",
            "census",
            "census-mutations",
            "data-parity",
            "oracle-controls",
            "synthetic-codegen",
            "catalog-transition",
        ],
    )
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument("--scratch", type=Path, default=SCRATCH)
    parser.add_argument("--out", type=Path)
    parser.add_argument(
        "--baseline",
        type=Path,
        default=REPO / ".synagent/artifacts/backend-v2-4e2a/census/cpp-ci.json",
    )
    parser.add_argument(
        "--baseline-commit", default="789ca434131bde84e2c55e35bec3c0818f35900f"
    )
    parser.add_argument("--no-additions", action="store_true")
    parser.add_argument("--expected-count", type=int, default=150)
    parser.add_argument(
        "--views",
        nargs="+",
        choices=["native", "windows", "macos"],
        default=["native", "windows", "macos"],
    )
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.scratch = args.scratch.resolve()
    args.scratch.mkdir(parents=True, exist_ok=True)
    temp = args.scratch / "tmp"
    temp.mkdir(exist_ok=True)
    os.environ.update(TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp))
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
    if args.gate == "catalog-transition":
        assert args.out, "catalog-transition requires --out"
        run(
            [
                "python3",
                str(REPO / "test/backend_v2_4e3b_transition.py"),
                "run",
                "--build",
                str(args.build),
                "--out",
                str(args.out),
                "--scratch",
                str(args.scratch),
            ]
        )
    elif args.gate == "mutations":
        from backend_v2_4e_mutations import mutations

        assert args.out, "mutations requires --out"
        mutations(args)
    else:
        globals()[args.gate.replace("-", "_")](args)


if __name__ == "__main__":
    main()
