#include "backend_definition_binding_fixture.h"
#include "backend_definition_runner_internal.h"
#include "configured_v2_backend_ops_internal.h"
#include "container_production_capabilities.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/configured/configured_server.h"
#include "lemon/backends/configured/container_execution.h"
#include "lemon/backends/llamacpp/llamacpp_request.h"
#include "lemon/utils/json_utils.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace binding_fixture;
using namespace lemon;
namespace fs = std::filesystem;

namespace lemon::backends {
struct ConfiguredServerTestAccess {
    static void effects(ConfiguredServer& server, ConfiguredServerServices services) { server.services_ = std::move(services); }
};
}

namespace {
Json load_json(const fs::path& path) { std::ifstream input(path); return Json::parse(input); }
Fixture fixed_fixture(const Json& doc) {
    const auto id = doc.at("id").get<std::string>();
    const auto& owner = reviewed_container_execution_owner(id);
    const auto admitted = admit({}, builtins(false, &doc, false, &owner));
    if (!admitted.catalog) { diagnostics(admitted.diagnostics); throw std::logic_error("Production admission failed"); }
    const auto token = admitted.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, id, 1});
    const auto compiled = compile_execution_definition(token);
    if (!compiled.definition) { diagnostics(compiled.diagnostics); throw std::logic_error("Production compilation failed"); }
    return {admitted.catalog, token, compiled.definition};
}
struct Services final : LaunchServices {
    int port = 1;
    bool install_failure = false;
    double pool = 62;
    Json pins;
    std::vector<std::string> events;
    bool exists(const std::string& path) const override { return fs::exists(path); }
    std::vector<std::string> supported_backends(const std::string&) const override { return {}; }
    void validate_backend(const std::string&, const std::string&) const override {}
    std::string external_binary(const BackendSpec&, const std::string&) const override { throw std::logic_error("Native forbidden"); }
    void install_backend(const std::string&, const std::string&) override { throw std::logic_error("Native forbidden"); }
    std::string managed_binary(const BackendSpec&, const std::string&) const override { throw std::logic_error("Native forbidden"); }
    int allocate_port() override { events.push_back("port"); return port; }
    int find_free_port(int) override { return port; }
    std::vector<std::pair<std::string, std::string>> environment(const std::string&, const std::string&, const std::string&, const std::string&) const override { return {}; }
    PlatformInfo platform() const override { return {"linux", "x86_64", "gfx1151"}; }
    long default_timeout_seconds() const override { return 2; }
    std::string container_image(const ContainerPolicy& policy) const override {
        const auto pin = effective_container_pin(policy, pins, "");
        return policy.repository + pin.substr(pin.find('@'));
    }
    void install_container_image(const std::string&) override { events.push_back("install"); if (install_failure) throw std::runtime_error("Fake install failure"); }
    std::vector<ContainerGpuNode> container_gpu_nodes() const override { return {{2, "gfx1151"}}; }
    std::string container_gpu_properties(unsigned) const override { return "capability 134217728\nctl_stack_size 1024\nsimd_count 5\ncwsr_size 959488\n"; }
    bool container_supports_arch(const std::string&, const std::string&, const std::string&) const override { return true; }
    IntegratedMemoryPool integrated_memory_pool() const override { return {true, 8.0, pool}; }
    FreeMemoryFact free_gpu_memory() const override { return {true, 120.0}; }
    std::optional<ArtifactByteFact> artifact_bytes(const std::string& path) const override {
        return ArtifactByteFact{true, (path.find("draft") == std::string::npos ? 60ULL : 4ULL) * 1073741824ULL};
    }
};
utils::ContainerHost fake_host() {
    utils::ContainerHost host;
    host.on_path = [](const auto& name) { return name == "podman" || name == "getsubids"; };
    host.gpu_devices_accessible = [] { return true; };
    host.group_id = [](const auto&) { return "109"; };
    host.account = [] { return "fixture"; };
    host.env = [](const auto&) { return ""; };
    host.is_root = [] { return false; };
    host.read_file = [](const auto&) { return ""; };
    host.run = [](const auto& argv, int, bool) {
        if (argv[0] == "getsubids") return utils::CommandResult{0, "0: fixture 100000 65536"};
        if (argv[1] == "info") return utils::CommandResult{0, "true netavark"};
        return utils::CommandResult{0, ""};
    };
    host.stream = [](const auto&, int, const auto&) -> int { throw std::logic_error("Pull forbidden"); };
    host.canonical = [](const auto& path) { return fs::canonical(path).string(); };
    host.file_kind = [](const auto& path) { return fs::is_directory(path) ? utils::ContainerFileKind::Directory : utils::ContainerFileKind::File; };
    host.files = [](const auto&) { return std::vector<std::string>{}; };
    host.sleep = [](auto) {};
    return host;
}
struct ClientState { ServerCommand invocation; unsigned starts = 0, stops = 0; bool alive = false; };
class FakeClient final : public ServerProcess {
public:
    explicit FakeClient(ClientState& state) : ServerProcess({}), state_(state) {}
    std::string start(const ServerCommand& command) override { state_.invocation = command; ++state_.starts; state_.alive = true; return "127.0.0.1"; }
    void stop() override { if (state_.alive) ++state_.stops; state_.alive = false; }
    bool running() const override { return state_.alive; }
private:
    ClientState& state_;
};
struct Logs {
    explicit Logs(std::vector<std::string>& lines) {
        AixLog::Log::init<AixLog::SinkCallback>(AixLog::Severity::trace, [&lines](const auto& metadata, const std::string& message) {
            if (metadata.tag.text == "RocmFpx" || metadata.tag.text == "LlamaCpp" || metadata.tag.text == "Halogen" || metadata.tag.text == "DS4")
                lines.push_back(AixLog::to_string(metadata.severity) + " " + metadata.tag.text + " " + message);
        });
    }
    ~Logs() { AixLog::Log::init({}); }
};
std::string replace(std::string text, const std::string& from, const std::string& to) {
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) { text.replace(at, from.size(), to); at += to.size(); }
    return text;
}
std::vector<std::string> expected_logs(const Json& source, const std::string& main, int port, const std::string& image, const RecipeOptions& options, const std::string& recipe) {
    std::vector<std::string> result;
    for (const auto& row : source.at("expected")) {
        auto value = replace(row.at("line"), "@MAIN@", main);
        value = replace(value, "@PORT@", std::to_string(port));
        if (source.at("engine") == "ds4") value = replace(value, "ghcr.io/peonist-ai/gufo@sha256:" + std::string(64, 'a'), image);
        if (value.rfind("Per-model settings: ", 0) == 0) value = "Per-model settings: " + options.to_log_string();
        if (value.rfind("Adding custom arguments: ", 0) == 0) value = "Adding custom arguments: " + options.get_option(recipe + "_args").get<std::string>();
        std::istringstream input(value); std::string line;
        while (std::getline(input, line)) result.push_back(std::string(row.at("level") == "info" ? "Info " : "Debug ") + row.at("category").get<std::string>() + " " + line);
    }
    return result;
}
void parity(const std::string& id) {
    const auto root = fs::path(LEMON_TEST_FIXTURES).parent_path().parent_path();
    const auto expected = load_json(root / "test/fixtures/backend-v2" / (id + ".production-parity.json"));
    const auto source = Json::parse(read_bytes(expected.at("c7b_fixture")));
    const auto* override_path = std::getenv("LEMON_CONTAINER_PRODUCTION_CANDIDATE");
    const auto doc = load_json(override_path ? fs::path(override_path) : root / "src/cpp/resources/backends" / (id + ".json"));
    const auto& frozen = expected.at("production_definition").at("merge");
    const auto& data = doc.at("merge");
    for (const auto* field : {"argv", "mounts", "readiness", "diagnostics"})
        check(data.at("launch").at(field) == frozen.at("launch").at(field), "C8Frozen" + std::string(field) + ":" + id);
    check(data.at("containers") == frozen.at("containers"), "C8FrozenContainerPolicy:" + id);
    check(data.at("operations") == frozen.at("operations"), "C8FrozenRewriteAndProtocols:" + id);
    check(data.at("metadata") == expected.at("metadata"), "C8FrozenCapabilityAndMetadata:" + id);
    check(data.at("launch").at("diagnostics") == source.at("definition").at("merge").at("launch").at("diagnostics"), "C8ExactC7bDiagnosticRecords:" + id);
    if (doc != expected.at("production_definition")) { check(false, "C8FrozenProductionDefinition:" + id); return; }
    const auto fixture = fixed_fixture(doc);
    const auto bound = bind(fixture);
    check(bool(bound.definition), "C8FixedGrantBinds:" + id);
    if (!bound.definition) { diagnostics(bound.diagnostics); return; }
    const auto entry = ConfiguredBackendEntry::make(bound.definition);
    const auto& owner = reviewed_container_execution_owner(id);
    check(owner.digest == expected.at("owner_digest") && owner.digest == v2::identity("lemonade.backend-container-owner.v1\n", owner.semantics), "C8FixedOwnerDigest:" + id);
    const auto stripped = AuthorityInspection::test_without_grants(*fixture.token);
    check(AuthorityInspection::grant_count(*stripped) == 0 && AuthorityInspection::permits_execution(*stripped, fixture.compiled->normalized_declaration()), "C8OwnerIndependentOfHooks:" + id);
    auto foreign = fixture.compiled->normalized_declaration();
    foreign["containers"][expected.at("backend").get<std::string>()]["repository"] = "docker.io/kyuz0/foreign";
    check(!AuthorityInspection::permits_execution(*stripped, foreign), "C8OwnerCannotWidenRepository:" + id);
    foreign = fixture.compiled->normalized_declaration(); foreign["launch"]["diagnostics"].erase(0);
    check(!AuthorityInspection::permits_execution(*stripped, foreign), "C8OwnerCannotRemoveDiagnostics:" + id);
    const auto active = admit({}, freeze_builtin_catalog());
    check(active.catalog && !active.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, id, 1}) &&
        embedded_v2_policies().leaves.size() == 2 && embedded_v2_policies().execution_owners.empty(), "C8InactiveProductionLeaf:" + id);
    const auto recipe = expected.at("recipe").get<std::string>();
    auto services = std::make_shared<Services>(); services->pins = load_json(root / "src/cpp/resources/backend_versions.json");
    check(services->pins.at(recipe).at(expected.at("backend").get<std::string>()) == expected.at("pin") && !expected.at("registry_validated").get<bool>(), "C8ExactUnvalidatedStackPin:" + id);
    const auto registry = load_json(root / "src/cpp/resources/server_models.json");
    for (const auto& [name, row] : expected.at("models").items()) check(registry.at(name) == row, "C8ExactRegistry:" + id + ":" + name);
    const auto scratch = fs::path(LEMON_C8_SCRATCH) / id;
    fs::create_directories(scratch / "tokenizer");
    for (const auto& name : {"main.fake", "table.hgn", "tokenizer/tokenizer.json"}) std::ofstream(scratch / name) << "fake";
    ModelInfo model; model.recipe = recipe; model.model_name = "fixture"; model.type = ModelType::LLM;
    model.resolved_paths["main"] = (scratch / "main.fake").string(); model.extras["halogen_lookup_table"] = "table.hgn";
    RecipeOptions options(recipe, Json::object()); options.set_option(recipe + "_backend", ""); options.set_option("ctx_size", 8192);
    if (expected.at("engine") != "halogen") options.set_option(recipe + "_args", "--fixture 7");
    DefinitionRunner runner(bound.definition);
    runner.resolve_runtime_options(model, options);
    httplib::Server http; std::string wire_path, wire_body;
    http.Get("/.*", [](const auto&, auto& response) { response.set_content("{}", "application/json"); });
    http.Post("/.*", [&](const auto& request, auto& response) { wire_path = request.target; wire_body = request.body; response.set_content("{}", "application/json"); });
    services->port = http.bind_to_any_port("127.0.0.1");
    if (services->port <= 0) throw std::runtime_error("CPU loopback bind failed");
    std::thread listener([&] { http.listen_after_bind(); });
    struct Cleanup { httplib::Server& server; std::thread& thread; ~Cleanup() { server.stop(); thread.join(); } } cleanup{http, listener};
    http.wait_until_ready();
    ClientState state;
    ConfiguredServerServices effects; effects.launch = services; effects.containers = std::make_shared<utils::ContainerManager>(fake_host());
    effects.make_process = [](auto, const auto&) -> std::unique_ptr<ServerProcess> { throw std::logic_error("Native forbidden"); };
    effects.make_container_client = [&](auto) { return std::make_unique<FakeClient>(state); };
    const std::vector<BuiltinRegistrationSeed> seeds{{&entry->descriptor, id, {}}};
    const auto parents = CatalogAdmissionBuilder::test_with_seeds(builtins(false, &doc, false, &owner), seeds);
    const auto isolated = build_backend_catalog(parents, {});
    check(isolated.catalog && isolated.catalog->registrations().size() == 1, "C8IsolatedCatalogFactory:" + id);
    if (!isolated.catalog) { diagnostics(isolated.diagnostics); return; }
    auto instance = isolated.catalog->registrations()[0].create({"error", nullptr, nullptr});
    auto* server = dynamic_cast<ConfiguredServer*>(instance.get());
    check(server != nullptr, "C8FactoryConfiguredServer:" + id); if (!server) return;
    ConfiguredServerTestAccess::effects(*server, effects);
    std::vector<std::string> lines; Logs capture(lines);
    ConfiguredServer traced(entry, {"trace", nullptr, nullptr}, effects);
    traced.load("fixture", model, options);
    const auto image = services->container_image(entry->descriptor.containers.at(expected.at("backend").get<std::string>()));
    check(lines == expected_logs(source, model.resolved_path("main"), services->port, image, options, recipe), "C8ExactOrderedC7bDiagnostics:" + id);
    traced.unload();
    const auto prepared = runner.prepare_load({"fixture", model, options, "error"}, entry->spec, *services);
    auto argv = expected.at("argv").get<std::vector<std::string>>();
    for (auto& arg : argv) arg = replace(arg, "@PORT@", std::to_string(services->port));
    check(prepared.plan.command.args == argv, "C8ExactSourceArgv:" + id);
    Json mounts = Json::array(); for (const auto& mount : prepared.plan.container->mounts) mounts.push_back(mount.destination);
    check(mounts == expected.at("mounts"), "C8ExactSourceMounts:" + id);
    check(prepared.plan.container->devices == data.at("containers").begin().value().at("devices").get<std::vector<std::string>>() && prepared.plan.container->image == image, "C8ExactDevicesAndImage:" + id);
    server->load("fixture", model, options);
    for (const auto& op : data.at("operations")) {
        if (op.at("delivery") != "buffered") continue;
        Json request{{"model", "caller"}, {"messages", Json::array({{{"role", "user"}, {"content", "/no_think\nquestion"}}})}};
        const auto operation = op.at("operation");
        if (operation == "chat_completion") server->chat_completion(request);
        else if (operation == "completion") server->completion(request);
        else if (operation == "responses") server->responses(request);
        else continue;
        check(wire_path == op.at("transport").at("path"), "C8BufferedProtocolPath:" + id);
    }
    ExecutionSession session{bound.definition};
    for (const auto* path : {"/v1/chat/completions", "/v1/chat/completions?probe=1", "/other/chat/completions-suffix", "/arbitrary?x=chat/completions", "/v1/responses", "/unrelated"}) {
        for (const auto* body : {"{broken", "{ \"messages\":[{\"role\":\"user\",\"content\":\"/no_think\\nquestion\"}],\"max_completion_tokens\":7}"}) {
            std::string expected_body = body;
            auto parsed = Json::parse(body, nullptr, false);
            const bool ds4 = expected.at("engine") == "ds4";
            if (!parsed.is_discarded() && ds4 && std::string(path).find("chat/completions") != std::string::npos) {
                parsed["messages"][0]["content"] = "question"; expected_body = parsed.dump();
            } else if (!parsed.is_discarded() && !ds4 && expected.at("engine") != "halogen" &&
                (std::string(path) == "/v1/chat/completions" || std::string(path) == "/v1/responses")) {
                if (std::string(path) == "/v1/chat/completions") utils::JsonUtils::add_legacy_max_tokens_alias(parsed);
                expected_body = backends::llamacpp::sanitize_tool_schema_limits(std::move(parsed)).dump();
            }
            const auto mapped = runner.map_stream(path, ModelType::LLM, session, body, false, 37);
            httplib::Client client("127.0.0.1", services->port);
            client.set_connection_timeout(2, 0); client.set_read_timeout(2, 0);
            const auto response = client.Post(mapped.path, mapped.original_body.value_or(mapped.body.dump()), "application/json");
            check(response && wire_path == path && wire_body == expected_body && !mapped.sse && mapped.timeout_seconds == 37, "C8ExactRewriteWire:" + id + path);
        }
    }
    server->unload();
    check(state.starts == 2 && state.stops == 2 && !state.alive && state.invocation.program == "podman", "C8FakeContainerLifecycle:" + id);
    services->install_failure = true; lines.clear(); bool refused = false;
    try { traced.load("fixture", model, options); } catch (const std::exception&) { refused = true; }
    check(refused && state.starts == 2, "C8InstallFailureCannotSpawn:" + id);
    const auto mask = container_production_capability(id);
    check(mask == expected.at("capability_mask").get<uint32_t>(), "C8GeneratedCapabilityMask:" + id);
    std::ofstream(scratch / "parity-transcript.json") << Json{{"argv", prepared.plan.command.args}, {"mounts", mounts}, {"runtime_argv", state.invocation.args}}.dump(2) << '\n';
}
void blocked_engine(const std::string& name) {
    const auto fixture = Json::parse(read_bytes("c8." + name + ".blocked.json"));
    const auto doc = name == "ds4" ? fixture.at("definition") : fixture.at("production_definition");
    const auto id = doc.at("id").get<std::string>();
    auto declaration = doc.at("merge"); declaration["recipe"] = doc.at("recipe");
    const auto semantics = container_execution_owner(declaration);
    const BuiltinContainerExecutionOwner owner{{id, 1}, semantics, v2::identity("lemonade.backend-container-owner.v1\n", semantics)};
    const auto admitted = admit({}, builtins(false, &doc, false, &owner));
    check(bool(admitted.catalog), "C8BlockedProbeAdmits:" + name); if (!admitted.catalog) return;
    const auto token = admitted.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, id, 1});
    const auto compiled = compile_execution_definition(token);
    check(bool(compiled.definition), "C8BlockedProbeCompiles:" + name); if (!compiled.definition) { diagnostics(compiled.diagnostics); return; }
    const auto bound = bind_execution_definition(admitted.catalog, compiled.definition);
    check(bool(bound.definition), "C8BlockedProbeBinds:" + name); if (!bound.definition) return;
    const auto scratch = fs::path(LEMON_C8_SCRATCH) / ("blocked-" + name); fs::create_directories(scratch);
    if (name == "ds4") {
        std::string error;
        try { (void)ConfiguredBackendEntry::make(bound.definition); } catch (const std::exception& value) { error = value.what(); }
        check(error == "Configured v2 artifact consumer is unavailable", "C8Ds4AbsoluteFallbackUnavailable");
        std::ofstream(scratch / "blocker.json") << Json{{"error", error}, {"source_absolute_fallback", true}, {"runtime_binding_available", false}}.dump(2) << '\n';
        return;
    }
    const auto entry = ConfiguredBackendEntry::make(bound.definition);
    Services services; services.install_failure = true;
    services.pins = {{"llamacpp", {{name, fixture.at("pin")}}}};
    ModelInfo model; model.recipe = "llamacpp"; model.model_name = "fixture"; model.type = ModelType::LLM;
    const auto file = scratch / "main.fake"; std::ofstream(file) << "fake"; model.resolved_paths["main"] = file.string(); model.extras["hf_load"] = true;
    RecipeOptions options("llamacpp", Json::object()); options.set_option("llamacpp_backend", ""); options.set_option("llamacpp_args", ""); options.set_option("ctx_size", 8192);
    std::string error;
    try { DefinitionRunner(bound.definition).prepare_load({"fixture", model, options, "error"}, entry->spec, services); }
    catch (const std::exception& value) { error = value.what(); }
    check(error == "Fake install failure" && services.events == std::vector<std::string>{"install"}, "C8EarlyHfRefusalUnavailable:" + name);
    const auto expected_error = "Model 'fixture' has llama-server download its own weights, which llamacpp:" + name + " cannot do from its container's private network. Load it on a llamacpp backend that runs on the host.";
    check(error != expected_error, "C8SourceRefusalErrorDiffers:" + name);
    std::ofstream(scratch / "blocker.json") << Json{{"observed_error", error}, {"source_expected_error", expected_error}, {"events", services.events}}.dump(2) << '\n';
}
#include "c8a_contracts.h"
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); allow_entries = true;
    try {
        if (argc == 2 && std::string(argv[1]) == "--pre-install") { c8a_pre_install(); return finish(); }
        if (argc == 3 && std::string(argv[1]) == "--blocked") { blocked_engine(argv[2]); return finish(); }
        if (argc != 2) throw std::invalid_argument("Expected a production container identity");
        parity(argv[1]);
    } catch (const std::exception& error) { std::printf("C8 exception: %s\n", error.what()); check(false, "C8ProductionPipeline"); }
    return finish();
}
