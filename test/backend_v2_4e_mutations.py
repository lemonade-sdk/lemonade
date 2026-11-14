"""Compile, link and execute isolated source-cache mutants without editing the checkout."""

import hashlib
import json
from pathlib import Path
import shlex
import subprocess

REPO = Path(__file__).resolve().parents[1]
CONFIGURED = "src/cpp/server/backends/configured/"


def execute(command, cwd, log, expected=0):
    process = subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=180,
    )
    log.write_text(process.stdout)
    assert process.returncode == expected, (
        command,
        process.returncode,
        process.stdout[-3000:],
    )
    return process.stdout


def link_command(build, target):
    lines = subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "commands", target], text=True
    ).splitlines()
    tokens = shlex.split(lines[-1])
    start = next(
        i
        for i, token in enumerate(tokens)
        if Path(token).name in ("c++", "g++", "clang++")
    )
    tokens = tokens[start:]
    if "&&" in tokens:
        tokens = tokens[: tokens.index("&&")]
    return tokens


def binary_mutation(args, name, source, old, new, assertion, target, control=False):
    work = args.scratch / "mutations" / name
    work.mkdir(parents=True, exist_ok=True)
    resources = work / "resources"
    if not resources.exists():
        resources.symlink_to(args.build / "resources", target_is_directory=True)
    original = REPO / source
    text = original.read_text()
    assert old in text, (name, old)
    replacement = work / original.name
    replacement.write_text(text if control else text.replace(old, new, 1))
    database = json.loads((args.build / "compile_commands.json").read_text())
    candidates = [row for row in database if Path(row["file"]) == original]
    row = next(
        (row for row in candidates if f"/{target}.dir/" in row["command"]),
        candidates[0],
    )
    command = shlex.split(row["command"])
    object_index = command.index("-o") + 1
    old_object = command[object_index]
    obj = work / "mutant.o"
    command[object_index] = str(obj)
    command[command.index(str(original))] = str(replacement)
    command.extend(["-iquote", str(original.parent)])
    command = ["-O0" if token.startswith("-O") else token for token in command]
    execute(command, args.build, work / "compile.log")
    link = link_command(args.build, target)
    binary = work / "probe"
    link[link.index("-o") + 1] = str(binary)
    if old_object in link:
        link[link.index(old_object)] = str(obj)
    else:
        link.insert(1, str(obj))
    execute(link, args.build, work / "link.log")
    output = execute(
        [
            str(binary),
            (
                "registrations"
                if name == "builtin-fallback-exposes-unfinished-seeds"
                else "source"
            ),
        ],
        args.build,
        work / "run.log",
        expected=0 if control else 1,
    )
    assert control or ("FAIL " + assertion) in output, (name, output)
    return {
        "name": name,
        "source": source,
        "source_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
        "compiled": True,
        "linked": True,
        "exit": 0 if control else 1,
        "assertion": assertion,
        "output": output,
    }


