#include "backend_builtin_seeds_internal.h"
#include "backend_catalog_admission_internal.h"
#include "backend_catalog_publication_internal.h"
#include "backend_definition_authorization_internal.h"
#include "lemon/backends/configured/backend_catalog.h"
#include "lemon/backends/configured/configured_server.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/http_client.h"
#include "lemon/wrapped_server.h"
#include "llamacpp_external_contract.h"

#include <httplib.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace lemon;
using namespace lemon::backends;
using Json = nlohmann::json;
namespace {
int checks = 0, failures = 0;
void check(bool condition, const std::string& name) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", name.c_str()); }
}
}

namespace lemon::backends {
struct ConfiguredServerTestAccess {
    static void launch(ConfiguredServer& server, std::shared_ptr<LaunchServices> services) {
        server.services_.launch = std::move(services);
    }
};
}

namespace {
Json read_json(const std::filesystem::path& path) {
    std::ifstream input(path);
    return Json::parse(input);
}
int ephemeral_port() {
    httplib::Server socket;
    socket.new_task_queue = [] { return new httplib::ThreadPool(1); };
    const int port = socket.bind_to_any_port("127.0.0.1");
    if (port <= 0) throw std::runtime_error("Ephemeral loopback bind failed");
    std::thread listener([&] { socket.listen_after_bind(); });
    socket.wait_until_ready();
    socket.stop();
    listener.join();
    return port;
}
void finalization_controls() {
    const auto parent = export_fixture::read_bytes("external/research-llama.json");
    const auto child = export_fixture::read_bytes("external/fork-b.json");
    const std::vector<ExternalDefinitionInput> sources{{parent, ""}, {child, ""}};
    const auto builtins = freeze_builtin_catalog();
    const auto complete = build_backend_catalog(builtins, sources);
    check(bool(complete.catalog) && catalog_publication().state() == CatalogPublication::State::Empty,
        "ExternalCandidateHasZeroEffects:unpublished");
    if (!complete.catalog) return;
    bool canonical = embedded_v2_policies().leaves.size() == 2 && embedded_v2_sources().size() == 2;
    for (const auto& seed : builtin_registration_seeds()) {
        const auto found = std::find_if(complete.catalog->registrations().begin(), complete.catalog->registrations().end(),
            [&](const auto& row) { return row.descriptor == seed.descriptor; });
        canonical = canonical && found != complete.catalog->registrations().end();
    }
    check(canonical, "RealFrozenBuiltinContextPreserved");
    auto collision = Json::parse(parent);
    collision["merge"]["metadata"]["config_section"] = "llamacpp";
    const auto collision_bytes = collision.dump();
    const auto rejected_collision = build_backend_catalog(builtins, {{collision_bytes, ""}});
    check(!rejected_collision.catalog && std::any_of(rejected_collision.diagnostics.entries.begin(), rejected_collision.diagnostics.entries.end(),
        [](const auto& row) { return row.code == "section_collision"; }) && !rejected_collision.stages.catalog_admitted &&
        catalog_publication().state() == CatalogPublication::State::Empty, "RealFrozenBuiltinContextPreserved:builtinReservations");
    for (std::size_t stage = 1; stage < 4; ++stage) {
        CatalogExecutionLimits limits;
        limits.stages[stage].work_visits = complete.metrics.stages[stage].work_visits - 1;
        const auto rejected = build_backend_catalog(builtins, sources, limits);
        const auto& entries = rejected.diagnostics.entries;
        const bool limited = std::any_of(entries.begin(), entries.end(), [](const auto& row) { return row.code == "limit_exceeded"; });
        check(!rejected.catalog && limited && rejected.stages.catalog_admitted &&
            rejected.stages.compiled == (stage > 1) && rejected.stages.bound == (stage > 2) &&
            !rejected.stages.executable && catalog_publication().state() == CatalogPublication::State::Empty,
            "ExternalDescendantFailurePublishesNothing:stage" + std::to_string(stage));
    }
}
void product(const std::filesystem::path& directory) {
    const auto binary = directory / "research-llama-server";
    std::filesystem::copy_file(LEMON_FAKE_ENGINE, binary, std::filesystem::copy_options::overwrite_existing);
    const auto transcript = directory / "process.json";
    const auto model_path = directory / "model.bin";
    std::ofstream(model_path) << "CPU fake artifact; no model";
    RuntimeConfig config(Json{{"research-llama", {{"cpu_bin", binary.string()}}}, {"global_timeout", 2},
        {"auto_evict", false}, {"inhibit_suspend", false}, {"log_level", "error"}});
    auto* previous = RuntimeConfig::global();
    RuntimeConfig::set_global(&config);
    int downloads = 0, managed = 0;
    const int port = ephemeral_port();
    auto server = create_server("research-llama", {"error"});
    auto* selected = dynamic_cast<ConfiguredServer*>(server.get());
    check(selected != nullptr, "ExternalForkDeclarationToProcessExact:factory");
    if (!selected) { RuntimeConfig::set_global(previous); return; }
    auto effects = production_backend_effects(nullptr, [=] { return port; });
    effects.supported_backends = [](const auto&) { return std::vector<std::string>{"cpu"}; };
    effects.validate_backend = [](const auto&, const auto& backend) { if (backend != "cpu") throw std::invalid_argument("not CPU"); };
    effects.binary.install = [&](const auto&, const auto&) { ++downloads; throw std::logic_error("Download forbidden"); };
    effects.binary.managed_path = [&](const auto&, const auto&) { ++managed; return std::string(LEMON_FAKE_ENGINE); };
    ConfiguredServerTestAccess::launch(*selected, std::make_shared<EffectLaunchServices>(effects));
    ModelInfo model;
    model.recipe = "research-llama";
    model.model_name = "external-client-model";
    model.checkpoints["main"] = "test/fake";
    model.resolved_paths["main"] = model_path.string();
    RecipeOptions options("research-llama", {{"ctx_size", 512}, {"research_llama_backend", "cpu"},
        {"research_llama_args", "--test-transcript " + transcript.string()}});
    const auto before_environment = std::getenv("GGML_DISABLE_VULKAN") ? std::string(std::getenv("GGML_DISABLE_VULKAN")) : "";
    utils::HttpClient::set_default_timeout(2);
    server->set_model_metadata("external-client-model", "external", ModelType::LLM, DEVICE_CPU, options);
    server->load("external-client-model", model, options);
    server->set_state(ModelState::READY);
    const Json request{{"model", "client-request-model"}, {"messages", {{{"role", "user"}, {"content", "hello CPU fake"}}}},
        {"max_completion_tokens", 7}};
    const auto response = server->chat_completion(request);
    const auto observed = read_json(transcript);
    std::ofstream(directory / "successful-process.json") << Json{{"process", observed}, {"response", response}}.dump(2) << '\n';
    const std::vector<std::string> expected{"-m", model_path.string(), "--ctx-size", "512", "--port", std::to_string(port), "--jinja", "--metrics", "--test-transcript", transcript.string(),
        "--parallel", "2", "--cache-type-k", "q8_0", "--cache-type-v", "q8_0"};
    check(observed.at("argv") == expected, "ExternalForkDeclarationToProcessExact:argv");
    check(observed.at("path") == binary.string() && observed.at("pid").get<int>() > 0 &&
        observed.at("path") != LEMON_FAKE_ENGINE, "ExternalForkNeverUsesBuiltinExecutable");
    auto expected_request = request;
    expected_request["max_tokens"] = 7;
    const Json expected_response{{"id", "external-fork-cpu-4g-73f6"}, {"model", "client-request-model"},
        {"choices", {{{"message", {{"role", "assistant"}, {"content", "external-fork-cpu-4g-73f6"}}}}}}};
    check(observed.at("requests") == Json::array({{{"path", "/v1/chat/completions"}, {"body", expected_request}}}) &&
        response == expected_response, "ExternalForkInheritedProtocolObserved");
    check(observed.at("environment").at("GGML_DISABLE_VULKAN") == "1" &&
        (std::getenv("GGML_DISABLE_VULKAN") ? std::string(std::getenv("GGML_DISABLE_VULKAN")) : "") == before_environment,
        "ExternalEnvironmentIsChildOnly");
    server->unload();
    check(server->get_backend_port() == 0 && !utils::HttpClient::is_reachable("http://127.0.0.1:" + std::to_string(port) + "/health", 1,
        utils::HttpSecurityPolicy::TrustedLoopback), "ExternalForkTeardownStopsRealProcess");
    const auto attempt = [&](const std::string& name, RecipeOptions attempted, const std::string& message) {
        bool rejected = false;
        try { server->load("external-client-model", model, attempted); }
        catch (const std::exception& error) { rejected = std::string(error.what()).find(message) != std::string::npos; }
        check(rejected && server->get_backend_port() == 0, name);
    };
    auto malformed = options;
    malformed.set_option("research_llama_args", "--model escaped");
    attempt("ExternalForkMalformedArgumentsFailBeforeSpawn", malformed, "Invalid custom llama-server arguments");
    auto readiness = options;
    readiness.set_option("research_llama_args", "--test-transcript " + transcript.string() + " --test-readiness fail");
    attempt("ExternalForkReadinessFailureStopsRealProcess", readiness, "failed");
    std::filesystem::remove(binary);
    attempt("ExternalForkMissingExecutableNeverFallsBack", options, "path");
    RuntimeConfig unconfigured(Json{{"research-llama", {{"cpu_bin", ""}}}});
    RuntimeConfig::set_global(&unconfigured);
    attempt("ExternalForkMissingConfigurationNeverFallsBack", options, "External backend executable is required");
    check(downloads == 0 && managed == 0, "ExternalOnlyNeverDownloads");
    RuntimeConfig::set_global(previous);
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--dump-pins") {
        auto policy = external_fixture::contract();
        export_fixture::repin(policy);
        Json slots = Json::array();
        for (const auto& slot : policy.slots) slots.push_back(slot.mutation_policy_digest);
        std::printf("%s\n", Json{{"policy", policy.policy_digest}, {"owner_graph", policy.owner_graph_digest},
            {"registry", v2::export_registry_digest({policy}, installed_hook_abis())}, {"slots", slots}}.dump(2).c_str());
        return 0;
    }
    try {
        check(installed_builtin_exports().exports().size() == 1 && installed_builtin_exports().digest() ==
            "sha256:d644387f2d2838a76ace64304318e673244e8cd117e496ae4394dc0792e2b9fa", "ProductionExportsMatchQualifiedContract");
        const auto& selected = BuiltinCatalogLoader::selected_exports();
        check(selected.exports().size() == 1 && selected.exports()[0].slots.size() == 17, "All17BaseSlotsCovered");
        finalization_controls();
        const auto directory = std::filesystem::temp_directory_path() / "external-fork";
        std::filesystem::create_directories(directory / "backends.d");
        for (const auto* id : {"research-llama", "fork-b"})
            std::ofstream(directory / "backends.d" / (std::string(id) + ".json")) << export_fixture::read_bytes(std::string("external/") + id + ".json");
        bootstrap_backend_catalog(directory.string());
        const auto* catalog = backend_catalog();
        check(catalog && catalog->registrations().size() == 19, "ExternalForkDeclarationToProcessExact:published");
        if (catalog) for (const auto& row : catalog->registrations()) if (row.configured_entry && row.configured_entry->executable_definition) {
            const auto& token = row.configured_entry->executable_definition->compiled().authorized();
            const auto& key = token.definition().key;
            if (key.id == "research-llama" || key.id == "fork-b") {
                check(key.space == DefinitionNamespace::External, "ExternalLeafNeverBuiltin:" + key.id);
                check(AuthorityInspection::grant_count(token) == (key.id == "research-llama" ? 13u : 10u), "GrantTuplesOnlyAttenuate:" + key.id);
                for (std::size_t i = 0; i < AuthorityInspection::grant_count(token); ++i)
                    check(AuthorityInspection::grant(token, i).base_id == "llamacpp", "GrantTuplesRetainBuiltinRoot:" + key.id);
            }
        }
        if (catalog) {
            auto rejected = nlohmann::json::parse(export_fixture::read_bytes("external/fork-b.json"));
            rejected["merge"]["install"]["pin_recipe"] = "llamacpp";
            const auto result = catalog->validate(rejected.dump());
            check(!result.at("valid").get<bool>() && result.at("stages").at("resolved") == true &&
                result.at("stages").at("authorized") == false && result.at("stages").at("catalog_admitted") == false &&
                std::any_of(result.at("diagnostics").begin(), result.at("diagnostics").end(),
                    [](const auto& row) { return row.at("code") == "invalid_local_executable_profile"; }) &&
                result.at("catalog_id") == catalog->export_schema().at("catalog_id") && backend_catalog() == catalog,
                "RejectedCandidatePreservesGeneration");
            bool replacement = false;
            try { bootstrap_backend_catalog((directory / "replacement").string()); }
            catch (const std::logic_error& error) { replacement = std::string(error.what()) == "Backend catalog is already frozen"; }
            check(replacement && backend_catalog() == catalog, "ExternalPublicationIsOneShot");
        }
        product(directory);
    } catch (const std::exception& error) { std::printf("%s\n", error.what()); check(false, "ExternalForkDeclarationToProcessExact"); }
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
