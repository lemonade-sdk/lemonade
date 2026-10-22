#include "backend_runtime_fixture.h"
#include "lemon/backends/configured/backend_spec_utils.h"
#include "lemon/utils/path_utils.h"
#include "llamacpp_v2_load_artifact_parity.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <tuple>

namespace lemon::backends::runtime {
struct TestAccess {
    static void reuse(InvocationGuard& guard, Frame& frame, const ExecutableDefinition& executable,
        std::size_t index, const execution::SymbolicHook& hook) {
        guard.~InvocationGuard();
        new (&guard) InvocationGuard(frame, executable, index, hook, hook.slot.owner_kind, hook.slot.owner_id, hook.slot.stage, {});
    }
    static std::unique_ptr<EnvironmentServices> environment(InvocationGuard& guard, EnvironmentSources sources, const std::string& root) {
        return std::unique_ptr<EnvironmentServices>(new EnvironmentServices(guard, std::move(sources), root, typed::StagingScope::ManagedTheRock));
    }
    static typed::StagingCapability capability(EnvironmentServices& service) { return *service.capability(); }
    static StagingPlan seal(EnvironmentServices& service, typed::EnvironmentPlan plan) { return service.seal(std::move(plan)); }
    static void expire(StagingPlan& plan) { plan.lifetime_.reset(); }
    static std::unique_ptr<InvocationGuard> guard(Frame& frame, const ExecutableDefinition& executable,
        std::size_t index, const execution::SymbolicHook& hook, SlotOwnerKind kind, const std::string& owner,
        const std::string& stage, utils::RequestCancelToken cancel = {}) {
        return std::unique_ptr<InvocationGuard>(new InvocationGuard(frame, executable, index, hook, kind, owner, stage, std::move(cancel)));
    }
};
}