def mutations(args):
    minimal = "test_backend_builtin_source_data"
    full = "test_backend_builtin_source_cache"
    cache = CONFIGURED + "backend_builtin_sources.cpp"
    cases = [
        (
            "initializer-runtime-registration",
            cache,
            '#include "backend_builtin_source_resources.h"',
            '#include "backend_builtin_source_resources.h"\n#include "lemon/backends/backend_registry.h"',
            "NoRegistrationDuringDescriptorInit",
            minimal,
        ),
        (
            "initializer-publication-consumption",
            cache,
            '#include "backend_builtin_source_resources.h"',
            '#include "backend_builtin_source_resources.h"\n#include "lemon/backends/backend_descriptor_registry.h"',
            "NoPublicationConsumptionDuringDescriptorInit",
            minimal,
        ),
        (
            "cache-wrong-namespace",
            cache,
            "found.space == key.space && ",
            "",
            "ResolvedCacheRejectsUnknownExactKey",
            minimal,
        ),
        (
            "cache-wrong-contract",
            cache,
            " && found.contract == key.contract",
            "",
            "ResolvedCacheRejectsUnknownExactKey",
            minimal,
        ),
        (
            "cache-wrong-id",
            cache,
            " && found.id == key.id",
            "",
            "ResolvedCacheRejectsUnknownExactKey",
            minimal,
        ),
        (
            "cache-corrupt-resolved-data",
            cache,
            "definitions_.push_back(result.definition);",
            'auto corrupt = std::make_shared<ResolvedDefinition>(*result.definition);\n            corrupt->declaration["metadata"]["display_name"] = "corrupt";\n            definitions_.push_back(corrupt);',
            "ResolvedCacheUsesEmbeddedBytes",
            minimal,
        ),
        (
            "entry-owned-registration-address",
            CONFIGURED + "backend_catalog.cpp",
            "registrations_.push_back({&entry->published_descriptor(),",
            "registrations_.push_back({&entry->descriptor,",
            "CanonicalDescriptorSurvivesFinalization",
            full,
        ),
        (
            "entry-owned-server-address",
            CONFIGURED + "configured_server.cpp",
            "set_descriptor(&entry_->published_descriptor());",
            "set_descriptor(&entry_->descriptor);",
            "ConfiguredServerUsesCanonicalDescriptor",
            full,
        ),
        (
            "canonical-equality-bypassed",
            CONFIGURED + "configured_v2_backend_ops.cpp",
            "if (canonical && (",
            "if (false && canonical && (",
            "CanonicalDescriptorRequiresCompleteFieldEquality",
            full,
        ),
        (
            "builtin-entry-loses-canonical-reference",
            CONFIGURED + "backend_catalog_execution.cpp",
            "BuiltinEntryBuilder::make(definition, *seed->descriptor)",
            "ConfiguredBackendEntry::make(definition)",
            "BuiltinCandidateFinalizes",
            full,
        ),
        (
            "canonical-json-type-check-removed",
            CONFIGURED + "configured_v2_backend_ops.cpp",
            "left.type() != right.type() || ",
            "",
            "CanonicalDescriptorRequiresCompleteFieldEquality",
            full,
        ),
        (
            "builtin-fallback-exposes-unfinished-seeds",
            "src/cpp/server/backends/backend_registry.cpp",
            "if (!has_v2)",
            "if (true)",
            "BuiltinOnlyFallbackIsCompletelyFinalized",
            full,
        ),
        (
            "embedded-sources-double-charged",
            CONFIGURED + "backend_catalog_admission.cpp",
            "frozen->definitions_ = k_backend_definition_v1_source_count;",
            "frozen->definitions_ = k_backend_definition_source_count;",
            "BuiltinStartupFailsClosed",
            full,
        ),
        (
            "embedded-bytes-double-charged",
            CONFIGURED + "backend_catalog_admission.cpp",
            "frozen->source_bytes_ = k_backend_definition_v1_source_bytes;",
            "frozen->source_bytes_ = k_backend_definition_source_bytes;",
            "BuiltinStartupFailsClosed",
            full,
        ),
    ]
    results = []
    for name, source, old, new, assertion, target in cases:
        if name.startswith("initializer-"):
            work = args.scratch / "mutations" / name
            original = (REPO / source).read_text()
            added = (
                "builtin_registrations();"
                if name.endswith("registration")
                else 'descriptor_for("llamacpp");'
            )
            anchor = "    static const ResolvedSources sources;"
            old = original
            new = original.replace(old.splitlines()[0], new, 1).replace(
                anchor, "    " + added + "\n" + anchor
            )
        if not any(row.get("control_for") == source for row in results):
            result = binary_mutation(
                args,
                "control-" + Path(source).stem + "-" + target,
                source,
                old,
                new,
                assertion,
                target,
                control=True,
            )
            result["control_for"] = source
            results.append(result)
        result = binary_mutation(args, name, source, old, new, assertion, target)
        results.append(result)
        print(name, "caught by", assertion, flush=True)
        (args.out / "mutations.json").write_text(json.dumps(results, indent=2) + "\n")
    inline_linkage_mutation(args, results)
    cmake_mutations(args, results)
    unknown_version_mutation(args, results)
    (args.out / "mutations.json").write_text(json.dumps(results, indent=2) + "\n")


