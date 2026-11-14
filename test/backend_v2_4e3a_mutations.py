"""One-property HRX and preparation mutants, compiled without editing production files."""

import argparse
import itertools
import json
from pathlib import Path
import shlex
import shutil
import subprocess
from types import SimpleNamespace
from unittest import mock

import backend_definition_data_parity as parity
import backend_definition_inventory as inventory
import backend_v2_4e3a_audit as audit
from llamacpp_frozen_control import REPO, ROOT, verify, verify_build_inputs
from llamacpp_oracle_audit import link_command

CONFIGURED = "src/cpp/server/backends/configured/"


def execute(command, cwd, log, expected=0):
    process = subprocess.run(
        command, cwd=cwd, capture_output=True, text=True, timeout=180
    )
    output = process.stdout + process.stderr
    log.write_text(output)
    assert process.returncode == expected, (command, process.returncode, output[-3000:])
    return output


def binary(
    args,
    name,
    source,
    old,
    new,
    assertion,
    header=None,
    prefix="",
    target="test_hrx_oracle",
    expected=1,
):
    work = args.scratch / name
    work.mkdir(parents=True, exist_ok=True)
    original = REPO / source
    text = original.read_text()
    assert old in text, (name, old)
    replacement = work / original.name
    replacement.write_text(prefix + text.replace(old, new, 1))
    row = next(
        r
        for r in args.database
        if Path(r["file"]) == original
        and (
            "lemonade-server-core.dir" in r["command"]
            or target + ".dir" in r["command"]
        )
    )
    command = shlex.split(row["command"])
    old_object = command[command.index("-o") + 1]
    obj = work / "mutant.o"
    command[command.index("-o") + 1] = str(obj)
    command[command.index("-c") + 1] = str(replacement)
    command.extend(["-iquote", str(original.parent)])
    command = ["-O0" if token.startswith("-O") else token for token in command]
    if header:
        path, before, after = header
        data = (REPO / path).read_text()
        assert before in data
        destination = (
            work
            / "include"
            / (
                Path(path).name
                if Path(path).name == "backend_definition_resource_files.h"
                else path.removeprefix("src/cpp/include/")
            )
        )
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(data.replace(before, after, 1))
        command.insert(1, "-I" + str(work / "include"))
    execute(command, args.build, work / "compile.log")
    link = link_command(args.build, target)
    assert old_object in link, (name, old_object)
    link[link.index(old_object)] = str(obj)
    probe = work / "probe"
    link[link.index("-o") + 1] = str(probe)
    execute(link, args.build, work / "link.log")
    output = execute(
        ["timeout", "120", str(probe)], args.build, work / "run.log", expected
    )
    assert expected == 0 or (assertion in output), (
        name,
        output[-3000:],
    )
    result = {
        "rule": name,
        "compiled": True,
        "linked": True,
        "exit": expected,
        "assertion": assertion,
    }
    print(result, flush=True)
    return result


