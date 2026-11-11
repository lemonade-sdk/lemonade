#include "backend_catalog_publication_internal.h"
#include "backend_definition_binding_fixture.h"
#include "backend_definition_runner_internal.h"
#include "lemon/auto_tune.h"
#include "lemon/backends/configured/configured_server.h"
#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/utils/path_utils.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace binding_fixture;
using namespace lemon;
namespace fs = std::filesystem;

namespace lemon::backends {
struct ContainerPrerequisiteTestAccess {
    static std::shared_ptr<const BackendCatalog> publish(std::shared_ptr<const ConfiguredBackendEntry> entry) {
        auto catalog = std::shared_ptr<BackendCatalog>(new BackendCatalog(BackendCatalog::Unpublished{}, nullptr));
        catalog->entries_.push_back(entry);
        catalog->registrations_.push_back({&entry->descriptor, [](const BackendContext&) -> std::unique_ptr<WrappedServer> { throw std::invalid_argument("fixture construction reached before host admission"); }, &entry->spec, entry->v2_ops.get(), entry.get()});
        catalog->data_ = std::make_shared<BackendCatalogData>();
        catalog->data_->descriptors.push_back(&entry->descriptor);
        CatalogPublicationGuard guard(catalog_publication());
        guard.commit(catalog->data_, catalog);
        return catalog;
    }
};
}

namespace lemon {
class HostStub final : public WrappedServer {
public:
    HostStub(const std::string& name, const BackendDescriptor& descriptor, ResidencyClass residency, std::atomic<unsigned>& unloads)
        : WrappedServer("fixture", "error", nullptr, nullptr), unloads_(unloads) {
        set_descriptor(&descriptor);
        set_model_metadata(name, "", ModelType::LLM, DEVICE_CPU, RecipeOptions("gufo", Json::object()));
        set_residency_class(residency);
        set_state(ModelState::READY);
    }
    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override {}
    void unload() override { ++unloads_; }
    bool is_backend_alive() const override { return true; }
    void busy(bool value) { std::lock_guard<std::mutex> lock(state_mutex_); maintenance_in_progress_ = value; }
private:
    std::atomic<unsigned>& unloads_;
};
struct HostExclusivityTestAccess {
    static void add(Router& router, std::unique_ptr<HostStub> server) {
        std::lock_guard<std::mutex> lock(router.load_mutex_);
        router.loaded_servers_.push_back(std::move(server));
    }
    static std::size_t admit(Router& router, SlotPolicy policy, ResidencyClass residency = ResidencyClass::Standard) {
        std::lock_guard<std::mutex> lock(router.load_mutex_);
        router.enforce_host_exclusivity_locked(policy, "second", residency);
        return router.loaded_servers_.size();
    }
};
}

