#include "backend_definition_binding_fixture.h"
#include "backend_definition_runner_internal.h"
#include "lemon/backend_manager.h"
#include "lemon/backends/configured/backend_spec_utils.h"
#include "lemon/backends/configured/configured_server.h"
#include "lemon/backends/configured/container_execution.h"
#include "lemon/container_process.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/http_client.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace binding_fixture;
using namespace lemon;
namespace fs = std::filesystem;

namespace lemon {
struct BackendManagerTestAccess {
    static void pins(BackendManager& manager, const json& pins) { manager.backend_versions_ = pins; }
};
}

namespace lemon::backends {
struct ConfiguredServerTestAccess {
    static void effects(ConfiguredServer& server, ConfiguredServerServices services) { server.services_ = std::move(services); }
};
}

namespace {
const std::string pin = "reviewed@sha256:" + std::string(64, 'a');
const std::string image = "ghcr.io/peonist-ai/gufo@sha256:" + std::string(64, 'a');
const Json versions{{"gufo", {{"rocm", pin}}}};
Json document() { return Json::parse(read_bytes("gufo.container.json")); }
BuiltinContainerExecutionOwner reviewed_owner() {
    auto doc = document();
    auto declaration = doc.at("merge");
    declaration["recipe"] = doc.at("recipe");
    auto semantics = container_execution_owner(declaration);
    return {{"gufo", 1}, semantics, "sha256:ded7ece9ac805215ad9d50f5cf5d1938194125fc79cfae43c79208105a39c50a"};
}
Fixture authorized() {
    const auto doc = document();
    const auto owner = reviewed_owner();
    auto parents = builtins(false, &doc, false, &owner);
    auto result = admit({}, parents);
    if (!result.catalog) { diagnostics(result.diagnostics); throw std::logic_error("Container admission failed"); }
    auto token = result.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, "gufo", 1});
    auto compiled = compile_execution_definition(token);
    if (!compiled.definition) throw std::logic_error("Container compilation failed");
    return {result.catalog, token, compiled.definition};
}
template<class Call> void rejects(Call call, const std::string& label) {
    bool rejected = false;
    try { call(); } catch (const std::exception&) { rejected = true; }
    check(rejected, label);
}
struct FakeHost {
    std::vector<std::vector<std::string>> commands;
    bool installed = false;
    bool allow_pull = true;
    utils::ContainerHost host() {
        utils::ContainerHost host;
        host.on_path = [](const auto& name) { return name == "podman" || name == "getsubids"; };
        host.gpu_devices_accessible = [] { return true; };
        host.group_id = [](const auto&) { return "109"; };
        host.account = [] { return "fixture"; };
        host.env = [](const auto&) { return ""; };
        host.is_root = [] { return false; };
        host.read_file = [](const auto&) { return ""; };
        host.run = [&](const auto& argv, int, bool) {
            commands.push_back(argv);
            if (argv[0] == "getsubids") return utils::CommandResult{0, "0: fixture 100000 65536"};
            if (argv[1] == "info") return utils::CommandResult{0, "true netavark"};
            if (argv[1] == "image" && argv[2] == "inspect") return utils::CommandResult{installed ? 0 : 1, ""};
            return utils::CommandResult{0, ""};
        };
        host.stream = [&](const auto& argv, int, const auto& line) {
            commands.push_back(argv);
            if (!allow_pull) throw std::logic_error("Pull forbidden");
            installed = line("Copying blob abc done");
            return installed ? 0 : 1;
        };
        host.canonical = [](const auto& path) { return fs::canonical(path).string(); };
        host.file_kind = [](const auto& path) { return fs::is_regular_file(path) ? utils::ContainerFileKind::File :
            fs::is_directory(path) ? utils::ContainerFileKind::Directory : utils::ContainerFileKind::Other; };
        host.files = [](const auto& path) { std::vector<std::string> files; for (const auto& entry : fs::directory_iterator(path)) files.push_back(entry.path().string()); return files; };
        host.sleep = [](auto) {};
        return host;
    }
    bool ran(const std::vector<std::string>& argv) const {
        return std::find(commands.begin(), commands.end(), argv) != commands.end();
    }
};
struct ClientState {
    bool alive = false;
    unsigned starts = 0, stops = 0;
    ServerCommand invocation;
};
class FakeClient final : public ServerProcess {
public:
    explicit FakeClient(ClientState& state) : ServerProcess({}), state_(state) {}
    std::string start(const ServerCommand& command) override {
        state_.invocation = command; state_.alive = true; ++state_.starts;
        return "127.0.0.1";
    }
    void stop() override { if (state_.alive) ++state_.stops; state_.alive = false; }
    bool running() const override { return state_.alive; }
private:
    ClientState& state_;
};
class Services final : public LaunchServices {
public:
    int port = 1;
    BackendManager& manager;
    std::string image_override;
    bool install = true;
    std::string system_os = "linux";
    explicit Services(BackendManager& manager) : manager(manager) {}
    bool exists(const std::string& path) const override { return fs::exists(path); }
    std::vector<std::string> supported_backends(const std::string&) const override { return {}; }
    void validate_backend(const std::string&, const std::string& backend) const override {
        if (backend != "rocm") throw std::invalid_argument("Wrong backend");
    }
    std::string external_binary(const BackendSpec&, const std::string&) const override { throw std::logic_error("External program forbidden"); }
    void install_backend(const std::string&, const std::string&) override { throw std::logic_error("Managed install forbidden"); }
    std::string managed_binary(const BackendSpec&, const std::string&) const override { throw std::logic_error("Managed program forbidden"); }
    int allocate_port() override { return port; }
    int find_free_port(int) override { return port; }
    std::vector<std::pair<std::string, std::string>> environment(const std::string&, const std::string&, const std::string&, const std::string&) const override { return {}; }
    PlatformInfo platform() const override { return {system_os, "x86_64", "gfx1151"}; }
    long default_timeout_seconds() const override { return 2; }
    std::string container_image(const ContainerPolicy& policy) const override { return image_override.empty() ? utils::container_image(policy.repository, manager.container_pin(policy)) : image_override; }
    void install_container_image(const std::string& identity) override { if (install) manager.install_container_image(identity); }
};