def compiled_mutations(args):
    registry = CONFIGURED + "backend_hook_registry.cpp"
    needle = "ArtifactHook(&llamacpp::legacy_llamacpp_artifact_ops)"
    poison = """#include "backend_typed_hook_registry_internal.h"
namespace lemon::backends {
class PoisonedV2ArtifactOps final : public BackendOps {
public:
    std::string validate_registration_checkpoint(const std::string& checkpoint) const override {
        return typed::llamacpp::validate_registration(checkpoint);
    }
};
const BackendOps* poisoned_v2_artifact_ops() { static const PoisonedV2ArtifactOps value; return &value; }
}
"""
    yield binary(
        args,
        "hrx-reaches-v2-artifact-target",
        registry,
        needle,
        "ArtifactHook(&poisoned_v2_artifact_ops)",
        "HrxGainedNothing:unexpected",
        prefix='#include "lemon/backends/backend_ops.h"\n' + poison,
    )
    header = "src/cpp/include/lemon/backends/configured/configured_backend_ops.h"
    for name, code in (
        (
            "dflash-default",
            'for (const auto& label : info.labels) if (label == "dflash") options.set_option("llamacpp_args", "--spec-type draft-dflash");',
        ),
        (
            "mtp-default",
            'for (const auto& label : info.labels) if (label == "mtp") options.set_option("llamacpp_args", "--spec-type draft-mtp");',
        ),
        (
            "vanilla-parallel-default",
            'options.set_option("llamacpp_args", "--parallel 1");',
        ),
    ):
        method = f"    void resolve_runtime_options(const ModelInfo& info, RecipeOptions& options) const override {{ {code} }}\n"
        yield binary(
            args,
            name,
            CONFIGURED + "configured_backend_ops.cpp",
            "#include <algorithm>",
            "#include <algorithm>",
            "HrxGainedNothing:no vanilla",
            header=(header, "public:\n", "public:\n" + method),
        )
    probe = """    std::string resolve_version(const std::string&, const std::string&) const override {
        struct Probe final : typed::SystemProbeServices {
            std::string llama_version() override { typed::observe_entry("leaked.system.probe"); return "probe-version"; }
            bool kfd_present() override { return false; }
            bool hip_plugin_present() override { return false; }
        } probe;
        return probe.llama_version();
    }
"""
    yield binary(
        args,
        "system-probe-leak",
        CONFIGURED + "configured_backend_ops.cpp",
        "#include <algorithm>",
        "#include <algorithm>",
        "HrxGainedNothing:unexpected",
        header=(header, "public:\n", "public:\n" + probe),
        prefix='#define LEMONADE_TYPED_HOOK_TEST\n#include "backend_typed_hook_registry_internal.h"\n',
    )
    line = '    if (metadata.value("uses_ctx_size", false)) values["options"]["ctx_size"] = context.options.get_option("ctx_size").get<int>();'
    yield binary(
        args,
        "embedding-context-floor",
        CONFIGURED + "launch_planner.cpp",
        line,
        line
        + '\n    if (context.model_info.type == ModelType::EMBEDDING) values["options"]["ctx_size"] = 8192;',
        "HrxGainedNothing:no embedding context floor",
    )
    line = "    LoadContext context{name, model, options, log_level_, {}};"
    yield binary(
        args,
        "preassigned-device-overwrite",
        CONFIGURED + "configured_server.cpp",
        line,
        line + "\n    device_type_ = DEVICE_GPU;",
        "HrxGainedNothing:preassigned device unchanged",
    )
    yield binary(
        args,
        "hf-loading-bypasses-main",
        CONFIGURED + "launch_planner.cpp",
        "if (!valid) throw std::runtime_error",
        'if (!valid && !(context.model_info.extras.count("hf_load") && context.model_info.extras.at("hf_load") == true)) throw std::runtime_error',
        "HrxGainedNothing:HF options cannot bypass",
    )
    artifact = "src/cpp/server/backends/llamacpp/legacy_llamacpp_artifact_ops.cpp"
    for name, old, new, assertion in (
        (
            "metadata",
            "info.max_context_window = meta.context_length;",
            "info.max_context_window = meta.context_length + 1;",
            "HrxGainedNothing:exact frozen artifact result:",
        ),
        (
            "resolution",
            "return resolve_gguf_path(ctx.model_cache_path, ctx.variant);",
            'return "wrong-main";',
            "HrxGainedNothing:exact frozen artifact result:",
        ),
        (
            "import",
            'return resolve_gguf_path(import_dir, "");',
            'return "wrong-import";',
            "HrxGainedNothing:legacy imported checkpoint result",
        ),
        (
            "registration",
            '"You are required to provide',
            '"Wrong: You are required to provide',
            "HrxGainedNothing:exact frozen artifact result:",
        ),
        (
            "file",
            '"Invalid GGUF cache file"',
            '"Wrong GGUF cache file"',
            "HrxGainedNothing:exact frozen artifact result:",
        ),
    ):
        yield binary(args, "legacy-artifact-" + name, artifact, old, new, assertion)


