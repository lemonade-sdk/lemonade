#include "backend_definition_binding_fixture.h"
#include "backend_definition_runner_internal.h"
#include "lemon/backends/configured/backend_definition_descriptor.h"
#include "lemon/backends/configured/launch_planner.h"

#include <fstream>

using namespace binding_fixture;

namespace {
Json container_fixture() { return Json::parse(read_bytes("gufo.container.json")); }
DefinitionResolutionResult resolve(const Json& value, DefinitionLimits limits = {}) {
    const DefinitionKey key{DefinitionNamespace::External, value.at("id"), 0};
    const auto bytes = value.dump();
    const auto snapshot = make_v2_source_snapshot({{key, bytes, ""}}, limits);
    if (!snapshot.snapshot) return {nullptr, snapshot.diagnostics};
    return DefinitionResolver(snapshot.snapshot, limits).resolve(key);
}
void negative(const std::string& name, const std::function<void(Json&)>& change, const std::string& code = "") {
    auto value = container_fixture();
    change(value["merge"]);
    const auto result = resolve(value);
    check(!result.definition && (code.empty() || has_code(result.diagnostics, code)), name);
    if (result.definition || (!code.empty() && !has_code(result.diagnostics, code))) diagnostics(result.diagnostics);
}
void structural_negative(const std::string& name, const std::function<void(Json&)>& change) {
    auto value = container_fixture();
    auto declaration = value["merge"];
    for (const auto* key : {"schema_version", "id", "recipe"}) declaration[key] = value[key];
    change(declaration);
    check(!validate_v2_structure(declaration, DefinitionV2SchemaRoot::Declaration).entries.empty(), name);
}
void compiled_and_blocked(const Json& value, const std::string& name) {
    const auto parents = builtins(false, &value);
    const auto admission = admit({}, parents);
    check(admission.catalog != nullptr, name + "CatalogAdmitsData");
    if (!admission.catalog) { diagnostics(admission.diagnostics); return; }
    const auto compiled = compile_execution_definition(admission.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, value.at("id"), 1}));
    check(compiled.definition != nullptr, name + "CompilesData");
    if (!compiled.definition) { diagnostics(compiled.diagnostics); return; }
    const auto& ir = ex::Inspection::program(*compiled.definition);
    check(ir.launch.program_source == "execution_cases" && !ir.launch.execution_cases.empty() &&
        ir.launch.execution_cases.front().program.at("source") == value["merge"]["launch"]["execution"]["cases"][0]["program"]["source"], name + "DiscriminatedProgram");
    check(ir.launch.mounts.size() == 1 && ir.launch.mounts[0].artifact == "main" && ir.launch.mounts[0].destination == "/mnt/models", name + "CompiledMountInventory");
    if (value["merge"]["install"]["strategy"] == "by_execution") {
        const auto& expected = value["merge"]["install"]["native"];
        check(ir.install.strategy == expected["strategy"] && ir.install.records == expected &&
            ir.install.cases.size() == expected.value("cases", Json::array()).size(), name + "NativeInstallProgramRetained");
        if (!ir.install.cases.empty()) check(ir.install.cases[0].filename == "/install/native/cases/0/filename", name + "NativeInstallExpressionsRetained");
    }
    const auto native = fixture(true);
    const auto native_bound = bind(native);
    const auto untrusted = ex::Inspection::test_program(*native.compiled, ir);
    const auto forged_bound = bd::Inspection::test_compiled(*native_bound.definition, untrusted);
    bool runner_rejected = false;
    try { DefinitionRunner runner(forged_bound); } catch (const std::exception& error) { runner_rejected = std::string(error.what()) == "Container execution is unavailable in C2"; }
    check(runner_rejected, name + "UnauthorizedRunnerRejectsBeforeServices");
    auto external_native = document(true);
    external_native["merge"]["artifacts"]["main"]["strategy"] = "resolved_path";
    external_native["merge"]["artifacts"]["main"].erase("hook");
    const auto external_admission = admit({external_native}, empty_builtins());
    check(external_admission.catalog != nullptr, name + "HookFreeExternalControlAdmitted");
    if (!external_admission.catalog) { diagnostics(external_admission.diagnostics); return; }
    const auto external_compiled = compile_execution_definition(external_admission.catalog->v2_definition(external_native.at("id")));
    const auto external_bound = bind_execution_definition(external_admission.catalog, external_compiled.definition);
    const auto external_ir = ex::Inspection::test_program(*external_compiled.definition, ir);
    const auto external_binding = bind_execution_definition(external_admission.catalog, external_ir);
    check(!external_binding.definition && has_code(external_binding.diagnostics, "container_execution_unavailable"), name + "HookFreeExternalBinderRejectsContainerIR");
    const auto external_forged = bd::Inspection::test_compiled(*external_bound.definition, external_ir);
    bool external_runner_rejected = false;
    try { DefinitionRunner runner(external_forged); } catch (const std::exception& error) { external_runner_rejected = std::string(error.what()) == "Container execution is unavailable in C2"; }
    check(external_runner_rejected, name + "HookFreeExternalRunnerRejectsContainerIR");
    const auto bound = bind_execution_definition(admission.catalog, compiled.definition);
    check(!bound.definition && has_code(bound.diagnostics, "container_execution_unavailable"), name + "CannotReachNativeProcess");
    const auto catalog = build_backend_catalog(parents, {});
    check(!catalog.catalog && catalog.stages.catalog_admitted && catalog.stages.compiled && !catalog.stages.bound &&
        has_code(catalog.diagnostics, "container_execution_unavailable"), name + "CatalogFinalizationFailsClosed");
}
}