void authority_and_plans(const Fixture& fixture, const fs::path& scratch) {
    const auto bound = bind(fixture);
    check(bound.definition != nullptr, "ReviewedBuiltinContainerBinds");
    if (!bound.definition) { diagnostics(bound.diagnostics); return; }
    auto hooked_doc = document();
    hooked_doc["merge"]["hooks"] = {{"launch", {{"id", "llamacpp.prepare.v1"}, {"params", Json::object()}}}};
    hooked_doc["merge"]["launch"]["prepare_order"] = {"backend", "hook", "artifacts", "program", "ports"};
    const auto hook_owner = reviewed_owner();
    const auto hooked_admission = admit({}, builtins(false, &hooked_doc, false, &hook_owner));
    check(hooked_admission.catalog != nullptr, "HookBearingContainerOwnerAdmitted");
    if (hooked_admission.catalog) {
        const auto hooked_token = hooked_admission.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, "gufo", 1});
        const auto no_hooks = AuthorityInspection::test_without_grants(*hooked_token);
        check(AuthorityInspection::grant_count(*hooked_token) == 1 && AuthorityInspection::grant_count(*no_hooks) == 0 &&
            AuthorityInspection::permits_execution(*no_hooks, hooked_token->definition().declaration), "SealedExecutionOwnerIndependentOfHookGrants");
        auto child_doc = child("gufo-child-without-hooks", "unused");
        child_doc["extends"] = {{"builtin", "gufo"}, {"contract", 1}};
        child_doc["edits"] = Json::array({{{"op", "remove"}, {"path", "/hooks"}},
            {{"op", "replace"}, {"path", "/launch/prepare_order"}, {"value", {"backend", "artifacts", "program", "ports"}}}});
        const auto child_admission = admit({child_doc}, builtins(false, &hooked_doc, false, &hook_owner));
        check(!child_admission.catalog && (has_code(child_admission.diagnostics, "container_execution_unavailable") ||
            has_code(child_admission.diagnostics, "unexported_builtin")), "DescendantRemovingAllHooksCannotInheritContainerExecution");
    }
    const auto stripped = AuthorityInspection::test_without_grants(*fixture.token);
    check(AuthorityInspection::permits_execution(*stripped, fixture.compiled->normalized_declaration()), "ExecutionOwnerSurvivesEveryHookRemoval");
    auto changed_owner = fixture.compiled->normalized_declaration();
    changed_owner["containers"]["rocm"]["repository"] = "ghcr.io/peonist-ai/foreign";
    check(!AuthorityInspection::permits_execution(*stripped, changed_owner), "HookRemovalCannotEraseSealedExecutionOwner");
    const std::vector<std::string> protected_paths{"/kind", "/containers/rocm/repository", "/containers/rocm/pin/recipe",
        "/containers/rocm/devices", "/containers/rocm/cap_add", "/containers/rocm/policy", "/containers/rocm/network",
        "/containers/rocm/teardown", "/containers/rocm/stop_grace_seconds", "/launch/execution/cases/0/program/source",
        "/launch/execution/cases/0/backends", "/launch/backend/fixed", "/launch/mounts/0/artifact", "/launch/mounts/0/destination",
        "/launch/address/source", "/launch/environment/preset", "/launch/working_directory", "/install/strategy", "/artifacts/main/role"};
    for (const auto& path : protected_paths) {
        auto changed = fixture.compiled->normalized_declaration();
        changed[Json::json_pointer(path)] = "changed";
        bool denied = false;
        try { denied = !AuthorityInspection::permits_execution(*stripped, changed); } catch (const std::exception&) { denied = true; }
        check(denied, "SealedExecutionOwner:" + path);
    }
    auto altered = document();
    altered["merge"]["containers"]["rocm"]["repository"] = "ghcr.io/peonist-ai/foreign";
    const auto reviewed = reviewed_owner();
    const auto mismatched = admit({}, builtins(false, &altered, false, &reviewed));
    check(!mismatched.catalog && has_code(mismatched.diagnostics, "sealed_execution_owner_mismatch"), "BuiltinCannotChangeReviewedExecutionOwner");
    const auto doc = document();
    const auto ungranted = admit({}, builtins(false, &doc));
    const auto compiled = compile_execution_definition(ungranted.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, "gufo", 1}));
    check(!bind_execution_definition(ungranted.catalog, compiled.definition).definition, "ContainerExecutionWithoutGrantRejected");
    const auto external = admit({doc}, empty_builtins());
    check(!external.catalog && has_code(external.diagnostics, "container_execution_unavailable"), "HookFreeExternalCannotExecute");
    auto descendant = child("gufo-child", "unused");
    descendant["extends"] = {{"builtin", "gufo"}, {"contract", 1}};
    descendant.erase("edits");
    const auto owner = reviewed_owner();
    const auto inheritance = admit({descendant}, builtins(false, &doc, false, &owner));
    check(!inheritance.catalog, "ContainerInheritanceUnavailableAfterHookRemoval");
    auto ir = ex::Inspection::program(*fixture.compiled);
    rejects([&] { selected_execution_case(ir, "cpu"); }, "MissingExecutionCaseRejected");
    auto second = ir.launch.execution_cases.front(); second.id = "second"; ir.launch.execution_cases.push_back(second);
    rejects([&] { selected_execution_case(ir, "rocm"); }, "TwoMatchingExecutionCasesRejected");
    ir = ex::Inspection::program(*fixture.compiled);
    ir.normalized.erase("containers");
    rejects([&] { selected_execution_case(ir, "rocm"); }, "ContainerCaseWithoutPolicyRejected");
    ir = ex::Inspection::program(*fixture.compiled);
    const auto policy = container_policy(ir, ir.launch.execution_cases.front());
    check(effective_container_pin(policy, versions, "") == pin && effective_container_pin(policy, versions, "builtin") == pin, "BackendVersionsPinResolved");
    const auto override_pin = "override@sha256:" + std::string(64, 'b');
    check(effective_container_pin(policy, versions, override_pin) == override_pin, "DigestPinOverrideResolved");
    rejects([&] { effective_container_pin(policy, Json{{"gufo", {{"rocm", "latest"}}}}, ""); }, "MutableTagCannotReachLaunch");
    for (const auto& invalid : {std::string("latest"), std::string("/host/engine"), std::string("tag@sha256:") + std::string(64, 'A')})
        rejects([&] { effective_container_pin(policy, versions, invalid); }, "PinOverrideCannotBypassValidation");
    rejects([&] { effective_container_pin(policy, Json::object(), override_pin); }, "MissingPinSourceRejectsOverride");
    check(valid_container_image(policy.repository, image) && !valid_container_image(policy.repository, policy.repository + ":latest") &&
        !valid_container_image(policy.repository, "ghcr.io/peonist-ai/other@sha256:" + std::string(64, 'a')), "OnlyAuthorizedDigestIdentityAtLaunch");
    auto entry = ConfiguredBackendEntry::make(bound.definition);
    check(configured_container_policy(entry->spec, "rocm").has_value(), "OCIInstallationUsesLeafPolicy");
    BackendSpec foreign_spec("foreign", entry->spec.binary, bound.definition);
    rejects([&] { configured_container_policy(foreign_spec, "rocm"); }, "OCIInstallationRejectsForeignOwner");
    auto* previous_config = RuntimeConfig::global();
    RuntimeConfig host_override(Json{{"gufo", {{"rocm_bin", (scratch / "mount-inventory" / "directory" / "first.bin").string()}}}});
    RuntimeConfig::set_global(&host_override);
    rejects([&] { BackendUtils::get_backend_binary_path(entry->spec, "rocm"); }, "OCIHasNoHostExecutableSubstitution");
    RuntimeConfig::set_global(previous_config);
    rejects([&] { entry->spec.install_params_fn(configured_install_context(entry->spec, "rocm", "", pin, {"linux"})); }, "ContainerLeafOwnsOCIInstallation");
    auto frame = runtime::Frame(*bound.definition);
    frame.data.values = {{"artifacts", {{"outside", (scratch / "mount-inventory" / "directory" / "first.bin").string()}}}};
    ir.launch.mounts.front().artifact = "outside";
    LaunchPlan plan; plan.container = utils::ContainerRunSpec{};
    rejects([&] { materialize_container_mounts(ir, frame, plan, [](const auto&) { return std::optional<Json>{true}; }); }, "MountOutsideInventoryRejected");
}