def generated_probe(args, work, source, assertion, failure):
    from backend_definition_data_parity import compiler_inputs

    compiler, includes, libraries = compiler_inputs(args.build)
    path = work / "probe.cpp"
    path.write_text(source)
    binary = work / "compiled-probe"
    execute(
        [compiler, "-O0", *includes, str(path), *libraries, "-o", str(binary)],
        args.build,
        work / "compile-link.log",
    )
    output = execute(
        [str(binary)], args.build, work / "assertion.log", expected=1 if failure else 0
    )
    assert not failure or assertion in output


def cmake_mutations(args, results):
    for name, file, old, new, assertion in (
        (
            "v1-selects-v2",
            "BackendDescriptorFromDefinition.cmake",
            "if(_version EQUAL 1)",
            "if(_version EQUAL 0)",
            "V1DefinitionSelectsOnlyMetadataEmitter",
        ),
        (
            "literal-budget-breached",
            "CxxStringLiterals.cmake",
            "set(LEMON_CXX_LITERAL_BUDGET 8000)",
            "set(LEMON_CXX_LITERAL_BUDGET 64000)",
            "LiteralContentAtMost8000Bytes",
        ),
    ):
        for control in (True, False):
            work = args.scratch / "mutations" / (("control-" if control else "") + name)
            work.mkdir(parents=True, exist_ok=True)
            for module in (
                "BackendDescriptorFromDefinition.cmake",
                "BackendDefinitionResources.cmake",
                "CxxStringLiterals.cmake",
            ):
                text = (REPO / "cmake" / module).read_text()
                if module == file and not control:
                    assert old in text
                    text = text.replace(old, new, 1)
                (work / module).write_text(text)
            prefix = (
                "cmake_minimum_required(VERSION 3.19)\n"
                + f'include("{work}/BackendDefinitionResources.cmake")\ninclude("{work}/BackendDescriptorFromDefinition.cmake")\n'
            )
            v1 = REPO / "src/cpp/resources/backends/hrx.json"
            if name == "v1-selects-v2":
                body = f'lemon_backend_descriptor_dispatch(value hrx llamacpp-hrx "{v1}")\n'
                body += 'file(WRITE "' + str(work / "generated.h") + '" "${value}")\n'
                probe = '#include "lemon/backends/configured/backend_builtin_sources.h"\n#include "lemon/backends/configured/backend_definition_descriptor.h"\n#include <cstdio>\n'
                probe += (
                    'unsigned entered = 0;\nnamespace lemon::backends {\nconst ResolvedDefinition& builtin_resolved_source(const DefinitionKey&) {\n++entered;\nstatic const auto source = [] { ResolvedDefinition r; r.declaration = nlohmann::json::parse(R"control('
                    + v1.read_text()
                    + ')control"); return r; }();\nreturn source; }\n}\n'
                )
                probe += (
                    '#include "'
                    + str(work / "generated.h")
                    + '"\nint main() { if(entered) { std::puts("FAIL '
                    + assertion
                    + '"); return 1; } return 0; }\n'
                )
            else:
                body = (
                    'string(REPEAT "x" 16001 input)\nlemon_cxx_raw_string_literals(value json test "${input}")\n'
                    + 'file(WRITE "'
                    + str(work / "generated.h")
                    + '" "inline constexpr char payload[] = ${value};")\n'
                )
                probe = (
                    '#include <cstdio>\n#include <fstream>\n#include <iterator>\n#include <regex>\n#include <string>\n#include "'
                    + str(work / "generated.h")
                    + '"\n'
                )
                probe += (
                    'int main() { std::ifstream f("'
                    + str(work / "generated.h")
                    + '"); const std::string text((std::istreambuf_iterator<char>(f)), {}); const std::regex raw(R"pattern(R"json\\(([\\s\\S]*?)\\)json")pattern"); for (auto it = std::sregex_iterator(text.begin(), text.end(), raw); it != std::sregex_iterator(); ++it) if ((*it)[1].length() > 8000) { std::puts("FAIL '
                    + assertion
                    + '"); return 1; } return sizeof(payload) == 16002 ? 0 : 2; }\n'
                )
            script = work / "probe.cmake"
            script.write_text(prefix + body)
            execute(["cmake", "-P", str(script)], args.build, work / "configure.log")
            generated_probe(args, work, probe, assertion, not control)
        results.append(
            {
                "name": name,
                "compiled": True,
                "linked": True,
                "assertion": assertion,
                "exit": 1,
                "caught": True,
            }
        )
        print(name, "compiled, linked and caught by", assertion, flush=True)