int main() {
    try {
        const auto value = container_fixture();
        const auto resolved = resolve(value);
        check(resolved.definition != nullptr, "WorkedGufoDefinitionResolves");
        if (!resolved.definition) { diagnostics(resolved.diagnostics); return finish(); }
        check(v2::check_schema_keywords(v2::schema_document()).entries.empty(), "ContainerSchemaKeywordsSupported");
        const auto& declaration = resolved.definition->declaration;
        for (const auto* os : {"linux", "windows", "macos"}) {
            const auto descriptor = descriptor_from_declaration(declaration, DefinitionNamespace::External, {os});
            const auto& policy = descriptor.containers.at("rocm");
            check(policy.repository == "ghcr.io/peonist-ai/gufo" && policy.pin.source == "backend_versions" &&
                policy.pin.recipe == "gufo" && policy.pin.backend == "rocm" &&
                policy.devices == std::vector<std::string>{"/dev/dri", "/dev/kfd"} && policy.cap_add.empty() &&
                policy.policy == "lemonade-isolated-v1" && policy.network == "private_internal" &&
                policy.teardown == "stop_remove_reap" && policy.stop_grace_seconds == 10,
                std::string("EveryContainerDescriptorField:") + os);
        }
        const auto external = admit({value}, empty_builtins());
        check(!external.catalog && external.resolved && !external.authorized &&
            has_code(external.diagnostics, "container_execution_unavailable"), "HookFreeExternalContainerHasNoExecutionAuthority");
        auto oversized = declaration;
        oversized["containers"]["rocm"]["repository"] = "ghcr.io/peonist-ai/" + std::string(262144, 'a');
        check(!validate_v2_structure(oversized, DefinitionV2SchemaRoot::Declaration).entries.empty(), "OversizedContainerRepositoryRejectsBeforeRegex");
        compiled_and_blocked(value, "Container");
        auto entrypoint = value;
        entrypoint["merge"]["launch"]["execution"]["cases"][0]["program"] = {{"source", "image_entrypoint"}};
        entrypoint["merge"]["launch"]["readiness"]["timeout"] = {{"source", "global"}, {"floor_seconds", 1800}};
        entrypoint["merge"]["launch"]["mounts"][0]["when"] = true;
        entrypoint["merge"]["containers"]["rocm"]["cap_add"] = {"SYS_PTRACE"};
        compiled_and_blocked(entrypoint, "Entrypoint");
        auto mixed = value;
        mixed["merge"]["kind"] = "local";
        mixed["merge"]["install"] = {{"strategy", "by_execution"}, {"native", {{"strategy", "none"}}}};
        mixed["merge"]["launch"]["execution"]["cases"].push_back({{"id", "cpu-native"}, {"kind", "native"},
            {"backends", {"cpu"}}, {"program", {{"source", "managed"}, {"install_policy", "always"}}}});
        compiled_and_blocked(mixed, "Mixed");
        mixed["merge"]["install"]["native"] = {{"strategy", "github_release"}, {"cases", Json::array({
            {{"id", "cpu-release"}, {"backend", {"cpu"}}, {"repo", "example/native"}, {"filename", {{"concat", {"engine-", {{"ref", "/install/version"}}}}}}}
        })}};
        compiled_and_blocked(mixed, "MixedRelease");
        structural_negative("ExecutionRecordBoundRejected", [](auto& d) { auto c = d["launch"]["execution"]["cases"][0]; d["launch"]["execution"]["cases"] = Json::array(); for (int i = 0; i < 33; ++i) d["launch"]["execution"]["cases"].push_back(c); });
        structural_negative("MountRecordBoundRejected", [](auto& d) { auto c = d["launch"]["mounts"][0]; d["launch"]["mounts"] = Json::array(); for (int i = 0; i < 65; ++i) d["launch"]["mounts"].push_back(c); });
        structural_negative("BackendSelectorBoundRejected", [](auto& d) { auto& backends = d["launch"]["execution"]["cases"][0]["backends"]; backends = Json::array(); for (int i = 0; i < 33; ++i) backends.push_back("backend" + std::to_string(i)); });
        structural_negative("ExecutionStableIdRequired", [](auto& d) { d["launch"]["execution"]["cases"][0].erase("id"); });
        structural_negative("MountStableIdRequired", [](auto& d) { d["launch"]["mounts"][0].erase("id"); });
        structural_negative("RepositoryLengthBoundRejected", [](auto& d) { d["containers"]["rocm"]["repository"] = "ghcr.io/peonist-ai/" + std::string(300, 'a'); });
        auto duplicate_native = mixed;
        duplicate_native["merge"]["install"]["native"]["cases"].push_back(duplicate_native["merge"]["install"]["native"]["cases"][0]);
        const auto duplicate_native_result = resolve(duplicate_native);
        check(!duplicate_native_result.definition && has_code(duplicate_native_result.diagnostics, "duplicate_identity"), "NativeInstallDuplicateIdentityRejected");
        mixed["merge"]["install"]["native"] = {{"strategy", "hook"}, {"hook", {{"id", "foreign"}, {"params", Json::object()}}}};
        check(!resolve(mixed).definition, "NativeInstallHookNotAdmitted");
        negative("TooManyPoliciesRejected", [](auto& m) { auto p = m["containers"]["rocm"]; for (int i = 0; i < 32; ++i) m["containers"]["extra" + std::to_string(i)] = p; }, "invalid_container_policy");
        negative("UnusedPolicyRejected", [](auto& m) { m["containers"]["cpu"] = m["containers"]["rocm"]; }, "unused_container_policy");
        negative("MissingContainerReferenceRejected", [](auto& m) { m["launch"]["execution"]["cases"][0]["container"] = "missing"; }, "invalid_reference");
        negative("CaseBackendPolicyMismatchRejected", [](auto& m) { m["launch"]["execution"]["cases"][0]["backends"] = {"cpu"}; m["metadata"]["support"][0]["backend"] = "cpu"; }, "invalid_reference");
        negative("NativeWorkingDirectoryRejected", [](auto& m) { m["launch"]["working_directory"] = "executable_directory"; }, "invalid_container_policy");
        negative("ContainerReadinessPathRejected", [](auto& m) { m["launch"]["readiness"]["path"] = "ready"; m["launch"]["watchdog"]["path"] = "ready"; }, "invalid_container_readiness");
        negative("MutableRepositoryTagRejected", [](auto& m) { m["containers"]["rocm"]["repository"] = "ghcr.io/peonist-ai/gufo:latest"; });
        negative("EmbeddedDigestRejected", [](auto& m) { m["containers"]["rocm"]["repository"] = "ghcr.io/peonist-ai/gufo@sha256:" + std::string(64, 'a'); });
        negative("UntrustedNamespaceRejected", [](auto& m) { m["containers"]["rocm"]["repository"] = "ghcr.io/foreign/gufo"; });
        negative("NamespacePrefixSpoofRejected", [](auto& m) { m["containers"]["rocm"]["repository"] = "ghcr.io/peonist-ai.evil/gufo"; });
        negative("MutablePinRejected", [](auto& m) { m["containers"]["rocm"]["pin"] = "latest"; });
        negative("InlinePinRejected", [](auto& m) { m["containers"]["rocm"]["pin"]["digest"] = "sha256:" + std::string(64, 'a'); });
        negative("ForeignPinSourceRejected", [](auto& m) { m["containers"]["rocm"]["pin"]["source"] = "environment"; });
        negative("ManagedContainerProgramRejected", [](auto& m) { m["launch"]["execution"]["cases"][0]["program"] = {{"source", "managed"}, {"install_policy", "always"}}; });
        auto native_container = declaration;
        auto& native_case = native_container["launch"]["execution"]["cases"][0];
        native_case["kind"] = "native"; native_case.erase("container");
        native_case["program"] = {{"source", "managed"}, {"install_policy", "always"}};
        check(!validate_v2_structure(native_container, DefinitionV2SchemaRoot::Declaration).entries.empty(), "ContainerNativeCaseSchemaRejected");
        negative("ContainerNativeCaseRejected", [](auto& m) { auto& c = m["launch"]["execution"]["cases"][0]; c["kind"] = "native"; c.erase("container"); c["program"] = {{"source", "managed"}, {"install_policy", "always"}}; });
        negative("TopLevelManagedProgramRejected", [](auto& m) { m["launch"]["program"] = {{"source", "managed"}, {"install_policy", "always"}}; });
        negative("UnknownDeviceRejected", [](auto& m) { m["containers"]["rocm"]["devices"] = {"/dev/mem"}; });
        negative("UnknownCapabilityRejected", [](auto& m) { m["containers"]["rocm"]["cap_add"] = {"SYS_ADMIN"}; });
        negative("DuplicateDeviceRejected", [](auto& m) { m["containers"]["rocm"]["devices"] = {"/dev/dri", "/dev/dri"}; });
        negative("ForeignArtifactMountRejected", [](auto& m) { m["launch"]["mounts"][0]["artifact"] = "outside"; }, "invalid_mount_artifact");
        negative("ArbitraryHostMountRejected", [](auto& m) { m["launch"]["mounts"][0]["source"] = "/etc"; });
        negative("ForeignDestinationRejected", [](auto& m) { m["launch"]["mounts"][0]["destination"] = "/etc"; });
        negative("UnknownExpansionRejected", [](auto& m) { m["launch"]["mounts"][0]["expand"] = "recursive"; });
        negative("WritableMountRejected", [](auto& m) { m["launch"]["mounts"][0]["mode"] = "rw"; });
        negative("HostNetworkRejected", [](auto& m) { m["containers"]["rocm"]["network"] = "host"; });
        negative("SubstitutePolicyRejected", [](auto& m) { m["containers"]["rocm"]["policy"] = "unconfined"; });
        negative("UnknownTeardownRejected", [](auto& m) { m["containers"]["rocm"]["teardown"] = "leave_running"; });
        negative("ChangedStopGraceRejected", [](auto& m) { m["containers"]["rocm"]["stop_grace_seconds"] = 0; });
        negative("HostAddressRejected", [](auto& m) { m["launch"]["address"]["source"] = "loopback"; });
        negative("ContainerCommandPathRejected", [](auto& m) { m["launch"]["execution"]["cases"][0]["program"]["name"] = "/usr/bin/gufo"; });
        negative("EntrypointNameRejected", [](auto& m) { m["launch"]["execution"]["cases"][0]["program"] = {{"source", "image_entrypoint"}, {"name", "gufo"}}; });
        negative("ContainerDefaultVersionsRejected", [](auto& m) { m["install"]["default_versions"] = {{"rocm", "latest"}}; });
        negative("ContainerManagedInstallRejected", [](auto& m) { m["install"] = {{"strategy", "none"}}; });
        negative("ExecutionOverlapRejected", [](auto& m) { auto c = m["launch"]["execution"]["cases"][0]; c["id"] = "other"; m["launch"]["execution"]["cases"].push_back(c); }, "execution_selector_overlap");
        negative("DuplicateExecutionIdentityRejected", [](auto& m) { m["launch"]["execution"]["cases"].push_back(m["launch"]["execution"]["cases"][0]); }, "duplicate_identity");
        negative("DuplicateMountIdentityRejected", [](auto& m) { m["launch"]["mounts"].push_back(m["launch"]["mounts"][0]); }, "duplicate_identity");
        negative("ContainerSupportNeedsExplicitCase", [](auto& m) {
            m["kind"] = "local"; m.erase("containers"); m["install"] = {{"strategy", "none"}};
            m["launch"].erase("execution"); m["launch"].erase("mounts"); m["launch"].erase("address");
            m["launch"]["program"] = {{"source", "managed"}, {"install_policy", "always"}};
        }, "invalid_execution_support");
        negative("MissingPolicyRejected", [](auto& m) { m.erase("containers"); });
        negative("MissingAddressRejected", [](auto& m) { m["launch"].erase("address"); });
        negative("MissingMountPlanRejected", [](auto& m) { m["launch"].erase("mounts"); });
        negative("ContainerSupportMismatchRejected", [](auto& m) { m["metadata"]["support"][0]["format"] = "native"; }, "invalid_execution_support");
        negative("NativeEnvironmentPresetRejected", [](auto& m) { m["launch"]["environment"]["preset"] = "executable_loader_path"; });
        negative("UnboundedReadinessRejected", [](auto& m) { m["launch"]["readiness"]["timeout"] = 100000; }, "invalid_container_readiness");
        negative("UnboundedReadinessFloorRejected", [](auto& m) { m["launch"]["readiness"]["timeout"] = {{"source", "global"}, {"floor_seconds", 0}}; });
        auto edited = value;
        edited["edits"] = Json::array({{{"op", "replace"}, {"path", "/launch/execution/cases"}, {"match", {{"id", "rocm-container"}}},
            {"at", "/program"}, {"value", {{"source", "image_entrypoint"}}}},
            {{"op", "replace"}, {"path", "/launch/mounts"}, {"match", {{"id", "main"}}}, {"at", "/expand"}, {"value", "file"}}});
        const auto edit_result = resolve(edited);
        check(edit_result.definition && edit_result.definition->declaration["launch"]["mounts"][0]["expand"] == "file", "ContainerRecordsSupportStableEdits");
        DefinitionLimits limits; limits.work_nodes = 1;
        check(!resolve(value, limits).definition, "ContainerRecordsHonorWorkBounds");
        BackendDefinition legacy; legacy.records = declaration;
        bool blocked = false;
        try { LaunchPlanner{}.plan(legacy, {}); } catch (const std::exception& error) { blocked = std::string(error.what()) == "Container execution is unavailable in C2"; }
        check(blocked, "LegacyPlannerCannotReachNativeProcess");
    } catch (const std::exception& error) {
        std::printf("Container fixture rejected: %s\n", error.what());
        check(false, "ContainerDefinitionPipeline");
    }
    return finish();
}