namespace {
struct HostServices : LaunchServices {
    std::string properties = "capability 134217728\nctl_stack_size 1024\nsimd_count 5\ncwsr_size 959488\n";
    mutable std::vector<unsigned> property_nodes;
    std::string container_gpu_properties(unsigned node) const override { property_nodes.push_back(node); return properties; }
    std::vector<ContainerGpuNode> container_gpu_nodes() const override { return {{2, "gfx1151"}, {1, "gfx1100"}}; }
    bool container_supports_arch(const std::string&, const std::string&, const std::string& arch) const override { return arch == "gfx1151"; }
    std::string container_image(const ContainerPolicy& policy) const override { return policy.repository + "@sha256:" + std::string(64, 'a'); }
    void install_container_image(const std::string&) override {}
    bool exists(const std::string& path) const override { return fs::exists(path); }
    std::vector<std::string> supported_backends(const std::string&) const override { return {}; }
    void validate_backend(const std::string&, const std::string& backend) const override { if (backend != "rocm") throw std::invalid_argument("backend"); }
    std::string external_binary(const BackendSpec&, const std::string&) const override { throw std::logic_error("native forbidden"); }
    void install_backend(const std::string&, const std::string&) override { throw std::logic_error("native forbidden"); }
    std::string managed_binary(const BackendSpec&, const std::string&) const override { throw std::logic_error("native forbidden"); }
    int allocate_port() override { return 1; }
    int find_free_port(int) override { return 1; }
    std::vector<std::pair<std::string, std::string>> environment(const std::string&, const std::string&, const std::string&, const std::string&) const override { return {}; }
    PlatformInfo platform() const override { return {"linux", "x86_64", "gfx1151"}; }
    long default_timeout_seconds() const override { return 2; }
};

std::shared_ptr<const ConfiguredBackendEntry> entry(const Json& doc) {
    auto declaration = doc.at("merge"); declaration["recipe"] = doc.at("recipe");
    const auto semantics = container_execution_owner(declaration);
    const BuiltinContainerExecutionOwner owner{{"gufo", 1}, semantics, v2::identity("lemonade.backend-container-owner.v1\n", semantics)};
    const auto admitted = admit({}, builtins(false, &doc, false, &owner));
    if (!admitted.catalog) { diagnostics(admitted.diagnostics); throw std::logic_error("Fixture admission failed"); }
    const auto token = admitted.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, "gufo", 1});
    const auto compiled = compile_execution_definition(token);
    if (!compiled.definition) { diagnostics(compiled.diagnostics); throw std::logic_error("Fixture compilation failed"); }
    const auto bound = bind_execution_definition(admitted.catalog, compiled.definition);
    if (!bound.definition) { diagnostics(bound.diagnostics); throw std::logic_error("Fixture binding failed"); }
    return ConfiguredBackendEntry::make(bound.definition);
}
template<class Call> void rejects(Call call, const std::string& label, const std::string& message = "") {
    bool caught = false;
    try { call(); } catch (const std::exception& error) { caught = message.empty() || std::string(error.what()).find(message) != std::string::npos; }
    check(caught, label);
}
void halogen_facts(const Json& fixture, const std::shared_ptr<const ConfiguredBackendEntry>& backend, const fs::path& scratch) {
    const auto& ops = *backend->v2_ops;
    const auto& selection = fixture.at("selection");
    ModelInfo model; model.recipe = "gufo"; model.type = ModelType::LLM; model.device = DEVICE_CPU;
    ops.populate_metadata(model, {});
    check(model.max_context_window == 262144, "HalogenNativeContextMetadata");
    for (const auto& raw : std::vector<Json>{nullptr, "auto", -1, 0, -8, 12.5, true, 8192}) {
        RecipeOptions options; options.set_option("ctx_size", raw);
        ops.resolve_runtime_options(model, options);
        check(options.get_option("ctx_size") == (raw == 8192 ? 8192 : 262144), "HalogenNativeContext:" + raw.dump());
    }
    const auto bundle = scratch / "bundle";
    fs::create_directories(bundle / "tokenizer");
    for (const auto* name : {"weights.hgn", "table-ngram.hgn", "vision-vision.hgn", "tokenizer/tokenizer.json"}) std::ofstream(bundle / name) << "fixture";
    model.resolved_paths["main"] = (bundle / "weights.hgn").string();
    bool management = false;
    try {
        management = ops.validate_registration_checkpoint("org/bundle:weights.hgn").empty() &&
            ops.validate_checkpoint_file(model.resolved_path()).empty() && ops.find_imported_checkpoint(bundle.string()).empty();
    } catch (const std::exception&) {}
    check(management, "HalogenManagementDefaultsNeedNoArtifactHook");
    const auto cache = scratch / "hgn-cache";
    fs::create_directories(cache / "refs"); fs::create_directories(cache / "snapshots/revision");
    std::ofstream(cache / "refs/main") << "revision\n";
    std::ofstream(cache / "snapshots/revision/weights.hgn") << "fixture";
    CheckpointResolveContext resolve;
    resolve.model_cache_path = cache.string(); resolve.variant = "weights.hgn"; resolve.type = "main";
    check(ops.resolve_checkpoint_path(model, resolve) == (cache / "snapshots/revision/weights.hgn").string(), "HalogenNormalSnapshotResolution");

    model.extras["halogen_lookup_table"] = "table-ngram.hgn";
    model.extras["halogen_vision_tower"] = "vision-vision.hgn";
    HostServices services;
    DefinitionRunner runner(backend->executable_definition);
    RecipeOptions options; options.set_option("ctx_size", -1); ops.resolve_runtime_options(model, options);
    const auto prepare = [&] { return runner.prepare_load({"fixture", model, options, "error", {}, {}}, backend->spec, services); };
    const auto full = prepare();
    std::map<std::string, std::string> expected{{"HALOGEN_CHECKPOINT", "/mnt/models/weights.hgn"}, {"HALOGEN_NGRAM_TABLE", "/mnt/models/table-ngram.hgn"},
        {"HALOGEN_TOKENIZER", "/mnt/models/tokenizer"}, {"HALOGEN_VISION_TOWER", "/mnt/models/vision-vision.hgn"}, {"HALOGEN_API_PORT", "1"}, {"HALOGEN_CTX", "262144"}, {"HIP_VISIBLE_DEVICES", "1"}};
    check(std::map<std::string, std::string>(full.plan.command.env.begin(), full.plan.command.env.end()) == expected && full.plan.command.program.empty() &&
        full.plan.command.args.empty() && full.plan.command.ready_endpoint == "/health" && full.plan.readiness_timeout_seconds == 1800, "HalogenExactEntrypointEnvironmentAndReadiness");
    check(services.property_nodes == std::vector<unsigned>{2}, "HalogenChecksSelectedKfdNode");
    check(full.plan.container && full.plan.container->mounts.size() == 4, "HalogenAllBundleMounts");
    model.extras.erase("halogen_vision_tower");
    const auto text = prepare();
    expected.erase("HALOGEN_VISION_TOWER");
    check(std::map<std::string, std::string>(text.plan.command.env.begin(), text.plan.command.env.end()) == expected &&
        text.plan.container->mounts.size() == 3, "HalogenTextBundleOmitsVision");
    model.extras["halogen_vision_tower"] = "absent.hgn";
    rejects(prepare, "HalogenMissingVisionRejected", "is missing " + (bundle / "absent.hgn").string());
    model.extras.erase("halogen_vision_tower");
    model.extras["halogen_lookup_table"] = "absent.hgn";
    rejects(prepare, "HalogenMissingTableRejected", "is missing " + (bundle / "absent.hgn").string());
    model.extras["halogen_lookup_table"] = "../outside.hgn";
    rejects(prepare, "HalogenEscapingExtraRejected", "remain in the snapshot");
    model.extras.erase("halogen_lookup_table");
    rejects(prepare, "HalogenMissingTableExtraRejected", "names no halogen_lookup_table");
    model.extras["halogen_lookup_table"] = "table-ngram.hgn";
    for (const auto& row : fixture.at("host_cases")) {
        services.properties = row.at("properties");
        if (row.at("failure").is_null()) { check(bool(prepare().plan.container), "HalogenHostAccepted:" + row.at("name").get<std::string>()); continue; }
        const auto& failure = row.at("failure");
        const utils::SetupFailure wanted{failure.at("id"), failure.at("message"), failure.at("steps"), failure.at("apply")};
        bool exact = false;
        try { prepare(); } catch (const std::exception& error) { exact = error.what() == utils::SetupReport{{wanted}}.text(); }
        check(exact, "HalogenHostFailure:" + row.at("name").get<std::string>());
    }
    services.properties.clear();
    check(bool(prepare().plan.container), "HalogenEmptyPropertiesSourcePassthrough");
    services.properties = std::string(65537, 'x');
    rejects(prepare, "HalogenPropertiesBounded");
    auto modified = backend->executable_definition->compiled().normalized_declaration();
    modified["model_contract"] = "ignored";
    check(!AuthorityInspection::permits_execution(backend->executable_definition->compiled().authorized(), modified), "HalogenModelContractSealed");
    modified = backend->executable_definition->compiled().normalized_declaration();
    modified["metadata"]["slot_policy"] = "standard";
    check(!AuthorityInspection::permits_execution(backend->executable_definition->compiled().authorized(), modified), "HalogenHostPolicySealed");
}
void router_contract(const std::shared_ptr<const ConfiguredBackendEntry>& backend, const fs::path& scratch) {
    utils::set_cache_dir((scratch / "cache").string()); utils::set_config_dir((scratch / "config").string());
    fs::create_directories(scratch / "config");
    std::ifstream defaults(fs::path(LEMON_TEST_FIXTURES).parent_path().parent_path() / "src/cpp/resources/defaults.json");
    auto settings = Json::parse(defaults);
    settings["disable_model_filtering"] = true; settings["offline"] = true; settings["auto_evict"] = false; settings["inhibit_suspend"] = false;
    settings["log_level"] = "error"; settings["max_loaded_models"] = 4;
    RuntimeConfig config(settings);
    RuntimeConfig::set_global(&config);
    ModelManager models;
    std::atomic<unsigned> unloads{0};
    BackendDescriptor local; local.slot_policy = SlotPolicy::Standard;
    BackendDescriptor cloud; cloud.slot_policy = SlotPolicy::Unmetered;
    Router router(&config, &models, nullptr);
    ModelInfo model; model.model_name = "second"; model.recipe = "gufo"; model.type = ModelType::LLM; model.device = DEVICE_CPU; model.size = 1e12;
    RecipeOptions automatic("gufo", Json::object()); automatic.set_option("ctx_size", -1);
    const auto effective = router.resolve_effective_options(model, automatic);
    check(effective.get_option("ctx_size") == 262144 && resolve_auto_ctx_size(effective, model) == -2, "HalogenContextBeforeRouterMemoryAdmission");
    auto resident = std::make_unique<HostStub>("first", backend->descriptor, ResidencyClass::Standard, unloads);
    auto* resident_ptr = resident.get(); resident_ptr->busy(true);
    HostExclusivityTestAccess::add(router, std::move(resident));
    rejects([&] { router.load_model("second", model, automatic); }, "RouterSecondExclusiveAdmissionRefused", "exclusive host access");
    check(unloads == 0, "RouterBusyExclusiveRemainsResident");
    std::atomic<unsigned> refused{0};
    const auto concurrent = [&] {
        try { router.load_model("second", model, automatic); }
        catch (const std::exception& error) { if (std::string(error.what()).find("exclusive host access") != std::string::npos) ++refused; }
    };
    std::thread first(concurrent), second(concurrent);
    first.join(); second.join();
    check(refused == 2 && unloads == 0, "RouterConcurrentExclusiveAdmissionsRefused");
    resident_ptr->busy(false);
    check(HostExclusivityTestAccess::admit(router, SlotPolicy::Standard) == 0 && unloads == 1, "RouterLocalAdmissionEvictsExclusive");
    HostExclusivityTestAccess::add(router, std::make_unique<HostStub>("local", local, ResidencyClass::Standard, unloads));
    HostExclusivityTestAccess::add(router, std::make_unique<HostStub>("cloud", cloud, ResidencyClass::Standard, unloads));
    check(HostExclusivityTestAccess::admit(router, SlotPolicy::ExclusiveHost) == 1 && unloads == 2, "RouterExclusiveEvictsLocalKeepsCloud");
    HostExclusivityTestAccess::add(router, std::make_unique<HostStub>("exclusive", backend->descriptor, ResidencyClass::Standard, unloads));
    check(HostExclusivityTestAccess::admit(router, SlotPolicy::Unmetered) == 2 && unloads == 2, "RouterCloudDoesNotDisplaceExclusive");
    rejects([&] { HostExclusivityTestAccess::admit(router, SlotPolicy::ExclusiveHost, ResidencyClass::RoutingHelper); }, "RouterExclusiveCrossResidencyRefused");
    check(HostExclusivityTestAccess::admit(router, SlotPolicy::ExclusiveHost) == 1 && unloads == 3, "RouterSecondIdleExclusiveSerializedByEviction");
}
}