void inventory(const fs::path& scratch) {
    const auto shard_files = std::vector<std::string>{"model-00001-of-00002.gguf", "model-00002-of-00002.gguf", "other-00001-of-00001.gguf"};
    check(runtime::ArtifactServices::gguf_shards(shard_files[0], shard_files) == std::set<std::string>{shard_files[0], shard_files[1]}, "GGUFShardsSelectOnlyMatchingInventory");
    rejects([&] { runtime::ArtifactServices::gguf_shards(shard_files[0], {shard_files[0]}); }, "IncompleteShardInventoryRejected");
    rejects([&] { runtime::ArtifactServices::gguf_shards(shard_files[0], {shard_files[0], "model-00000-of-00002.gguf"}); }, "InvalidShardIndexRejected");
    rejects([&] { runtime::ArtifactServices::gguf_shards(shard_files[0], {shard_files[0], "other-00002-of-00002.gguf"}); }, "ForeignShardCannotFillInventory");
    const auto root = scratch / "mount-inventory";
    fs::create_directories(root / "directory");
    std::ofstream(root / "directory" / "first.bin") << "fixture";
    std::ofstream(root / "directory" / "second.bin") << "fixture";
    const auto files = runtime::ArtifactServices::container_mounts((root / "directory").string(), "directory_files", "/mnt/models/tree");
    check(files.size() == 2 && files[0].destination == "/mnt/models/tree/first.bin" && files[1].destination == "/mnt/models/tree/second.bin", "DirectoryMountUsesSortedInventoryFiles");
#ifndef _WIN32
    fs::remove(root / "alias");
    fs::create_directory_symlink(root / "directory", root / "alias");
    const auto alias = runtime::ArtifactServices::container_mounts((root / "alias" / "first.bin").string(), "file", "/mnt/models/file.bin");
    check(alias.size() == 1 && alias[0].source == fs::canonical(root / "directory" / "first.bin").string(), "ApprovedFileResolvesDirectorySymlinks");
    const auto alias_directory = runtime::ArtifactServices::container_mounts((root / "alias").string(), "directory_files", "/mnt/models/alias");
    check(alias_directory.size() == 2 && alias_directory[0].source == fs::canonical(root / "directory" / "first.bin").string(), "ApprovedDirectoryResolvesSymlinkRoot");
    rejects([&] { runtime::ArtifactServices::container_mounts((root / "directory" / "first.bin").string(), "file", "/mnt/models/../escape"); }, "MaterializedDestinationTraversalRejected");
    fs::create_directories(root / "outside");
    std::ofstream(root / "outside" / "secret.bin") << "outside";
    fs::remove(root / "directory" / "escape.bin");
    fs::create_symlink(root / "outside" / "secret.bin", root / "directory" / "escape.bin");
    rejects([&] { runtime::ArtifactServices::container_mounts((root / "directory").string(), "directory_files", "/mnt/models/tree"); }, "SymlinkOutsideInventoryRejected");
    fs::remove(root / "directory" / "escape.bin");
    fs::create_directories(root / "hf" / "blobs");
    fs::create_directories(root / "hf" / "snapshots" / "active");
    std::ofstream(root / "hf" / "blobs" / "blob") << "fake blob";
    fs::remove(root / "hf" / "snapshots" / "active" / "model.bin");
    fs::create_symlink("../../blobs/blob", root / "hf" / "snapshots" / "active" / "model.bin");
    const auto hf = runtime::ArtifactServices::container_mounts((root / "hf" / "snapshots" / "active").string(), "directory_files", "/mnt/models/hf");
    check(hf.size() == 1 && hf[0].source == fs::canonical(root / "hf" / "blobs" / "blob").string() && hf[0].destination == "/mnt/models/hf/model.bin", "HFLinksResolveOnlyToApprovedBlobs");
#endif
    rejects([&] { runtime::ArtifactServices::container_mounts((root / "directory").string(), "recursive", "/mnt/models/tree"); }, "UnknownMountExpansionRejected");
    rejects([&] { runtime::ArtifactServices::container_mounts((root / "directory").string(), "directory_files", "/outside/tree"); }, "MaterializedMountDestinationConfined");
}