using namespace runtime_fixture;

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--capture-helper") {
        if (std::string(argv[2]) == "wait") std::this_thread::sleep_for(std::chrono::milliseconds(300));
        else { std::cout << std::string(10000, 'a'); std::cerr << std::string(10000, 'b'); }
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--version") {
        std::cout << (std::filesystem::path(argv[0]).filename() == "other-engine" ? "version: 9999" : "version: 3432");
        return 0;
    }
    try {
    const auto executable = runtime_fixture::executable();
    const auto& program = ex::Inspection::program(executable->compiled());
    const auto at = std::find_if(program.hooks.begin(), program.hooks.end(), [](const auto& hook) { return hook.contract == typed::Contract::Backend; });
    const auto index = static_cast<std::size_t>(at - program.hooks.begin());
    rt::Frame frame(*executable);
    frame.facts.insert(typed::Fact::BackendOptions);
    auto guard = rt::TestAccess::guard(frame, *executable, index, *at, SlotOwnerKind::Root, "backend", "backend");
    rt::SelectionServices service(*guard, {"cpu"});
    check(service.supports("cpu") && !service.supports("cuda"), "SelectionUsesCapturedLeafSupport");
    check(throws([&] { service.validate_device({"cuda", "CUDA0"}); }, "scope"), "SelectionCannotEscapeBackendScope");
    check(throws([&] { guard->check(typed::Effect::ArtifactRead); }, "Effect outside"), "ServiceCannotEscapeEffectSet");
    frame.active_slot.reset();
    check(throws([&] { service.supports("cpu"); }, "Inactive"), "ServiceOutsideBoundSlotRejects");
    frame.active_slot = index;
    const auto identity = frame.executable;
    frame.executable = nullptr;
    check(throws([&] { service.supports("cpu"); }, "Inactive"), "ServiceOnForeignExecutableRejects");
    frame.executable = identity;
    check(throws([&] { rt::TestAccess::guard(frame, *executable, index, *at, SlotOwnerKind::Root, "backend", "backend"); }, "Nested"), "NestedSlotRejects");
    auto other_thread = std::async(std::launch::async, [&] { return throws([&] { service.supports("cpu"); }, "Inactive"); });
    check(other_thread.get(), "ServiceOnForeignThreadRejects");
    check(throws([&] { guard->check(typed::Effect::SupportQuery, 1024 * 1024 + 1); }, "allowance"), "ServiceAllowanceIsBounded");
    guard.reset();
    check(!frame.active_slot, "GuardExpiresOnReturn");
    rt::Frame other(*executable);
    check(other.nonce != frame.nonce, "InvocationNoncesAreUnique");
    std::atomic<bool> cancelled{false};
    guard = rt::TestAccess::guard(frame, *executable, index, *at, SlotOwnerKind::Root, "backend", "backend", {&cancelled, {}});
    rt::SelectionServices cancelled_service(*guard, {"cpu"});
    cancelled = true;
    check(throws([&] { cancelled_service.supports("cpu"); }, "cancelled"), "EveryEffectChecksCancellation");
    guard.reset();
    for (const auto& item : {std::pair<SlotOwnerKind, std::string>{SlotOwnerKind::Operation, "backend"}, {SlotOwnerKind::Root, "system"}})
        check(throws([&] { rt::TestAccess::guard(frame, *executable, index, *at, item.first, item.second, "backend"); }, "owner/stage"), "WrongOwnerRejects");
    check(throws([&] { rt::TestAccess::guard(frame, *executable, index, *at, SlotOwnerKind::Root, "backend", "request"); }, "owner/stage"), "WrongStageRejects");
    frame.facts.clear();
    check(throws([&] { rt::TestAccess::guard(frame, *executable, index, *at, SlotOwnerKind::Root, "backend", "backend"); }, "Missing"), "MissingProducerIsNotLazilyInvoked");
    const auto prepare = std::find_if(program.hooks.begin(), program.hooks.end(), [](const auto& hook) { return hook.contract == typed::Contract::Prepare; });
    for (const auto& input : prepare->inputs) if (!input.producer) frame.facts.insert(input.fact);
    check(throws([&] { rt::TestAccess::guard(frame, *executable, static_cast<std::size_t>(prepare - program.hooks.begin()), *prepare,
        SlotOwnerKind::Root, "launch", "launch"); }, "Missing"), "MissingProducedFactDoesNotInvokeBackendLazily");
    const auto entry = ConfiguredBackendEntry::make(executable);
    auto info = model();
    RecipeOptions launch_only;
    entry->backend_ops()->resolve_runtime_options(info, launch_only);
    check(launch_only.get_option("fork_args") == "", "DefaultsApplyAtDeclaredStage");
    for (const auto& value : {Json(""), Json(nullptr), Json("--user 2")}) {
        RecipeOptions options; options.set_option("fork_args", value); options.set_option("merge_args", false);
        entry->backend_ops()->resolve_runtime_options(info, options);
        check(options.get_explicit_option("fork_args") == value, "RuntimeOptionsPreserveExplicitEmptyNullAndMergeArgs");
    }
    auto document = runtime_document();
    document["merge"]["launch"]["custom_args"]["defaults"][0]["stage"] = "runtime_options";
    const auto defaults = ConfiguredBackendEntry::make(runtime_fixture::executable(document));
    RecipeOptions omitted;
    defaults->backend_ops()->resolve_runtime_options(info, omitted);
    check(omitted.get_option("fork_args") == "--parallel 1", "DefaultsApplyAtDeclaredStage");
    for (const auto& value : {Json(""), Json(nullptr)}) {
        RecipeOptions explicit_value; explicit_value.set_option("fork_args", value);
        defaults->backend_ops()->resolve_runtime_options(info, explicit_value);
        check(explicit_value.get_explicit_option("fork_args") == value, "RuntimeDefaultsPreserveExplicitEmptyAndNull");
    }
    RecipeOptions no_merge; no_merge.set_option("merge_args", false);
    defaults->backend_ops()->resolve_runtime_options(info, no_merge);
    check(no_merge.get_option("fork_args") == "", "RuntimeDefaultsRespectMergeArgs");
    for (const auto& argument : {"--parallel 4", "-np 4"}) {
        auto aliases = document;
        aliases["merge"]["launch"]["custom_args"]["defaults"][0]["aliases"] = {"-np"};
        const auto alias_entry = ConfiguredBackendEntry::make(runtime_fixture::executable(aliases));
        RecipeOptions selected; selected.set_option("fork_args", argument);
        alias_entry->backend_ops()->resolve_runtime_options(info, selected);
        check(selected.get_option("fork_args") == argument, "RuntimeDefaultAliasesPreserveOverride");
    }
    const auto candidate_document = Json::parse(read_bytes("llamacpp.candidate.json"));
    std::shared_ptr<const ExecutableDefinition> candidate_executable;
    const bool candidate_compiles = !throws([&] { candidate_executable = runtime_fixture::executable(candidate_document); });
    check(candidate_compiles, "RuntimeCandidateCompilesWithDeclaredRuntimeFacts");
    if (!candidate_compiles) return finish();
    const auto candidate_entry = ConfiguredBackendEntry::make(candidate_executable);
    check(candidate_entry->bindings.size() == 11, "RuntimeCandidateHasElevenRealBoundCells");
    auto all_document = binding_fixture::document();
    const auto all = runtime_fixture::executable(all_document);
    DefinitionRunner runner(all);
    const auto all_entry = ConfiguredBackendEntry::make(all);
    all_entry->backend_ops()->resolve_runtime_options(info, launch_only);
    BackendSpec forged_aliases(all_entry->spec.recipe, all_entry->spec.binary, all, {}, false, true);
    const auto forged_context = configured_install_context(forged_aliases, "cpu", "request", "pin", native_platform());
    check(throws([&] { DefinitionRunner(all).install_params(forged_context); }, "ownership"), "EmptyAliasesRemainBuiltinLlamaOnly");
    check(candidate_entry->spec.public_recipe.empty() && candidate_entry->spec.pin_recipe.empty() &&
        candidate_entry->spec.effective_public_recipe() == "llamacpp" && candidate_entry->spec.effective_pin_recipe() == "llamacpp",
        "FrozenEmptyAliasesRetainEffectiveBuiltinOwner");
    const auto& all_program = ex::Inspection::program(all->compiled());
    const auto service_guard = [&](rt::Frame& service_frame, typed::Contract contract) {
        const auto found = std::find_if(all_program.hooks.begin(), all_program.hooks.end(), [&](const auto& hook) { return hook.contract == contract; });
        for (const auto& input : found->inputs) { service_frame.facts.insert(input.fact); if (input.producer) service_frame.produced.insert(*input.producer); }
        const auto& slot = found->slot;
        return rt::TestAccess::guard(service_frame, *all, static_cast<std::size_t>(found - all_program.hooks.begin()), *found, slot.owner_kind, slot.owner_id, slot.stage);
    };
    unsigned fact_calls = 0;
    rt::InstallFactSources fact_sources{[&] { ++fact_calls; return "v7.12.1"; }, [&] { ++fact_calls; return "gfx1151"; }, [&] { ++fact_calls; return "sm_120"; }};
    InstallContext install{all_entry->spec, {}, all_entry->descriptor.recipe, all_entry->descriptor.recipe,
        "cpu", "request", "pin", "resolved", native_platform(), InstallPhase::Repository, all};
    const auto repository = runner.install_params(install, fact_sources);
    check(repository.repo == "test/engine" && repository.filename.empty() && fact_calls == 0, "RepositoryObtainsNoAssetFactsOrFilename");
    install.phase = InstallPhase::Asset;
    check(runner.install_params(install, fact_sources).filename == "engine-resolved" + install.platform.os && fact_calls == 0, "SelectedCaseWithoutFactsIsLazy");
    install.executable_definition = executable;
    check(throws([&] { runner.install_params(install, fact_sources); }, "ownership") && fact_calls == 0, "InstallRejectsForeignOwnerBeforeFacts");
    auto fact_document = all_document;
    auto& release = fact_document["merge"]["install"]["cases"][0];
    release["backend"] = {"rocm-stable"};
    release["filename"] = {{"concat", {"runtime-", {{"ref", "/install/therock_version"}}, "-", {{"ref", "/install/version"}}}}};
    const auto fact_executable = runtime_fixture::executable(fact_document);
    DefinitionRunner fact_runner(fact_executable);
    const auto fact_entry = ConfiguredBackendEntry::make(fact_executable);
    InstallContext fact_install{fact_entry->spec, {}, fact_entry->descriptor.recipe, fact_entry->descriptor.recipe,
        "rocm-stable", "request", "pin", "resolved", native_platform(), InstallPhase::Repository, fact_executable};
    check(!throws([&] {
        if (!fact_runner.install_params(fact_install, fact_sources).filename.empty() || fact_calls != 0) throw std::logic_error("eager asset fact");
    }), "RepositoryDoesNotEvaluateSelectedAssetExpression");
    fact_install.phase = InstallPhase::Asset;
    check(fact_runner.install_params(fact_install, fact_sources).filename == "runtime-7.12-resolved" && fact_calls == 1, "AssetRequestsOnlySelectedEnumeratedFact");
    rt::Frame install_frame(*all);
    auto install_guard = service_guard(install_frame, typed::Contract::InstallContext);
    typed::InstallFactsInput fact_input{typed::InstallPhase::Repository, typed::Platform::Linux, "rocm-stable", "", "", "", {typed::InstallFact::TheRockVersion}};
    rt::InstallServices repository_services(*install_guard, fact_input, fact_sources);
    check(throws([&] { repository_services.therock_version(); }, "phase/case") && fact_calls == 1, "RepositoryServiceRejectsAssetFact");
    fact_input.phase = typed::InstallPhase::Asset; fact_input.requested.clear();
    rt::InstallServices unrequested(*install_guard, fact_input, fact_sources);
    check(throws([&] { unrequested.therock_version(); }, "phase/case") && fact_calls == 1, "InstallServiceRejectsUnrequestedFact");
    install_guard.reset();
    std::vector<std::string> install_events;
    BackendSpec observed_spec(all_entry->spec.recipe, all_entry->spec.binary, all,
        [&](const InstallContext& context) {
            install_events.push_back(context.phase == InstallPhase::Repository ? "repository" : "asset");
            check(context.executable_definition == all && context.requested_version == "request" && context.pinned_version == "pin" &&
                context.pin_recipe == "binding-fixture" && context.public_recipe == "binding-fixture", "BackendManagerPhaseContextRetainsOwnerAndVersionInputs");
            return all_entry->spec.install_params_fn(context);
        });
    ConfiguredInstallResolution install_resolution;
    const auto phases_ok = !throws([&] {
        install_resolution = configured_install_params(observed_spec, "cpu", "request", "pin", native_platform(), [&](const std::string& repo) {
            install_events.push_back("resolve");
            check(repo == "test/engine", "BackendManagerResolvesVersionFromSelectedRepository");
            return "tag";
        });
    });
    check(phases_ok && install_events == std::vector<std::string>{"repository", "resolve", "asset"} &&
        install_resolution.asset.filename == "engine-tag" + native_platform().os && install_resolution.version == "tag",
        "BackendManagerPhaseConsumerUsesRepositoryThenResolvedAsset");
    for (const auto& [backend, field, expected] : std::vector<std::tuple<std::string, std::string, std::string>>{
        {"rocm-nightly", "rocm_asset_family", "gfx1151"}, {"cuda", "cuda_architecture", "sm_120"}}) {
        auto specialized = fact_document;
        specialized["merge"]["install"]["cases"][0]["backend"] = {backend};
        specialized["merge"]["install"]["cases"][0]["filename"] = {{"ref", "/install/" + field}};
        const auto selected = runtime_fixture::executable(specialized);
        const auto selected_entry = ConfiguredBackendEntry::make(selected);
        auto selected_context = configured_install_context(selected_entry->spec, backend, "request", "pin", native_platform());
        const auto before_facts = fact_calls;
        DefinitionRunner selected_runner(selected);
        selected_runner.install_params(selected_context, fact_sources);
        check(fact_calls == before_facts, "RepositoryNeverQueriesAssetFamilies:" + backend);
        selected_context.phase = InstallPhase::Asset;
        check(!throws([&] {
            if (selected_runner.install_params(selected_context, fact_sources).filename != expected || fact_calls != before_facts + 1)
                throw std::logic_error("wrong selected fact");
        }),
            "SelectedCaseRequestsExactlyItsFact:" + backend);
    }
    auto conditional_document = fact_document;
    conditional_document["merge"]["install"]["cases"][0]["backend"] = {"cuda", "rocm-stable"};
    conditional_document["merge"]["install"]["cases"][0]["filename"] = {{"coalesce", Json::array({
        {{"when", Json::array({{{"eq", Json::array({{{"ref", "/install/backend"}}, "cuda"})}}, {{"ref", "/install/cuda_architecture"}}})}},
        {{"ref", "/install/therock_version"}}})}};
    const auto conditional = runtime_fixture::executable(conditional_document);
    const auto conditional_entry = ConfiguredBackendEntry::make(conditional);
    for (const auto& backend : {"cuda", "rocm-stable"}) {
        auto selected_context = configured_install_context(conditional_entry->spec, backend, "request", "pin", native_platform());
        selected_context.phase = InstallPhase::Asset;
        const auto before_facts = fact_calls;
        check(!throws([&] {
            const auto filename = DefinitionRunner(conditional).install_params(selected_context, fact_sources).filename;
            if (filename != (std::string(backend) == "cuda" ? "sm_120" : "7.12") || fact_calls != before_facts + 1) throw std::logic_error("eager branch");
        }), "InstallFactsFollowWhenAndCoalesceBranches:" + std::string(backend));
    }
    for (const auto& operation : {"all", "any"}) {
        auto short_circuit = fact_document;
        short_circuit["merge"]["install"]["cases"][0]["filename"] = {{"string", {{operation, Json::array({std::string(operation) == "any", {{"present", {{"ref", "/install/cuda_architecture"}}}}})}}}};
        const auto selected = runtime_fixture::executable(short_circuit);
        const auto selected_entry = ConfiguredBackendEntry::make(selected);
        auto selected_context = configured_install_context(selected_entry->spec, "rocm-stable", "request", "pin", native_platform());
        selected_context.phase = InstallPhase::Asset;
        const auto before_facts = fact_calls;
        check(!throws([&] {
            const auto filename = DefinitionRunner(selected).install_params(selected_context, fact_sources).filename;
            if (filename != (std::string(operation) == "any" ? "true" : "false") || fact_calls != before_facts) throw std::logic_error("eager boolean branch");
        }), "InstallBooleanShortCircuitSkipsFacts:" + std::string(operation));
    }
    auto repeated_fact_document = fact_document;
    auto& repeated_fact_case = repeated_fact_document["merge"]["install"]["cases"][0];
    repeated_fact_case["filename"] = {{"concat", Json::array({{{"ref", "/install/therock_version"}}, {{"ref", "/install/therock_version"}}})}};
    repeated_fact_case["version_override"] = {{"ref", "/install/pinned_version"}};
    const auto repeated = runtime_fixture::executable(repeated_fact_document);
    const auto repeated_entry = ConfiguredBackendEntry::make(repeated);
    auto repeated_context = configured_install_context(repeated_entry->spec, "rocm-stable", "request", "pin", native_platform());
    repeated_context.phase = InstallPhase::Asset;
    const auto before_repeated = fact_calls;
    const auto repeated_result = DefinitionRunner(repeated).install_params(repeated_context, fact_sources);
    check(repeated_result.filename == "7.127.12" && repeated_result.version_override == "pin" && fact_calls == before_repeated + 1,
        "InstallFactIsReadOnceAcrossFilenameReferences");
    auto filtered_document = all_document;
    auto& filtered_case = filtered_document["merge"]["install"]["cases"][0];
    filtered_case["os"] = {"linux"}; filtered_case["machine_arch"] = {"x86_64"}; filtered_case["arch"] = {"gfx1151"};
    const auto filtered = runtime_fixture::executable(filtered_document);
    const auto filtered_entry = ConfiguredBackendEntry::make(filtered);
    DefinitionRunner filtered_runner(filtered);
    for (const auto& axis : {"backend", "os", "machine_arch", "arch"}) {
        auto selected_context = configured_install_context(filtered_entry->spec, "cpu", "request", "pin", {"linux", "x86_64", "gfx1151"});
        if (std::string(axis) == "backend") selected_context.backend = "cuda";
        if (std::string(axis) == "os") selected_context.platform.os = "windows";
        if (std::string(axis) == "machine_arch") selected_context.platform.machine_arch = "aarch64";
        if (std::string(axis) == "arch") selected_context.platform.rocm_arch = "gfx0000";
        const auto before_facts = fact_calls;
        check(throws([&] { filtered_runner.install_params(selected_context, fact_sources); }, "case: " + std::string(axis)) && fact_calls == before_facts,
            "CaseSelectorsRunBeforeFacts:" + std::string(axis));
    }
    unsigned architecture_queries = 0;
    install_platform(filtered_entry->spec, "cpu", {[] { return "windows"; }, [&] { ++architecture_queries; return "gfx1151"; }});
    install_platform(filtered_entry->spec, "cuda", {[] { return "linux"; }, [&] { ++architecture_queries; return "gfx1151"; }});
    check(architecture_queries == 0, "InstallPlatformFiltersBackendAndOsBeforeArchitecture");
    check(install_platform(filtered_entry->spec, "cpu", {[] { return "linux"; }, [&] { ++architecture_queries; return "gfx1151"; }}).rocm_arch == "gfx1151" && architecture_queries == 1,
        "InstallPlatformQueriesArchitectureForEligibleCase");
    auto unsupported_machine = filtered_document;
    unsupported_machine["merge"]["install"]["cases"][0]["machine_arch"] = {native_platform().machine_arch == "x86_64" ? "aarch64" : "x86_64"};
    const auto unsupported_machine_entry = ConfiguredBackendEntry::make(runtime_fixture::executable(unsupported_machine));
    install_platform(unsupported_machine_entry->spec, "cpu", {[] { return "linux"; }, [&] { ++architecture_queries; return "gfx1151"; }});
    check(architecture_queries == 1, "InstallPlatformFiltersMachineBeforeArchitecture");
    unsigned probes = 0;
    rt::ProbeSources probes_sources{
        [&](std::size_t bytes, unsigned wait, const std::function<bool()>& cancel) {
            ++probes;
            check(bytes == 65536 && wait == 5000 && !cancel(), "ProbeReadAndWaitAreBounded");
            return utils::CapturedProcess{"version: 1234", 0, false, false, false};
        }, [&] { ++probes; return true; }, [&] { ++probes; return false; }};
    check(runner.resolve_version("cpu", "file-version", probes_sources) == "file-version" && probes == 0, "ManagedVersionNeverProbesSystem");
    check(runner.resolve_version("system", "", probes_sources) == "b1234" && probes == 1, "SystemVersionUsesFixedPurposeService");
    check(!runner.check_install("system", true, probes_sources).installed && probes == 3, "SystemKfdAndHipChecksAreEligibleAndLazy");
    runner.check_install("cpu", true, probes_sources); runner.check_install("system", false, probes_sources);
    check(probes == 3, "IneligibleInstallNeverProbes");
    auto limited = probes_sources;
    limited.version = [](auto, auto, const auto&) { return utils::CapturedProcess{"version: 1", 0, true, false, false}; };
    check(throws([&] { runner.resolve_version("system", "", limited); }, "Bounded"), "TruncatedProbeCannotBecomeVersion");
    rt::Frame system_frame(*all);
    auto system_guard = service_guard(system_frame, typed::Contract::System);
    rt::SystemServices ineligible(*system_guard, {typed::Platform::Linux, "cpu", true}, probes_sources);
    check(throws([&] { ineligible.llama_version(); }, "eligible") && probes == 3, "SystemServiceCannotProbeArbitraryBackend");
    system_guard.reset();
    const auto capture_binary = std::filesystem::absolute(argv[0]).string();
    const auto captured = utils::ProcessManager::capture_output(capture_binary, {"--capture-helper", "bytes"}, 1024, 1000);
    check(captured.output.size() == 1024 && captured.byte_limit, "ByteCaptureBoundsNewlineFreeCombinedOutput");
    const auto start = std::chrono::steady_clock::now();
    const auto waited = utils::ProcessManager::capture_output(capture_binary, {"--capture-helper", "wait"}, 1024, 20);
    check(waited.timed_out && std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "ByteCaptureWaitIsBounded");
    check(throws([&] { utils::ProcessManager::capture_output(capture_binary, {}, 65537, 1000); }, "policy") &&
        throws([&] { utils::ProcessManager::capture_output(capture_binary, {}, 1024, 5001); }, "policy"), "CaptureRejectsUnboundedPolicy");
    const auto cancelled_capture = utils::ProcessManager::capture_output(capture_binary, {"--capture-helper", "wait"}, 1024, 1000, [] { return true; });
    check(cancelled_capture.cancelled, "CancelledProbeDoesNotSpawn");
    const auto* temporary = std::getenv("TMPDIR");
    const auto scratch = std::filesystem::path(temporary ? temporary : ".synagent/scratch/backend-v2-4d1b/temp") / "runtime-consumer-fixture";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch / "snapshots" / "active");
    std::filesystem::create_directories(scratch / "blobs");
    std::filesystem::create_directories(scratch / "refs");