def comparison_mutations(args):
    source = "test/cpp/test_backend_definition_v1_compat.cpp"
    path = str(
        (args.build / "test/include/backend_definition_resource_files.h").relative_to(
            REPO
        )
    )
    header = (REPO / path).read_text()
    candidate = (REPO / "test/fixtures/backend-v2/llamacpp.candidate.json").read_text()

    def mixed(data):
        literals = "\n".join(
            json.dumps(data[i : i + 2000], ensure_ascii=False)
            for i in range(0, len(data), 2000)
        )
        added = "inline constexpr char k_control_candidate[] =\n" + literals + ";\n"
        value = header.replace(
            "inline constexpr EmbeddedResourceFile",
            added + "inline constexpr EmbeddedResourceFile",
        )
        return value.replace(
            "};\n\n}",
            '    {"/resources/backends/llamacpp.json", k_control_candidate, sizeof(k_control_candidate) - 1},\n};\n\n}',
        )

    full_source = (REPO / source).read_text()
    yield binary(
        args,
        "compiled-eight-v1-plus-qualified-v2-control",
        source,
        full_source,
        full_source,
        "",
        header=(path, header, mixed(candidate)),
        target="test_backend_definition_v1_compat",
        expected=0,
    )
    old = audit.git_bytes(source).decode()
    yield binary(
        args,
        "compiled-gate-parses-every-source-as-v1",
        source,
        full_source,
        old,
        "Invalid baseline definition",
        header=(path, header, mixed(candidate)),
        target="test_backend_definition_v1_compat",
    )
    yield binary(
        args,
        "compiled-v2-candidate-drift",
        source,
        full_source,
        full_source,
        "SoleAdditionalSourceIsQualifiedLlamaCandidate",
        header=(path, header, mixed(candidate + "\n")),
        target="test_backend_definition_v1_compat",
    )
    hrx = next(line for line in header.splitlines(True) if "hrx.json)" in line)
    yield binary(
        args,
        "compiled-missing-hrx",
        source,
        full_source,
        full_source,
        "ExplicitEightV1Definitions:missing source",
        header=(path, header, header.replace(hrx, "")),
        target="test_backend_definition_v1_compat",
    )
    yield binary(
        args,
        "compiled-foreign-source",
        source,
        full_source,
        full_source,
        "ExplicitEightV1Definitions:foreign source",
        header=(path, header, header.replace("hrx.json)", "foreign.json)")),
        target="test_backend_definition_v1_compat",
    )
    yield binary(
        args,
        "hrx-capability-widened",
        "test/cpp/test_hrx_oracle.cpp",
        "using namespace llm_oracle;",
        "using namespace llm_oracle;",
        "HRX advertised capability mask stays chat-only",
        header=(
            "src/cpp/include/lemon/backends/hrx/hrx_server.h",
            "return 0;",
            "return 1;",
        ),
    )
    yield binary(
        args,
        "hrx-callable-embedding-removed",
        CONFIGURED + "configured_server.cpp",
        "adapters.embeddings = this;",
        "adapters.embeddings = nullptr;",
        "HrxGainedNothing:unexpected",
    )


def rejected(name, action, assertion):
    try:
        action()
    except AssertionError as error:
        assert assertion in str(error), (name, str(error), assertion)
        print(name, "caught", assertion, flush=True)
        return {"rule": name, "assertion": assertion, "caught": True}
    raise AssertionError(f"UncaughtPreparationMutation:{name}")