void milestone(const Fixture& fixture, const fs::path& scratch) {
    const auto bound = bind(fixture);
    if (!bound.definition) { check(false, "ContainerDefinitionActuallyExecutes"); return; }
    auto entry = ConfiguredBackendEntry::make(bound.definition);
    const auto doc = document();
    const auto owner = reviewed_owner();
    std::vector<BuiltinRegistrationSeed> seeds{{&entry->descriptor, "gufo", {}}};
    const auto parents = CatalogAdmissionBuilder::test_with_seeds(builtins(false, &doc, false, &owner), seeds);
    const auto catalog = build_backend_catalog(parents, {});
    check(catalog.catalog && catalog.stages.executable && catalog.catalog->registrations().size() == 1 &&
        catalog.catalog->registrations()[0].configured_entry, "ContainerEntersExecutableCatalog");
    if (!catalog.catalog) { diagnostics(catalog.diagnostics); return; }
    FakeHost host;
    auto manager = std::make_shared<utils::ContainerManager>(host.host());
    BackendManager backend_manager(manager);
    BackendManagerTestAccess::pins(backend_manager, versions);
    auto* previous_config = RuntimeConfig::global();
    const auto runtime_policy = configured_container_policy(entry->spec, "rocm").value();
    RuntimeConfig mutable_override(Json{{"gufo", {{"rocm_bin", "latest"}}}});
    RuntimeConfig::set_global(&mutable_override);
    rejects([&] { backend_manager.container_pin(runtime_policy); }, "RuntimeConfigOverrideCannotBypassPinValidation");
    RuntimeConfig valid_override(Json{{"gufo", {{"rocm_bin", "override@sha256:" + std::string(64, 'b')}}}});
    RuntimeConfig::set_global(&valid_override);
    check(backend_manager.container_pin(runtime_policy) == "override@sha256:" + std::string(64, 'b'), "RuntimeConfigDigestOverrideResolved");
    RuntimeConfig::set_global(previous_config);
    for (const auto& field : {"offline", "no_fetch_executables"}) {
        Json disabled_values{{"offline", false}, {"no_fetch_executables", false}};
        disabled_values[field] = true;
        RuntimeConfig disabled(disabled_values);
        RuntimeConfig::set_global(&disabled);
        rejects([&] { backend_manager.install_container_image(image); }, std::string("OCIInstallationHonors:") + field);
        RuntimeConfig::set_global(previous_config);
        host.installed = false;
    }
    auto services = std::make_shared<Services>(backend_manager);
    httplib::Server http;
    std::atomic<unsigned> ready{0}, requests{0};
    http.Get("/ready", [&](const auto&, auto& response) { ++ready; response.set_content("{}", "application/json"); });
    Json observed;
    http.Post("/v1/chat/completions", [&](const auto& request, auto& response) {
        observed = Json::parse(request.body); ++requests;
        response.set_content(Json{{"id", "fake-container"}, {"choices", {{{"message", {{"role", "assistant"}, {"content", "container reply"}}}}}}}.dump(), "application/json");
    });
    services->port = http.bind_to_any_port("127.0.0.1");
    if (services->port <= 0) throw std::runtime_error("Cannot bind loopback");
    std::thread listener([&] { http.listen_after_bind(); });
    struct Cleanup { httplib::Server& server; std::thread& thread; ~Cleanup() { server.stop(); if (thread.joinable()) thread.join(); } } cleanup{http, listener};
    http.wait_until_ready();
    ClientState client;
    unsigned native = 0;
    ConfiguredServerServices effects;
    effects.launch = services;
    effects.containers = manager;
    effects.make_process = [&](auto, const auto&) -> std::unique_ptr<ServerProcess> { ++native; throw std::logic_error("Native fallback forbidden"); };
    effects.make_container_client = [&](auto) { return std::make_unique<FakeClient>(client); };
    auto catalog_server = catalog.catalog->registrations()[0].create({"error", nullptr, &backend_manager});
    auto* server = dynamic_cast<ConfiguredServer*>(catalog_server.get());
    check(server != nullptr, "CatalogFactoryCreatesConfiguredContainerServer");
    if (!server) return;
    ConfiguredServerTestAccess::effects(*server, std::move(effects));
    const auto file = scratch / "inventory" / "checkpoint.bin";
    fs::create_directories(file.parent_path());
    std::ofstream(file) << "CPU fixture only";
    ModelInfo model; model.recipe = "gufo"; model.model_name = "fixture"; model.type = ModelType::LLM;
    model.resolved_paths["main"] = file.string();
    RecipeOptions options;
    auto invalid_services = Services(backend_manager);
    invalid_services.port = services->port;
    invalid_services.install = false;
    invalid_services.image_override = "ghcr.io/peonist-ai/gufo:latest";
    DefinitionRunner runner(entry->executable_definition);
    rejects([&] { runner.prepare_load({"fixture", model, options, "error"}, entry->spec, invalid_services); }, "MutableImageRejectedBeforeInstallation");
    invalid_services.image_override.clear();
    invalid_services.system_os = "macos";
    rejects([&] { runner.prepare_load({"fixture", model, options, "error"}, entry->spec, invalid_services); }, "ContainerLaunchRequiresLinux");
    invalid_services.system_os = "linux";
    auto changed_ir = ex::Inspection::program(*fixture.compiled);
    changed_ir.launch.environment_set.push_back({"HOME", "/launch/argv/0/args/0"});
    auto changed_compiled = ex::Inspection::test_program(*fixture.compiled, changed_ir);
    auto changed_bound = bd::Inspection::test_compiled(*bound.definition, changed_compiled);
    auto changed_entry = ConfiguredBackendEntry::make(changed_bound);
    DefinitionRunner protected_runner(changed_bound);
    rejects([&] { protected_runner.prepare_load({"fixture", model, options, "error"}, changed_entry->spec, invalid_services); }, "ProtectedContainerEnvironmentRejected");
    changed_ir = ex::Inspection::program(*fixture.compiled);
    changed_ir.launch.records["readiness"]["timeout"] = {{"source", "global"}, {"floor_seconds", 123}};
    changed_compiled = ex::Inspection::test_program(*fixture.compiled, changed_ir);
    changed_bound = bd::Inspection::test_compiled(*bound.definition, changed_compiled);
    changed_entry = ConfiguredBackendEntry::make(changed_bound);
    DefinitionRunner floor_runner(changed_bound);
    bool floor_ok = false;
    try {
        const auto floor_plan = floor_runner.prepare_load({"fixture", model, options, "error"}, changed_entry->spec, invalid_services);
        floor_ok = floor_plan.plan.readiness_timeout_seconds == 123;
    } catch (const std::exception&) {}
    check(floor_ok, "ContainerReadinessHonorsGlobalFloor");
    bool launched = false;
    try { server->load("fixture", model, options); launched = true; } catch (const std::exception& error) { std::printf("Launch error: %s\n", error.what()); }
    check(launched && client.starts == 1 && client.invocation.program == "podman" && native == 0 && ready > 0, "ContainerDefinitionActuallyExecutes");
    if (!launched) return;
    const auto& argv = client.invocation.args;
    const auto has = [&](const auto& value) { return std::find(argv.begin(), argv.end(), value) != argv.end(); };
    check(has(image) && !has(pin) && has("--pull=never") && has("--cap-drop=all"), "ContainerProcessReceivesImmutableImageAndConfinement");
    check(has(fs::canonical(file).string() + ":/mnt/models/main/checkpoint.bin:ro,z") &&
        has("/mnt/models/main/checkpoint.bin") && !has(file.string()), "InventoryMountAndWholeValueRewrite");
    check(has("/dev/dri") && has("/dev/kfd") && has("127.0.0.1:" + std::to_string(services->port) + ":" + std::to_string(services->port)), "ReviewedDevicesAndLoopbackAddress");
    const Json request{{"model", "caller"}, {"messages", {{{"role", "user"}, {"content", "hello"}}}}};
    const auto response = server->chat_completion(request);
    check(requests == 1 && observed == request && response.at("id") == "fake-container", "InheritedOperationPathServesContainerRequest");
    server->unload();
    const auto name = utils::ContainerManager::container_name("gufo", "rocm", "fixture");
    check(!client.alive && client.stops == 1 && host.ran({"podman", "stop", "--time", "10", name}) &&
        host.ran({"podman", "rm", "-f", name}) && host.ran({"podman", "network", "rm", name}), "ContainerTeardownStopsRemovesAndReaps");
    check(host.ran({"podman", "pull", image}), "OCIInstallUsesDigestOnlyPull");
    std::ofstream(scratch / "transcript.json") << Json{{"program", client.invocation.program}, {"argv", argv},
        {"host_commands", host.commands}, {"readiness_requests", ready.load()}, {"request", observed}, {"response", response},
        {"client_starts", client.starts}, {"client_stops", client.stops}, {"native_processes", native}}.dump(2) << '\n';
}
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        const auto scratch = fs::path(LEMON_CONTAINER_EXECUTION_SCRATCH);
        fs::create_directories(scratch);
        RuntimeConfig config(Json{{"global_timeout", 2}, {"offline", false}, {"no_fetch_executables", false}, {"auto_evict", false}, {"inhibit_suspend", false}, {"log_level", "error"}});
        RuntimeConfig::set_global(&config);
        utils::HttpClient::set_default_timeout(2);
        const auto fixture = authorized();
#ifdef __linux__
        inventory(scratch);
#endif
        authority_and_plans(fixture, scratch);
#ifdef __linux__
        milestone(fixture, scratch);
#endif
        RuntimeConfig::set_global(nullptr);
    } catch (const std::exception& error) { std::printf("Container execution error: %s\n", error.what()); check(false, "ContainerExecutionPipeline"); }
    return finish();
}