#include "c6b_ds4_contract.h"

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        const auto scratch = fs::path(LEMON_PREREQUISITE_SCRATCH);
        fs::create_directories(scratch);
        const auto fixture = Json::parse(read_bytes("c6b.halogen.json"));
        const auto backend = entry(fixture.at("definition"));
        for (const auto* invalid : {"unknown-profile", "standard-slot", "missing-role", "nonboolean-environment", "missing-host-phase"}) {
            auto changed = fixture.at("definition");
            const std::string name = invalid;
            if (name == "unknown-profile") changed["merge"]["model_contract"] = "arbitrary";
            if (name == "standard-slot") changed["merge"]["metadata"]["slot_policy"] = "standard";
            if (name == "missing-role") changed["merge"]["artifacts"].erase("table");
            if (name == "missing-host-phase") changed["merge"]["launch"]["prepare_order"].erase(0);
            if (name == "nonboolean-environment") changed["merge"]["launch"]["environment"]["set"][0]["when"] = "yes";
            rejects([&] { entry(changed); }, "HalogenClosedContract:" + name);
        }
        const auto published = ContainerPrerequisiteTestAccess::publish(backend);
        check(backend->descriptor.slot_policy == SlotPolicy::ExclusiveHost &&
            std::string(slot_policy_to_string(backend->descriptor.slot_policy)) == "exclusive_host", "ExclusiveHostSchemaAndDescriptor");
        halogen_facts(fixture, backend, scratch);
        ds4_contract(scratch);
        router_contract(backend, scratch);
        RuntimeConfig::set_global(nullptr);
    } catch (const std::exception& error) { std::printf("Prerequisite exception: %s\n", error.what()); check(false, "ContainerPrerequisitePipeline"); }
    return finish();
}