#ifdef __linux__
    check(!throws([&] { rt::environment_sources().stage("gfx1151", scratch.string()); }), "StagingFalseResultRetainsLegacyNonFailure");
#endif
    {
        std::ofstream file(scratch / "blobs" / "hash", std::ios::binary);
        file << "GGUF";
        const auto number = [&](auto value) { file.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
        const auto text = [&](const std::string& value) { number(static_cast<std::uint64_t>(value.size())); file.write(value.data(), value.size()); };
        const auto scalar = [&](const std::string& key, std::uint32_t value) { text(key); number(std::uint32_t{4}); number(value); };
        const std::uint32_t version = 3;
        const std::uint64_t zero = 0;
        number(version); number(zero); number(std::uint64_t{8});
        text("general.architecture"); number(std::uint32_t{8}); text("qwen3");
        scalar("qwen3.context_length", 65536); scalar("qwen3.block_count", 2);
        scalar("qwen3.embedding_length", 128); scalar("qwen3.attention.key_length", 32);
        text("qwen3.attention.head_count_kv"); number(std::uint32_t{9}); number(std::uint32_t{4}); number(std::uint64_t{2});
        number(std::uint32_t{4}); number(std::uint32_t{6});
        text("qwen3.attention.sliding_window_pattern"); number(std::uint32_t{9}); number(std::uint32_t{7}); number(std::uint64_t{2});
        number(std::uint8_t{1}); number(std::uint8_t{0});
        scalar("qwen3.nextn_predict_layers", 1);
    }
    { std::ofstream file(scratch / "refs" / "main"); file << "active\n"; }
    std::filesystem::create_symlink("../../blobs/hash", scratch / "snapshots" / "active" / "model-Q4_K_M.gguf");
    { std::ofstream file(scratch / "blobs" / "auxhash"); file << "GGUF"; }
    std::filesystem::create_symlink("../../blobs/auxhash", scratch / "snapshots" / "active" / "mmproj.gguf");
    rt::Frame artifact_frame(*all);
    auto artifact_guard = service_guard(artifact_frame, typed::Contract::Artifacts);
    rt::ArtifactServices reads(*artifact_guard, scratch.string(), true);
    const auto inventory = reads.entries(reads.root());
    const auto snapshot = std::find_if(inventory.begin(), inventory.end(), [](const auto& item) {
        return item.active && std::filesystem::path(item.resolved_path).filename() == "model-Q4_K_M.gguf";
    });
    check(snapshot != inventory.end() && reads.prefix(snapshot->handle, 4) == "GGUF", "ArtifactInventoryApprovesHfSnapshotBlobRelationship");
    check(throws([&] { reads.prefix(snapshot->handle, 262145); }, "limit"), "ArtifactPrefixIsBounded");
    rt::ArtifactServices foreign(*artifact_guard, scratch.string(), true);
    check(throws([&] { foreign.prefix(snapshot->handle, 4); }, "another service"), "ForeignArtifactHandleRejects");
    check(throws([&] { reads.approved_result((scratch / "unknown").string()); }, "inventory"), "ReturnedPathMustBelongToInventory");
    const auto artifact_hook = std::find_if(all_program.hooks.begin(), all_program.hooks.end(), [](const auto& hook) { return hook.contract == typed::Contract::Artifacts; });
    const auto retained_lifetime = artifact_guard->lifetime().lock();
    rt::TestAccess::reuse(*artifact_guard, artifact_frame, *all, static_cast<std::size_t>(artifact_hook - all_program.hooks.begin()), *artifact_hook);
    check(throws([&] { reads.prefix(snapshot->handle, 4); }, "Expired"), "ExpiredArtifactHandleRejects");
    using ArtifactStorage = std::aligned_storage_t<sizeof(rt::ArtifactServices), alignof(rt::ArtifactServices)>;
    ArtifactStorage storage;
    auto* reused = new (&storage) rt::ArtifactServices(*artifact_guard, (scratch / "blobs" / "hash").string(), false);
    const auto old_handle = reused->root();
    reused->~ArtifactServices();
    reused = new (&storage) rt::ArtifactServices(*artifact_guard, (scratch / "blobs" / "hash").string(), false);
    check(throws([&] { reused->prefix(old_handle, 4); }, "Foreign or expired"), "ReusedServiceCannotAcceptExpiredArtifactHandle");
    reused->~ArtifactServices();
    artifact_guard.reset();
    CheckpointResolveContext resolve;
    resolve.model_cache_path = scratch.string(); resolve.type = "main"; resolve.variant = "Q4_K_M";
    check(runner.resolve_artifact(resolve) == snapshot->resolved_path && runner.find_imported(scratch.string()) == snapshot->resolved_path,
        "ArtifactConsumerUsesBoundOwnerAndApprovedInventory");
    auto auxiliary = resolve; auxiliary.type = "mmproj"; auxiliary.variant = "mmproj.gguf";
    check(!throws([&] {
        if (runner.resolve_artifact(auxiliary) != (scratch / "snapshots" / "active" / "mmproj.gguf").string()) throw std::logic_error("wrong auxiliary");
    }), "AuxiliaryResolutionUsesBoundArtifactOwnerAndActiveInventory");
    auto artifact_model = model(); artifact_model.resolved_paths["main"] = snapshot->resolved_path; artifact_model.labels.clear();
    all_entry->backend_ops()->populate_metadata(artifact_model, {});
    check(artifact_model.max_context_window == 65536 && artifact_model.gguf.architecture == "qwen3" && artifact_model.gguf.head_count_kv == 10 &&
        artifact_model.gguf.head_count_kv_per_layer == std::vector<std::int64_t>{4, 6} && artifact_model.gguf.swa_layer_count == 1 &&
        artifact_model.gguf.embedding_length == 128 && artifact_model.gguf.key_length == 32 && artifact_model.labels == std::vector<std::string>{"mtp"},
        "BoundedHeaderPopulatesTypedMetadataAndCapabilityLabels");
    check(all_entry->backend_ops()->validate_checkpoint_file(snapshot->resolved_path).empty() &&
        !all_entry->backend_ops()->validate_registration_checkpoint("owner/model-GGUF").empty(), "ArtifactMetadataAndValidationConsumeTypedMembers");
    std::filesystem::create_symlink(capture_binary, scratch / "snapshots" / "active" / "escape.gguf");
    check(throws([&] { runner.resolve_artifact(resolve); }, "outside approved"), "ArtifactInventoryRejectsAmbientFilesystemEscape");
    std::filesystem::remove(scratch / "snapshots" / "active" / "escape.gguf");
    std::filesystem::create_symlink("blobs/hash", scratch / "alias.gguf");
    check(throws([&] { runner.resolve_artifact(resolve); }, "snapshot/blob"), "ArtifactLinkRequiresApprovedSnapshotBlobRelationship");
    std::filesystem::remove(scratch / "alias.gguf");
#ifndef _WIN32
    const auto binary_directory = scratch / "native probe ; literal";
    std::filesystem::create_directory(binary_directory);
    std::filesystem::copy_file(capture_binary, binary_directory / "llama-server");
    std::filesystem::copy_file(capture_binary, binary_directory / "other-engine");
    const auto previous_path = utils::get_environment_variable_utf8("PATH");
    utils::set_environment_variable_utf8("PATH", binary_directory.string());
    bool fixed_version = false;
    try { fixed_version = runner.resolve_version("system", "") == "b3432"; } catch (const std::exception&) {}
    utils::set_environment_variable_utf8("PATH", previous_path);
    check(fixed_version, "FixedProbeNeverUsesShellOrArbitraryExecutable");
#endif
    rt::Frame environment_frame(*all);
    auto environment_guard = service_guard(environment_frame, typed::Contract::Environment);
    unsigned parent_reads = 0, runtime_reads = 0, stages = 0;
    std::string staged_root;
    rt::EnvironmentSources environment_sources{
        [&](auto) -> std::optional<std::string> { ++parent_reads; return "parent"; },
        [&](auto) { ++runtime_reads; return std::string("gfx1151"); },
        [&](const auto&, const auto& root) { ++stages; staged_root = root; }};
    auto environment_service = rt::TestAccess::environment(*environment_guard, environment_sources, scratch.string());
    check(throws([&] { environment_service->parent(static_cast<typed::ParentKey>(99)); }, "enumerated") && parent_reads == 0, "EnvironmentRejectsNonEnumeratedParentKey");
    check(throws([&] { environment_service->runtime(static_cast<typed::RuntimeQuery>(99)); }, "enumerated") && runtime_reads == 0, "EnvironmentRejectsNonEnumeratedRuntimeQuery");
    for (const auto key : {typed::ParentKey::Path, typed::ParentKey::LlamaDevice, typed::ParentKey::MetalNoResidency, typed::ParentKey::Home,
        typed::ParentKey::LibraryPath, typed::ParentKey::CudaVisibleDevices, typed::ParentKey::PrimeRenderOffload}) {
        check(!throws([&] { if (environment_service->parent(key) != "parent") throw std::logic_error("wrong parent"); }),
            "EnvironmentEnumeratedParentKeySetExact");
    }
    for (const auto query : {typed::RuntimeQuery::RocmArchitecture, typed::RuntimeQuery::TheRockDirectory, typed::RuntimeQuery::CudaVisibleDevices,
        typed::RuntimeQuery::HomeDirectory, typed::RuntimeQuery::CudaArchitecture}) {
        check(!throws([&] { if (environment_service->runtime(query) != "gfx1151") throw std::logic_error("wrong runtime"); }),
            "EnvironmentEnumeratedRuntimeQuerySetExact");
    }
    const auto staging_capability = rt::TestAccess::capability(*environment_service);
    environment_service->runtime(typed::RuntimeQuery::RocmArchitecture);
    typed::EnvironmentPlan staging{{{"PATH", "child"}}, {{staging_capability, "gfx1151"}}};
    const auto child = rt::StagingConsumer::execute(rt::TestAccess::seal(*environment_service, staging));
    check(child == staging.assignments && stages == 1 && staged_root == scratch.string(), "StagingUsesIssuedRootAndChildAssignments");
    auto unissued_architecture = staging; unissued_architecture.staging.front().architecture = "json-architecture";
    check(throws([&] { rt::TestAccess::seal(*environment_service, unissued_architecture); }, "capability") && stages == 1,
        "StagingRejectsArchitectureOutsideRuntimeQuery");
    auto foreign_environment = rt::TestAccess::environment(*environment_guard, environment_sources, scratch.string());
    foreign_environment->runtime(typed::RuntimeQuery::RocmArchitecture);
    check(throws([&] { rt::TestAccess::seal(*foreign_environment, staging); }, "capability"), "ForeignStagingCapabilityRejects");
    auto expired_staging = rt::TestAccess::seal(*environment_service, staging);
    rt::TestAccess::expire(expired_staging);
    check(throws([&] { rt::StagingConsumer::execute(std::move(expired_staging)); }, "Expired") && stages == 1, "ExpiredStagingEnvelopeRejects");
    environment_guard.reset();
    auto launch_document = runtime_document();
    launch_document["merge"]["hooks"]["environment"] = all_document["merge"]["hooks"]["environment"];
    launch_document["merge"]["launch"]["backend"]["fixed"] = "rocm-stable";
    const auto launch_bound = runtime_fixture::executable(launch_document);
    const auto launch_entry = ConfiguredBackendEntry::make(launch_bound);
    DefinitionRunner launch_runner(launch_bound);
    Launch launch_service;
    launch_service.host.os = "windows"; launch_service.supported = {"rocm-stable"};
    launch_service.program = (scratch / "selected-engine").string();
    { std::ofstream file(launch_service.program); file << "fake"; }
    RecipeOptions launch_options; launch_options.set_option("staging_path", (scratch / "json-escape").string());
    const auto launch_result = launch_runner.prepare_load({"fixture", info, launch_options, "error", {}, {}}, launch_entry->spec,
        launch_service, {}, environment_sources);
    check(stages == 2 && staged_root == scratch.string() && launch_result.plan.command.env.size() == 3,
        "JsonPathCannotSupplyStagingAuthority");
    auto staging_parent = launch_document;
    staging_parent["merge"]["metadata"]["options"] = Json::array();
    staging_parent["merge"]["launch"].erase("custom_args");
    staging_parent["merge"]["launch"]["program"]["source"] = "external_or_managed";
    staging_parent["merge"]["launch"]["program"]["install_policy"] = "always";
    const auto external_root = scratch / "external-install";
    std::filesystem::create_directory(external_root);
    launch_service.external_program = (external_root / "selected-engine").string();
    { std::ofstream file(launch_service.external_program); file << "fake"; }
    const auto staging_parent_bound = runtime_fixture::executable(staging_parent);
    const auto staging_parent_entry = ConfiguredBackendEntry::make(staging_parent_bound);
    DefinitionRunner(staging_parent_bound).prepare_load({"fixture", info, launch_options, "error", {}, {}}, staging_parent_entry->spec, launch_service, {}, environment_sources);
    check(stages == 3 && staged_root == external_root.string(), "LoaderBuiltinMayStageSelectedExternalRoot");
    auto staging_child = admission_fixture::child("staging-child", "unused");
    staging_child["extends"] = {{"builtin", "binding-fixture"}, {"contract", 1}};
    const auto staging_child_bound = runtime_fixture::executable(staging_child, true, builtins(false, &staging_parent, true));
    const auto staging_child_entry = ConfiguredBackendEntry::make(staging_child_bound);
    DefinitionRunner(staging_child_bound).prepare_load({"fixture", info, launch_options, "error", {}, {}}, staging_child_entry->spec, launch_service, {}, environment_sources);
    check(stages == 3, "ExternalLeafNeverGetsBuiltinExternalStagingAuthority");
    launch_service.external_program.clear();
    DefinitionRunner(staging_child_bound).prepare_load({"fixture", info, launch_options, "error", {}, {}}, staging_child_entry->spec, launch_service, {}, environment_sources);
    check(stages == 4 && staged_root == scratch.string(), "ExternalLeafCanStageItsManagedRoot");
    launch_service.external_program = (external_root / "selected-engine").string();
    auto installing_document = runtime_document();
    installing_document["merge"]["install"] = all_document["merge"]["install"];
    installing_document["merge"]["launch"]["prepare_order"] = {"backend", "install", "hook", "artifacts", "program", "ports"};
    const auto installing = runtime_fixture::executable(installing_document);
    const auto installing_entry = ConfiguredBackendEntry::make(installing);
    Launch installing_service; installing_service.allow_install = true;
    DefinitionRunner(installing).prepare_load({"fixture", info, launch_options, "error", {}, {}}, installing_entry->spec, installing_service);
    const auto installed_event = std::find(installing_service.events.begin(), installing_service.events.end(), "install:binding-fixture:cpu");
    check(installed_event != installing_service.events.end() && installed_event < std::find(installing_service.events.begin(), installing_service.events.end(), "program"),
        "InstallPhaseUsesLeafIdentityBeforeProgram");
    const auto mapped = launch_runner.map_request(BackendOperation::ChatCompletion, OperationDelivery::Buffered, ModelType::LLM,
        *launch_result.session, {{"max_completion_tokens", 3}});
    launch_runner.map_response(BackendOperation::ChatCompletion, ModelType::LLM, *launch_result.session, {}, mapped.body);
    for (const auto* member : {"backend.select_backend", "backend.validate_device", "install_context", "runtime_options", "prepare", "environment",
        "system.resolve_version", "system.check_install", "artifacts.metadata", "artifacts.validate_registration", "artifacts.validate_file", "artifacts.resolve_path", "artifacts.find_imported",
        "max_tokens_alias", "tool_schema_limits", "client_model"}) {
        check(std::find(entries.begin(), entries.end(), member) != entries.end(), std::string("AllTenBoundContractsConsumed:") + member);
    }
    auto external_document = runtime_document();
    external_document["merge"].erase("hooks");
    external_document["merge"]["launch"]["argv"].erase(1);
    external_document["merge"]["launch"]["prepare_order"] = {"backend", "artifacts", "program", "ports"};
    external_document["merge"]["install"] = all_document["merge"]["install"];
    for (auto& operation : external_document["merge"]["operations"]) { operation["request"].erase("hooks"); operation.erase("response"); }
    const auto external_bound = runtime_fixture::executable(external_document, true);
    const auto external_entry = ConfiguredBackendEntry::make(external_bound);
    check(configured_external_default_version(external_entry->spec, "cpu", Json::object()) == "v1" &&
        configured_external_default_version(external_entry->spec, "cpu", {{"binding-fixture", {{"cpu", "shipped"}}}}) == "shipped" &&
        configured_external_default_version(external_entry->spec, "cpu", {{"other-recipe", {{"cpu", "foreign"}}}}) == "v1",
        "LeafPinOwnershipAndShippedDefaultPrecedence");
    auto external_only_document = external_document;
    external_only_document["merge"]["install"] = {{"strategy", "external_only"}};
    external_only_document["merge"]["launch"]["prepare_order"] = {"backend", "artifacts", "program", "ports"};
    const auto external_only = runtime_fixture::executable(external_only_document, true);
    const auto external_only_entry = ConfiguredBackendEntry::make(external_only);
    check(configured_external_only(external_only_entry->spec) && !throws([&] {
        DefinitionRunner(external_only).prepare_load({"fixture", info, launch_options, "error", {}, {}}, external_only_entry->spec, launch_service);
    }), "ExternalOnlyNeverDownloads");
    const auto hrx = runtime_fixture::executable(binding_fixture::document(true));
    const auto& hrx_program = ex::Inspection::program(hrx->compiled());
    const auto hrx_hook = std::find_if(hrx_program.hooks.begin(), hrx_program.hooks.end(), [](const auto& hook) { return hook.contract == typed::Contract::Artifacts; });
    rt::Frame hrx_frame(*hrx); hrx_frame.facts = {typed::Fact::ModelArtifacts, typed::Fact::ModelLabels};
    auto hrx_guard = rt::TestAccess::guard(hrx_frame, *hrx, static_cast<std::size_t>(hrx_hook - hrx_program.hooks.begin()), *hrx_hook,
        hrx_hook->slot.owner_kind, hrx_hook->slot.owner_id, hrx_hook->slot.stage);
    rt::SystemServices hrx_system(*hrx_guard, {typed::Platform::Linux, "system", true}, probes_sources);
    auto hrx_environment = rt::TestAccess::environment(*hrx_guard, environment_sources, scratch.string());
    const auto before_probes = probes;
    const auto before_parents = parent_reads;
    const auto before_runtime = runtime_reads;
    check(throws([&] { hrx_system.llama_version(); }, "Effect outside") &&
        throws([&] { hrx_environment->parent(typed::ParentKey::Path); }, "Effect outside") &&
        throws([&] { hrx_environment->runtime(typed::RuntimeQuery::RocmArchitecture); }, "Effect outside") &&
        probes == before_probes && parent_reads == before_parents && runtime_reads == before_runtime,
        "ArtifactOnlyHrxCannotObtainRuntimeOrSystemEffects");
    rt::ArtifactServices hrx_reads(*hrx_guard, (scratch / "blobs" / "hash").string(), false);
    check(hrx_reads.prefix(hrx_reads.root(), 4) == "GGUF", "ArtifactOnlyHrxRetainsApprovedReads");
    auto nul_path = (scratch / "blobs" / "hash").string(); nul_path.push_back('\0'); nul_path += "escape";
    check(throws([&] { rt::ArtifactServices invalid(*hrx_guard, nul_path, false); }, "Invalid approved"), "ArtifactInputCannotHidePathAfterNul");
    const auto entry_limit = scratch / "entry-limit";
    std::filesystem::create_directory(entry_limit);
    for (unsigned i = 0; i < 2049; ++i) std::filesystem::create_directory(entry_limit / std::to_string(i));
    check(throws([&] { rt::ArtifactServices oversized(*hrx_guard, entry_limit.string(), true); }, "entry/depth limit"),
        "ArtifactEntryLimitBoundsEmptyDirectoryTraversal");
    auto deep = scratch / "depth-limit";
    const auto depth_limit = deep;
    for (unsigned i = 0; i < 19; ++i) { std::filesystem::create_directory(deep); deep /= "nested"; }
    check(throws([&] { rt::ArtifactServices oversized(*hrx_guard, depth_limit.string(), true); }, "entry/depth limit"),
        "ArtifactDepthIsBounded");
    rt::Frame denied_frame(*all);
    auto denied_guard = service_guard(denied_frame, typed::Contract::System);
    check(throws([&] { rt::ArtifactServices denied(*denied_guard, (scratch / "blobs" / "hash").string(), false); }, "Effect outside"),
        "ArtifactAuthorityCheckedBeforeOpeningInventory");
    const auto before_denied_facts = fact_calls;
    rt::InstallServices denied_facts(*denied_guard, {typed::InstallPhase::Asset, typed::Platform::Linux, "rocm-stable", "", "", "", {typed::InstallFact::TheRockVersion}}, fact_sources);
    check(throws([&] { denied_facts.therock_version(); }, "Effect outside") && fact_calls == before_denied_facts,
        "InstallAuthorityCheckedBeforeReadingFacts");
    auto legacy_install = fact_document;
    legacy_install["merge"]["install"]["repository_result"] = "full_params";
    legacy_install["merge"]["install"]["repository_facts"] = {"therock_version"};
    legacy_install["merge"]["install"]["fact_timing"] = "before_platform";
    legacy_install["merge"]["install"]["unmanaged_backends"] = {"system"};
    legacy_install["merge"]["install"]["backend_fallback"] = "rocm-stable";
    legacy_install["merge"]["install"]["cases"][0]["os"] = {"linux"};
    legacy_install["merge"]["install"]["errors"] = {{"os", "legacy platform error"}, {"machine_arch", "legacy machine error"}, {"no_case", "legacy no case"}};
    const auto legacy_executable = runtime_fixture::executable(legacy_install);
    const auto legacy_entry = ConfiguredBackendEntry::make(legacy_executable);
    const DefinitionRunner legacy_runner(legacy_executable);
    auto legacy_context = configured_install_context(legacy_entry->spec, "rocm-stable", "request", "pin", {"linux", "x86_64", ""});
    auto before_legacy = fact_calls;
    InstallParams legacy_repository;
    const bool legacy_repository_ok = !throws([&] { legacy_repository = legacy_runner.install_params(legacy_context, fact_sources); });
    check(legacy_repository_ok && legacy_repository.repo == "test/engine" && legacy_repository.filename == "runtime-7.12-pin" && fact_calls == before_legacy + 1,
        "DeclaredRepositoryFactProducesFullPinnedParameters");
    legacy_context.phase = InstallPhase::Asset;
    legacy_context.resolved_version = "resolved";
    before_legacy = fact_calls;
    check(legacy_runner.install_params(legacy_context, fact_sources).filename == "runtime-7.12-resolved" && fact_calls == before_legacy + 1,
        "DeclaredAssetFactUsesResolvedVersion");
    legacy_context.backend = "system";
    before_legacy = fact_calls;
    check(legacy_runner.install_params(legacy_context, fact_sources).repo.empty() && fact_calls == before_legacy,
        "DeclaredUnmanagedBackendDoesNotReadFacts");
    legacy_context.backend = "unknown";
    InstallParams fallback_params;
    const bool fallback_ok = !throws([&] { fallback_params = legacy_runner.install_params(legacy_context, fact_sources); });
    check(fallback_ok && fallback_params.filename == "runtime-7.12-resolved" && fact_calls == before_legacy + 1,
        "DeclaredFallbackAppliesOnlyToInstallEvaluation");
    legacy_context.backend = "rocm-stable";
    legacy_context.platform.os = "macos";
    before_legacy = fact_calls;
    check(throws([&] { legacy_runner.install_params(legacy_context, fact_sources); }, "legacy platform error") && fact_calls == before_legacy + 1,
        "DeclaredBeforePlatformFactPrecedesDeclaredPlatformError");
    auto selected_install = legacy_install;
    selected_install["merge"]["install"]["fact_timing"] = "selected_case";
    const auto selected_executable = runtime_fixture::executable(selected_install);
    const auto selected_install_entry = ConfiguredBackendEntry::make(selected_executable);
    auto selected_install_context = configured_install_context(selected_install_entry->spec, "rocm-stable", "request", "pin", {"macos", "x86_64", ""});
    before_legacy = fact_calls;
    check(throws([&] { DefinitionRunner(selected_executable).install_params(selected_install_context, fact_sources); }, "legacy platform error") && fact_calls == before_legacy,
        "DefaultSelectedCaseStillRejectsPlatformBeforeFacts");
    auto declared_fact_input = fact_input;
    declared_fact_input.phase = typed::InstallPhase::Repository;
    declared_fact_input.requested = {typed::InstallFact::TheRockVersion};
    declared_fact_input.repository_facts = {typed::InstallFact::TheRockVersion};
    rt::Frame declared_frame(*all);
    auto declared_guard = service_guard(declared_frame, typed::Contract::InstallContext);
    rt::InstallServices declared_services(*declared_guard, declared_fact_input, fact_sources);
    before_legacy = fact_calls;
    std::string declared_value;
    const bool declared_ok = !throws([&] { declared_value = declared_services.therock_version(); });
    check(declared_ok && declared_value == "v7.12.1" && fact_calls == before_legacy + 1,
        "RepositoryServiceAcceptsOnlyDeclaredRequestedFact");
    check(throws([&] { declared_services.cuda_architecture(); }, "phase/case") && fact_calls == before_legacy + 1,
        "RepositoryServiceRejectsAnotherFactDespiteOptIn");
    declared_guard.reset();
    const auto reject_install = [&](const Json& value, const std::string& expected, const std::string& label) {
        const auto admitted = admit({}, builtins(false, &value));
        const auto compiled = compile_execution_definition(admitted.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, value.at("id"), 1}));
        check(!compiled.definition && std::any_of(compiled.diagnostics.entries.begin(), compiled.diagnostics.entries.end(),
            [&](const auto& error) { return error.code == expected; }), label);
    };
    auto foreign_stream_policy = runtime_document();
    foreign_stream_policy["merge"]["launch"]["direct_stream_policy"] = "llamacpp_legacy";
    reject_install(foreign_stream_policy, "unsupported_execution_feature", "LegacyDirectStreamingPolicyIsBuiltinOnly");
    const ExecutionSession strict_session{all, {}, {}, {}, "fixture"};
    check(throws([&] { runner.map_stream("/unknown", ModelType::LLM, strict_session, "raw", false, 1); }, "not bound"),
        "DefaultDirectStreamingStillRejectsUnknownEndpoints");
    auto invalid_runtime = candidate_document;
    invalid_runtime["merge"]["launch"]["custom_args"]["defaults"][0]["when"] = {{"ref", "/runtime/draft"}};
    reject_install(invalid_runtime, "invalid_reference", "RuntimeFactsCannotInventFields");
    invalid_runtime = candidate_document;
    invalid_runtime["merge"]["launch"]["custom_args"]["defaults"][0]["stage"] = "launch";
    reject_install(invalid_runtime, "phase_dependency", "RuntimeFactsCannotLeakIntoLaunchDefaults");
    auto invalid_install = legacy_install;
    invalid_install["merge"]["install"].erase("repository_facts");
    reject_install(invalid_install, "undeclared_repository_fact", "FullParametersCannotObtainUndeclaredRepositoryFacts");
    invalid_install = legacy_install;
    invalid_install["merge"]["install"].erase("repository_result");
    reject_install(invalid_install, "invalid_install_policy", "RepositoryFactDeclarationRequiresFullParameters");
    invalid_install = legacy_install;
    invalid_install["merge"]["hooks"].erase("install_context");
    invalid_install["merge"]["install"]["cases"][0]["filename"] = "no-fact";
    reject_install(invalid_install, "invalid_install_policy", "RepositoryFactsRequireBoundInstallContract");
    invalid_install = legacy_install;
    invalid_install["merge"]["install"]["repository_facts"].push_back("cuda_architecture");
    reject_install(invalid_install, "unused_repository_fact", "RepositoryFactDeclarationCannotContainUnusedCapability");
    invalid_install = legacy_install;
    invalid_install["merge"]["install"]["backend_fallback"] = "absent";
    reject_install(invalid_install, "invalid_install_policy", "InstallFallbackMustNameDeclaredBackendGroup");
    invalid_install = legacy_install;
    invalid_install["merge"]["install"]["unmanaged_backends"] = {"rocm-stable"};
    reject_install(invalid_install, "invalid_install_policy", "UnmanagedBackendCannotOverlapManagedCase");
    invalid_install = legacy_install;
    invalid_install["merge"]["install"]["strategy"] = "external_only";
    invalid_install["merge"]["install"]["cases"][0]["filename"] = "no-fact";
    reject_install(invalid_install, "invalid_install_policy", "LegacyInstallPoliciesRequireManagedReleases");
    invalid_install = legacy_install;
    invalid_install["merge"]["install"]["cases"][0]["filename"] = {{"coalesce", Json::array({"constant", {{"ref", "/install/therock_version"}}})}};
    reject_install(invalid_install, "ambiguous_install_facts", "BeforePlatformCannotEagerlyEvaluateConditionalFactBranches");
    invalid_install = legacy_install;
    auto other_platform = invalid_install["merge"]["install"]["cases"][0];
    other_platform["id"] = "another-platform";
    other_platform["os"] = {"windows"};
    other_platform["filename"] = "no-fact";
    invalid_install["merge"]["install"]["cases"].push_back(other_platform);
    reject_install(invalid_install, "ambiguous_install_facts", "BeforePlatformGroupRequiresIdenticalFactRequirements");
    invalid_install = legacy_install;
    invalid_install["merge"]["install"]["cases"][0]["backend"] = {"cuda"};
    reject_install(invalid_install, "invalid_install_fact_backend", "BeforePlatformCannotReadAnotherBackendFact");
    llama_corpus::witnesses(candidate_executable, candidate_entry);
    llama_corpus::parity(candidate_executable, *candidate_entry);
    llamacpp_install_parity(candidate_executable, *candidate_entry);
    llamacpp_runtime_parity(candidate_executable);
    llamacpp_protocol_parity(candidate_executable, candidate_entry);
    llamacpp_loopback(candidate_entry);
    std::filesystem::remove_all(scratch);
    } catch (const std::exception& error) {
        std::printf("Runtime rejection: %s\n", error.what());
        check(false, "RuntimeServicesReachNamedBoundaries");
    }
    return finish();
}