def preparation_mutations(args):
    from backend_v2_4e3a_audit import MANIFEST

    manifest = json.loads(MANIFEST.read_text())
    snapshot = args.baseline.read_bytes()
    audit.baseline(manifest, snapshot)
    for key, value, assertion in (
        ("unknown", True, "ExactTransitionManifestFields"),
        ("stage", "active", "InertTransitionStage"),
        (
            "changes",
            [{"op": "remove", "path": "/schema"}],
            "InertCatalogTransitionHasNoChanges",
        ),
        ("base_commit", "wrong", "TransitionBaseCommit"),
        ("base_tree", "wrong", "TransitionBaseTree"),
        ("snapshot_sha256", "wrong", "PinnedWholeBaseSnapshot"),
        ("snapshot_bytes", 0, "PinnedWholeBaseSnapshot"),
        ("candidate_sha256", "wrong", "PinnedQualifiedCandidate"),
        ("v1_sources", {}, "PinnedEightV1Sources"),
        ("recipes", manifest["recipes"][::-1], "BaselineRecipeOrder"),
    ):
        mutant = dict(manifest, **{key: value})
        yield rejected(
            "transition-" + key, lambda: audit.baseline(mutant, snapshot), assertion
        )
    work = args.scratch / "control-contamination"
    work.mkdir(exist_ok=True)
    shutil.copy(ROOT / "build/CMakeCache.txt", work / "CMakeCache.txt")
    database = json.loads((ROOT / "build/compile_commands.json").read_text())
    database[0]["command"] += " -I" + str(REPO / "build/include")
    (work / "compile_commands.json").write_text(json.dumps(database))
    yield rejected(
        "control-reuses-production-registrations",
        lambda: verify_build_inputs(ROOT / "source", work),
        "FrozenControlOwnRegistrations",
    )
    database[0]["command"] = database[0]["command"].replace(
        str(REPO / "build/include"), str(REPO / "src/cpp/include")
    )
    (work / "compile_commands.json").write_text(json.dumps(database))
    yield rejected(
        "control-reuses-production-sources",
        lambda: verify_build_inputs(ROOT / "source", work),
        "FrozenControlNoProductionInputs",
    )
    real = Path.read_bytes
    path = ROOT / "source/src/cpp/include/lemon/backends/llamacpp/llamacpp.h"
    with mock.patch.object(
        Path,
        "read_bytes",
        lambda self: b"wrong descriptor" if self == path else real(self),
    ):
        yield rejected(
            "frozen-descriptor-drift",
            lambda: verify(ROOT / "source"),
            "FrozenControlSource:",
        )
    generated = ROOT / "build/include/backend_factories_generated.h"
    with mock.patch.object(
        Path,
        "read_bytes",
        lambda self: (
            b"production v2 registrations" if self == generated else real(self)
        ),
    ):
        yield rejected(
            "control-copies-production-registrations",
            lambda: verify(ROOT / "source", ROOT / "build"),
            "FrozenControlGeneratedBytes:backend_factories_generated.h",
        )
    version = ROOT / "source/.version"
    before_text = Path.read_text
    with mock.patch.object(
        Path,
        "read_text",
        lambda self, *a, **k: (
            "parent-head" if self == version else before_text(self, *a, **k)
        ),
    ):
        yield rejected(
            "control-inherits-parent-version",
            lambda: verify(ROOT / "source"),
            "FrozenControlVersionPinned",
        )
    generated_version = ROOT / "build/include/lemon/version.h"
    with mock.patch.object(
        Path,
        "read_bytes",
        lambda self: b"parent-head" if self == generated_version else real(self),
    ):
        yield rejected(
            "control-generated-version-drift",
            lambda: verify(ROOT / "source", ROOT / "build"),
            "FrozenControlGeneratedBytes:lemon/version.h",
        )
    protected = REPO / "src/cpp/resources/backends/hrx.json"
    with mock.patch.object(
        Path,
        "read_bytes",
        lambda self: b"wrong hrx" if self == protected else real(self),
    ):
        yield rejected(
            "protected-hrx-bytes-changed",
            lambda: audit.source_preservation(args.scratch),
            "ProtectedSource:src/cpp/resources/backends/hrx.json",
        )
    symbols = subprocess.check_output(
        [
            "nm",
            "-C",
            str(
                args.build
                / "CMakeFiles/lemonade-server-core.dir/src/cpp/server/backends/llamacpp/legacy_llamacpp_artifact_ops.cpp.o"
            ),
        ],
        text=True,
    )
    with mock.patch.object(
        subprocess,
        "check_output",
        return_value=symbols + "\nU lemon::backends::DefinitionRunner\n",
    ):
        yield rejected(
            "legacy-adapter-reaches-runner",
            lambda: audit.purity(args.build, args.scratch),
            "LegacyArtifactTargetCannotReachV2RuntimeOps",
        )
    for method in ("resolve_runtime_options", "resolve_version", "check_install"):
        with mock.patch.object(
            subprocess,
            "check_output",
            return_value=symbols
            + "\nT lemon::backends::llamacpp::LegacyLlamaCppArtifactOps::"
            + method
            + "\n",
        ):
            yield rejected(
                "legacy-adapter-forbidden-override-" + method,
                lambda: audit.purity(args.build, args.scratch),
                "LegacyArtifactTargetCannotReachV2RuntimeOps",
            )
    work = args.scratch / "inventory"
    sources = work / "src/cpp/resources/backends"
    sources.mkdir(parents=True, exist_ok=True)
    for name in inventory.V1_NAMES:
        shutil.copy(REPO / f"src/cpp/resources/backends/{name}.json", sources)
    assert inventory.production_inventory(work) == inventory.V1_NAMES
    (sources / "foreign.json").write_text("{}")
    yield rejected(
        "inventory-foreign-source",
        lambda: inventory.production_inventory(work),
        "ExactMixedDefinitionSources",
    )
    (sources / "foreign.json").unlink()
    hrx = (sources / "hrx.json").read_bytes()
    (sources / "hrx.json").unlink()
    yield rejected(
        "inventory-missing-hrx",
        lambda: inventory.production_inventory(work),
        "ExactMixedDefinitionSources",
    )
    (sources / "hrx.json").write_bytes(hrx)
    candidate = work / "test/fixtures/backend-v2/llamacpp.candidate.json"
    candidate.parent.mkdir(parents=True, exist_ok=True)
    candidate.write_bytes(
        (REPO / "test/fixtures/backend-v2/llamacpp.candidate.json").read_bytes()
    )
    (sources / "llamacpp.json").write_bytes(candidate.read_bytes())
    assert inventory.production_inventory(work) == inventory.V1_NAMES | {"llamacpp"}
    (sources / "llamacpp.json").write_bytes(candidate.read_bytes() + b"\n")
    yield rejected(
        "inventory-v2-candidate-drift",
        lambda: inventory.production_inventory(work),
        "SoleAdditionalSourceIsQualifiedLlamaCandidate",
    )
    (sources / "hrx.json").write_text(
        hrx.decode().replace('"schema_version": 1', '"schema_version": 2')
    )
    yield rejected(
        "inventory-v1-version-drift",
        lambda: inventory.production_inventory(work),
        "ExplicitEightV1Definitions",
    )
    before_read, before_exists = Path.read_text, Path.exists

    def trap(path, action):
        if str(path).startswith(str(REPO / "src/cpp/resources/backends")):
            raise AssertionError("BaselineMustNotReadCurrentInputs")
        return action(path)

    args_parity = SimpleNamespace(
        out=args.scratch / "baseline-from-git", baseline_commit=audit.BASE
    )
    args_parity.out.mkdir(exist_ok=True)
    with mock.patch.object(
        Path, "read_text", lambda p, *a, **k: trap(p, before_read)
    ), mock.patch.object(Path, "exists", lambda p: trap(p, before_exists)):
        parity.baseline_generated_header(args_parity)
        namespace = dict(parity.__dict__)
        code = (
            (REPO / "test/backend_definition_data_parity.py")
            .read_text()
            .replace(
                "source = tree / relative\n        if relative in files:",
                "source = REPO / relative\n        if source.exists():",
            )
        )
        assert code != (REPO / "test/backend_definition_data_parity.py").read_text()
        exec(compile(code, "baseline-current-input-mutant.py", "exec"), namespace)
        yield rejected(
            "baseline-current-tree-contamination",
            lambda: namespace["baseline_generated_header"](args_parity),
            "BaselineMustNotReadCurrentInputs",
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument(
        "--group", choices=["all", "preparation", "hrx", "comparisons"], default="all"
    )
    args = parser.parse_args()
    args.build, args.scratch, args.out = (
        args.build.resolve(),
        args.scratch.resolve(),
        args.out.resolve(),
    )
    args.scratch.mkdir(parents=True, exist_ok=True)
    args.out.mkdir(parents=True, exist_ok=True)
    args.database = json.loads((args.build / "compile_commands.json").read_text())
    results = []
    groups = {
        "preparation": preparation_mutations,
        "hrx": compiled_mutations,
        "comparisons": comparison_mutations,
    }
    selected = groups.values() if args.group == "all" else [groups[args.group]]
    for result in itertools.chain.from_iterable(group(args) for group in selected):
        results.append(result)
        (args.out / "mutations.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"{len(results)} single-rule checks passed (including positive controls)")


if __name__ == "__main__":
    main()