def inline_linkage_mutation(args, results):
    target = "test_backend_builtin_source_data"
    database = json.loads((args.build / "compile_commands.json").read_text())
    header = args.build / "test/source-cache/backend_builtin_source_test_descriptor.h"
    for control in (True, False):
        work = (
            args.scratch
            / "mutations"
            / (("control-" if control else "") + "inline-descriptor-linkage")
        )
        work.mkdir(parents=True, exist_ok=True)
        text = header.read_text()
        if not control:
            text = text.replace(
                "inline const BackendDescriptor", "static const BackendDescriptor", 1
            )
        (work / header.name).write_text(text)
        link = link_command(args.build, target)
        for name in (
            "test_backend_builtin_source_cache.cpp",
            "backend_builtin_source_cache_peer.cpp",
        ):
            row = next(
                row
                for row in database
                if Path(row["file"]).name == name
                and f"/{target}.dir/" in row["command"]
            )
            command = shlex.split(row["command"])
            old_object = command[command.index("-o") + 1]
            obj = work / (name + ".o")
            command[command.index("-o") + 1] = str(obj)
            command.insert(1, "-I" + str(work))
            command = ["-O0" if token.startswith("-O") else token for token in command]
            execute(command, args.build, work / (name + ".compile.log"))
            link[link.index(old_object)] = str(obj)
        binary = work / "probe"
        link[link.index("-o") + 1] = str(binary)
        execute(link, args.build, work / "link.log")
        output = execute(
            [str(binary), "source"],
            args.build,
            work / "run.log",
            expected=0 if control else 1,
        )
        assert control or "FAIL SameDescriptorAddressAcrossTranslationUnits" in output
    results.append(
        {
            "name": "inline-descriptor-linkage",
            "compiled": True,
            "linked": True,
            "exit": 1,
            "assertion": "SameDescriptorAddressAcrossTranslationUnits",
            "caught": True,
        }
    )
    print(
        "inline-descriptor-linkage compiled, linked and caught by SameDescriptorAddressAcrossTranslationUnits",
        flush=True,
    )


def unknown_version_mutation(args, results):
    assertion = "UnknownEnvelopeVersionFailsConfigure"
    for control in (True, False):
        work = (
            args.scratch
            / "mutations"
            / (("control-" if control else "") + "unknown-envelope-version")
        )
        work.mkdir(parents=True, exist_ok=True)
        for module in ("BackendDefinitionResources.cmake", "CxxStringLiterals.cmake"):
            text = (REPO / "cmake" / module).read_text()
            if module == "BackendDefinitionResources.cmake" and not control:
                text = text.replace(
                    'if(NOT _version STREQUAL "1" AND NOT _version STREQUAL "2")',
                    "if(FALSE)",
                    1,
                )
            (work / module).write_text(text)
        source = work / "unknown.json"
        source.write_text('{"schema_version":3}')
        script = work / "probe.cmake"
        script.write_text(
            'cmake_minimum_required(VERSION 3.19)\ninclude("'
            + str(work / "BackendDefinitionResources.cmake")
            + '")\nlemon_backend_definition_version(value "'
            + str(source)
            + '")\n'
        )
        command = (
            "cmake -P "
            + shlex.quote(str(script))
            + " > "
            + shlex.quote(str(work / "configure.log"))
            + " 2>&1"
        )
        probe = (
            "#include <cstdlib>\n#include <cstdio>\nint main() { if (std::system("
            + json.dumps(command)
            + ') == 0) { std::puts("FAIL '
            + assertion
            + '"); return 1; } return 0; }\n'
        )
        generated_probe(args, work, probe, assertion, not control)
    results.append(
        {
            "name": "unknown-envelope-version",
            "compiled": True,
            "linked": True,
            "exit": 1,
            "assertion": assertion,
            "caught": True,
        }
    )
    print(
        "unknown-envelope-version compiled, linked and caught by " + assertion,
        flush=True,
    )
