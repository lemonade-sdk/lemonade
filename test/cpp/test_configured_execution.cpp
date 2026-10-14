#include "configured_fixture.h"
#include "lemon/backends/configured/configured_server.h"
#include "lemon/backends/thinksound/thinksound.h"
#include "lemon/backends/thinksound/thinksound_server.h"
#include "lemon/config_file.h"
#include "lemon/utils/path_utils.h"

#include <cstdlib>
#include <filesystem>
#include <new>

using namespace thinksound_oracle;
using namespace lemon::backends;

namespace {
thread_local bool track_allocations = false;
thread_local size_t allocations = 0;
void set_env(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}
class Access : public WrappedServer {
public:
    Access() : WrappedServer("access", "error") {}
    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override {}
    static void locks(WrappedServer& server, const std::function<void()>& body) {
        std::scoped_lock lock(server.*(&Access::state_mutex_), server.*(&Access::process_mutex_));
        body();
    }
    static void start(WrappedServer& server, std::unique_ptr<ServerProcess> process, const ServerCommand& command, long timeout) {
        (server.*(&Access::start_server))(std::move(process), command, timeout);
    }
    static void bind_again(WrappedServer& server) { (server.*(&Access::bind_operations))(OperationBindings({}, {})); }
    static json post(WrappedServer& server, const json& request) { return (server.*(&Access::forward_request))("/json", request, 5); }
};
std::unique_ptr<WrappedServer> arm(bool configured, EffectsFixture& fixture, const std::string& log_level = "error") {
    if (!configured) return std::make_unique<ThinkSoundServer>(log_level, nullptr, nullptr, fixture.effects());
    return std::make_unique<ConfiguredServer>(entry(k_thinksound_definition, TrustedCompatibility::LegacyNonChatSse), BackendContext{log_level},
        ConfiguredServerServices{std::make_shared<FixtureLaunchServices>(fixture), fixture.effects().make_process});
}
void load(WrappedServer& server) {
    server.set_model_metadata("fixture", "fixture/repo", ModelType::AUDIO_GENERATION, DEVICE_CPU, empty_options());
    server.load("fixture", model(), empty_options(), false);
    server.set_state(ModelState::READY);
}
void inert_query_oracle(WrappedServer& server, EffectsFixture& fixture) {
    const auto before = fixture.events;
    std::mutex mutex;
    std::condition_variable finished;
    bool done = false, correct = true;
    size_t count = 999;
    std::thread reader;
    bool while_locked = false;
    Access::locks(server, [&] {
        reader = std::thread([&] {
            allocations = 0; track_allocations = true;
            for (int i = 0; i < 30000; ++i) {
                correct &= server.supports_operation(BackendOperation::AudioGeneration, OperationDelivery::Buffered, ModelType::AUDIO_GENERATION);
                correct &= !server.supports_operation(BackendOperation::ChatCompletion, OperationDelivery::Buffered, ModelType::LLM);
            }
            track_allocations = false; count = allocations;
            std::lock_guard<std::mutex> lock(mutex); done = true; finished.notify_all();
        });
        std::unique_lock<std::mutex> lock(mutex);
        while_locked = finished.wait_for(lock, std::chrono::seconds(3), [&] { return done; });
    });
    reader.join();
    check(while_locked && correct && count == 0 && fixture.events == before, "60000 inert support queries while both mutexes held");
}
}

namespace lemon {
struct RoutingHelperTestHook {
    static void add(Router& router, std::unique_ptr<WrappedServer> server) {
        std::lock_guard<std::mutex> lock(router.load_mutex_);
        router.loaded_servers_.push_back(std::move(server));
    }
};
}

void* operator new(size_t size) { if (track_allocations) ++allocations; if (void* memory = std::malloc(size ? size : 1)) return memory; throw std::bad_alloc(); }
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }

int main(int argc, char**) {
    if (argc > 1) {
        set_env("LEMONADE_THINKSOUND_CONFIGURED", "0");
        auto legacy = backends::create_server("thinksound", BackendContext{"error"});
        check(dynamic_cast<ThinkSoundServer*>(legacy.get()) != nullptr, "explicit OFF keeps A factory");
        check(!backends::thinksound::spec()->definition && backends::thinksound::spec() == backends::thinksound::legacy_spec(), "explicit OFF keeps A install spec");
        check(backends::thinksound::ops() == backends::thinksound::legacy_ops(), "explicit OFF keeps A artifact ops");
        return failures ? 1 : 0;
    }
    set_env("LEMONADE_THINKSOUND_CONFIGURED", "1");
    set_env("LEMONADE_BACKEND_WATCHDOG_POLL_SECONDS", "1");
    HttpFixture http;
    for (bool configured : {false, true}) {
        http.respond(200, std::string("RIFF\0\xff\r\nWAVE", 12));
        EffectsFixture fixture(http.port);
        {
            auto server = arm(configured, fixture, "debug");
            load(*server);
            check(fixture.events == std::vector<std::string>{u8"model:/model dir/雪\"trailing\\", "supported:thinksound", "validate:thinksound:vulkan", "external:thinksound:vulkan", "install:thinksound:vulkan", "managed:thinksound:vulkan", "port", "process", "start"}, "full A/D launch trace literal");
            check(fixture.command.args == std::vector<std::string>{"--dir", u8"/model dir/雪\"trailing\\", "--host", "127.0.0.1", "--port", std::to_string(http.port)} && fixture.cwd.empty() && fixture.output.inherit && !fixture.output.filter_health_logs, "full A/D argv/output literal");
            wire_oracle(*server, http);
            const auto& descriptions = server->operation_bindings()->descriptions();
            check(descriptions.size() == 4 && descriptions[0].operation == BackendOperation::ChatCompletion && descriptions[1].operation == BackendOperation::Completion && descriptions[2].operation == BackendOperation::Responses && descriptions[3].operation == BackendOperation::AudioGeneration, "A/D binding order matches literal legacy order");
            check(descriptions[0].model_types == 511 && descriptions[0].provenance == OperationProvenance::LegacyForwarding && descriptions[3].model_types == 64 && descriptions[3].delivery == OperationDelivery::Buffered && descriptions[3].provenance == OperationProvenance::Implemented, "A/D compiled provenance and mode masks match independent literals");
            for (const auto operation : {BackendOperation::ChatCompletion, BackendOperation::Completion, BackendOperation::Responses}) {
                const auto response = server->dispatch_completion_operation(operation, json::object());
                const std::map<BackendOperation, std::string> expected = {
                    {BackendOperation::ChatCompletion, R"({"error":{"code":"model_not_applicable","message":"thinksound-server does not support chat completion. Use the appropriate endpoint for this model type instead.","type":"unsupported_operation"}})"},
                    {BackendOperation::Completion, R"({"error":{"code":"model_not_applicable","message":"thinksound-server does not support text completion. Use the appropriate endpoint for this model type instead.","type":"unsupported_operation"}})"},
                    {BackendOperation::Responses, R"({"error":{"code":"model_not_applicable","message":"thinksound-server does not support responses. Use the appropriate endpoint for this model type instead.","type":"unsupported_operation"}})"}
                };
                check(response.dump() == expected.at(operation), "compiled unsupported buffered rejection matches independent literal");
            }
            inert_query_oracle(*server, fixture);
            if (configured) {
                bool rejected = false;
                try { Access::bind_again(*server); } catch (const std::logic_error&) { rejected = true; }
                check(rejected, "second binding installation rejected");
            }
            server->unload();
            check(fixture.stops == 1 && server->get_process_info().pid == 0 && server->get_backend_port() == 0 && server->get_launch_command().empty(), "full A/D unload metadata clear");
        }
        for (const auto* backend : {"rocm", "cuda", "rocm-stable"}) {
            EffectsFixture environment(http.port); environment.external = "/external dir/ts-server";
            auto server = arm(configured, environment);
            server->load("fixture", model(), RecipeOptions("thinksound", {{"thinksound_backend", backend}}), true);
#ifdef _WIN32
            const std::string key = "PATH", rocm = "/runtime/core:/runtime/libs;/external dir;old", cuda = "/external dir;old";
#else
            const std::string key = "LD_LIBRARY_PATH", rocm = "/runtime/core:/runtime/libs:/external dir:old", cuda = "/external dir:old";
#endif
            std::vector<std::pair<std::string, std::string>> expected;
            if (std::string(backend) == "rocm") expected = {{key, rocm}};
            if (std::string(backend) == "cuda") expected = {{key, cuda}, {"CUDA_VISIBLE_DEVICES", "2,0"}, {"__NV_PRIME_RENDER_OFFLOAD", "1"}};
            check(environment.command.env == expected, "full A/D environment literal");
        }
        for (int scenario = 0; scenario < 5; ++scenario) {
            EffectsFixture failure(http.port);
            if (scenario == 0) failure.model_exists = false;
            if (scenario == 1) failure.supported.clear();
            if (scenario == 2) failure.port = 0;
            if (scenario == 3) failure.child_running = false;
            std::atomic<bool> cancel{scenario == 4};
            auto server = arm(configured, failure); server->set_load_cancel_flag(&cancel);
            std::string message;
            try { load(*server); } catch (const std::exception& error) { message = error.what(); }
            if (scenario == 0) check(message == "Model path not found for checkpoint: fixture/repo" && failure.events.size() == 1, "A/D artifact failure precedence");
            if (scenario == 1) check(!message.empty() && failure.events.size() == 2, "A/D backend selection failure precedence");
            if (scenario == 2) check(message == "Failed to find an available port", "A/D port error literal");
            if (scenario >= 3) check(message == "thinksound-server failed to start" && failure.stops == 1 && server->get_process_id() == 0, "A/D cancelled/exited readiness cleanup");
        }
        {
            EffectsFixture timeout(http.port); auto server = arm(configured, timeout);
            http.health_status = 503;
            ServerCommand command; command.program = "fixture"; command.port = http.port;
            std::string message;
            try { Access::start(*server, timeout.effects().make_process({}, ""), command, 1); } catch (const std::exception& error) { message = error.what(); }
            http.health_status = 200;
            check(message == "thinksound-server failed to start" && timeout.stops == 1 && server->get_process_id() == 0, "A/D common readiness timeout cleanup");
        }
        {
            EffectsFixture crash(http.port); auto server = arm(configured, crash); load(*server);
            crash.child_running = false;
            std::unique_lock<std::mutex> lock(crash.stop_mutex);
            const bool stopped = crash.stopped.wait_for(lock, std::chrono::seconds(3), [&] { return crash.stops.load() == 1; });
            check(stopped && server->was_watchdog_triggered() && server->get_process_id() == 0, "A/D idle crash detected by watchdog");
        }
        for (int progress : {0, 1}) {
            EffectsFixture crash(http.port); auto server = arm(configured, crash); load(*server);
            http.break_transfer(progress, [&] { crash.child_running = false; });
            Capture capture; bool retry = false;
            try { dynamic_cast<IAudioGenerationServer*>(server.get())->audio_generations({{"prompt", "rain"}}, capture.sink); }
            catch (const BackendStreamRetryableReset&) { retry = true; }
            check(retry == (progress == 0) && crash.stops == 1 && server->was_watchdog_triggered(), "A/D retry signal only before any bytes");
            if (progress == 0) check(capture.bytes.empty(), "A/D no bytes before retry");
            else check(capture.bytes == R"(PARTdata: {"error":{"details":{"backend":"thinksound-server","code":"backend_watchdog_reset","reason":"backend process exited during streaming request","retryable":true},"message":"thinksound-server was unresponsive and has been reset by the backend watchdog. The model will be reloaded automatically on retry.","type":"backend_error"}})" "\n\n", "A/D partial bytes plus literal reset envelope without replay");
        }
        {
            EffectsFixture rejection(http.port); auto server = arm(configured, rejection); load(*server);
            http.respond(200, "payload"); Capture capture; capture.sink.write = [](const char*, size_t) { return false; };
            dynamic_cast<IAudioGenerationServer*>(server.get())->audio_generations({{"prompt", "rain"}}, capture.sink);
            check(rejection.stops == 0 && !server->was_watchdog_triggered() && capture.done == 1 && server->get_backend_health_state() == "ready", "A/D sink rejection releases byte request without reset");
        }
        {
            EffectsFixture cleanup(http.port);
            { auto server = arm(configured, cleanup); load(*server); }
            check(cleanup.stops == 1, "A/D destructor stops fake process");
        }
    }
    const auto root = std::filesystem::temp_directory_path() / ("configured-public-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    utils::set_cache_dir(root.string()); utils::set_config_dir(root.string());
    const std::string previous_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    set_env("PATH", root.string().c_str());
    set_env("LEMONADE_FLM_NPU_BIN", "");
    for (bool configured : {false, true}) {
        auto configuration = ConfigFile::base_defaults();
        configuration.update(json{{"offline", true}, {"disable_model_filtering", true},
            {"no_fetch_executables", true}, {"auto_evict", false}, {"auto_check_model_updates", false}, {"inhibit_suspend", false}, {"log_level", "error"}});
        const auto config = std::make_shared<RuntimeConfig>(configuration);
        RuntimeConfig::set_global(config.get());
        {
            Server frontend(config, root.string(), root.string());
            auto& models = MediaResponseTestHook::models(frontend);
            models.register_user_model("user.fixture", {{"recipe", "thinksound"}, {"checkpoint", "fixture/repo"}});
            models.register_user_model("user.wrong", {{"recipe", "llamacpp"}, {"checkpoint", "fixture/repo:wrong.gguf"}});
            EffectsFixture fixture(http.port); auto server = arm(configured, fixture); load(*server);
            server->set_model_metadata("user.fixture", "fixture/repo", ModelType::AUDIO_GENERATION, DEVICE_CPU, empty_options());
            WrappedServer* resident = server.get();
            RoutingHelperTestHook::add(MediaResponseTestHook::router(frontend), std::move(server));
            const std::vector<std::pair<std::string, std::string>> invalid = {
                {R"({"prompt":"rain"})", R"({"error":{"message":"Missing 'model' field in request","type":"invalid_request_error"}})"},
                {R"({"model":"user.fixture"})", R"({"error":{"message":"Missing 'prompt' field in request","type":"invalid_request_error"}})"},
                {R"({"model":"user.fixture","prompt":0})", R"({"error":{"message":"'prompt' must be a string","type":"invalid_request_error"}})"},
                {R"({"model":"user.wrong","prompt":"rain"})", R"({"error":{"code":"model_not_applicable","message":"Model 'user.wrong' is not an audio-generation model","type":"invalid_request_error"}})"},
                {R"({"model":"user.fixture","prompt":"rain","response_format":"mp3"})", R"fixture({"error":{"message":"response_format 'mp3' is not supported by this model (supported: wav)","type":"invalid_request_error"}})fixture"}
            };
            for (const auto& [body, expected] : invalid) {
                httplib::Request request; request.body = body; httplib::Response response; response.status = 200;
                MediaResponseTestHook::generate(frontend, request, response);
                check(response.status == 400 && response.body == expected, "A/D public endpoint validation error literal");
            }
            http.respond(200, std::string("RIFF\0\xff\r\nWAVE", 12)); http.delay();
            httplib::Request request; request.body = R"({"model":"user.fixture","prompt":"rain"})";
            httplib::Response response; response.status = 200;
            std::thread worker([&] { MediaResponseTestHook::generate(frontend, request, response); });
            const bool entered = http.wait_for_request();
            check(entered && resident->is_busy() && resident->get_backend_health_state() == "busy", "A/D real router admission held while public buffered audio waits for first byte");
            http.unblock(); worker.join();
            check(response.status == 200 && response.body == std::string("RIFF\0\xff\r\nWAVE", 12) && response.get_header_value("Content-Type") == "audio/wav" && !resident->is_busy() && resident->get_backend_health_state() == "ready", "A/D public bytes MIME and router release");
            for (const auto& [status, bytes, expected_status, expected] : std::vector<std::tuple<int, std::string, int, std::string>>{
                {200, "", 502, R"({"error":{"message":"Generation failed: the backend produced no output (it likely crashed or ran out of GPU memory). Check the server logs.","type":"backend_error"}})"},
                {400, "plain", 400, R"({"error":{"message":"plain","status_code":400,"type":"backend_error"}})"}}) {
                http.respond(status, bytes); httplib::Response error; error.status = 200;
                MediaResponseTestHook::generate(frontend, request, error);
                check(error.status == expected_status && error.body == expected && !resident->is_busy(), "A/D public error literal releases model acquisition");
            }
            MediaResponseTestHook::router(frontend).unload_model("user.fixture");
            check(fixture.stops == 1, "A/D real router unload stops fake process");
        }
        RuntimeConfig::set_global(nullptr);
    }
    set_env("PATH", previous_path.c_str());
    std::filesystem::remove_all(root);
    const auto definition_entry = entry(k_thinksound_definition, TrustedCompatibility::LegacyNonChatSse);
    const json expected_defaults = {{"backend", "auto"}, {"vulkan_bin", "builtin"}, {"rocm_bin", "builtin"}, {"cuda_bin", "builtin"}};
    check(definition_entry->descriptor.config_defaults() == expected_defaults && backends::thinksound::descriptor.config_defaults() == expected_defaults, "A/D config defaults match independent literal");
    for (const auto* descriptor : {&definition_entry->descriptor, &backends::thinksound::descriptor}) {
        check(descriptor->recipe == "thinksound" && descriptor->display_name == "ThinkSound" && descriptor->binary == "ts-server" && descriptor->config_section.empty() && descriptor->default_device == DEVICE_GPU && descriptor->slot_policy == SlotPolicy::Standard && descriptor->selectable_backend && !descriptor->uses_ctx_size && !descriptor->dynamic_models && descriptor->supported_modes == std::vector<std::string>{"audio-generation"} && descriptor->rocm_channels == std::vector<std::string>{"stable"}, "A/D descriptor identity literals");
        check(descriptor->options.size() == 1 && descriptor->options[0].name == "thinksound_backend" && descriptor->options[0].cli_flag == "--thinksound" && descriptor->options[0].default_value == "" && descriptor->options[0].type_name == "BACKEND" && descriptor->options[0].help == "ThinkSound backend to use" && descriptor->options[0].group == "Audio Generation Options", "A/D option and CLI metadata literals");
    }
    auto selected = backends::create_server("thinksound", BackendContext{"error"});
    check(dynamic_cast<ConfiguredServer*>(selected.get()) && backends::thinksound::spec()->definition && dynamic_cast<const ConfiguredBackendOps*>(backends::thinksound::ops()), "explicit opt-in activates configured factory/spec/ops together");
    set_env("LEMONADE_THINKSOUND_CONFIGURED", "0");
    check(dynamic_cast<ConfiguredServer*>(backends::create_server("thinksound", BackendContext{"error"}).get()) != nullptr, "activation is frozen after first use");
    auto external_doc = json::parse(k_thinksound_definition); external_doc["metadata"]["options"][0]["cli_flag"] = "";
    const auto external = BackendDefinitionStore().validate({"external.json", external_doc.dump(), DefinitionOrigin::External});
    bool rejected = false;
    try { ConfiguredBackendEntry bad(external.definition, TrustedCompatibility::LegacyNonChatSse); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "external definition cannot receive trusted legacy SSE attachment");
    auto json_doc = json::parse(k_thinksound_definition);
    json_doc["id"] = "ordinary-fixture"; json_doc["recipe"] = "ordinary-fixture";
    json_doc["metadata"]["supported_modes"] = {"chat"};
    json_doc["metadata"]["display_name"] = "Ordinary Fixture";
    json_doc["metadata"]["server_name"] = "ordinary-server";
    json_doc["metadata"]["log_category"] = "ordinary-server";
    json_doc["metadata"]["binary"] = "ordinary-bin";
    json_doc["install"]["install_recipe"] = "ordinary-install";
    json_doc["install"]["pin_recipe"] = "ordinary-pin";
    json_doc["metadata"]["options"] = json::array({json{{"name", "fixture_value"}, {"value_type", "integer"}, {"default_value", 2}}});
    json_doc["launch"]["backend"] = {{"fixed", "vulkan"}};
    auto& operation = json_doc["operations"][0]; operation["operation"] = "chat_completion"; operation["model_modes"] = {"chat"};
    operation["transport"] = {{"kind", "http_json"}, {"method", "POST"}, {"path", "/json"}, {"port", "http"}, {"timeout", 5}, {"response_codec", "json.v1"}};
    operation["request"] = {{"base", "identity"}, {"set", {{"/fixed", {{"ref", "/options/fixture_value"}}}}}};
    const auto ordinary_entry = entry(json_doc.dump());
    for (int i = 0; i < 3; ++i) {
        auto audio_instance = definition_entry->factory(BackendContext{"error"});
        auto json_instance = ordinary_entry->factory(BackendContext{"error"});
        check(audio_instance->supports_operation(BackendOperation::AudioGeneration, OperationDelivery::Buffered, ModelType::AUDIO_GENERATION) && !audio_instance->supports_operation(BackendOperation::ChatCompletion, OperationDelivery::Buffered, ModelType::LLM), "entry-owned audio factory remains isolated");
        check(json_instance->supports_operation(BackendOperation::ChatCompletion, OperationDelivery::Buffered, ModelType::LLM) && !json_instance->supports_operation(BackendOperation::AudioGeneration, OperationDelivery::Buffered, ModelType::AUDIO_GENERATION), "entry-owned JSON factory remains isolated");
    }
    BackendCreateFn expired;
    { auto temporary = entry(json_doc.dump()); expired = temporary->factory; }
    bool expired_rejected = false;
    try { expired(BackendContext{"error"}); } catch (const std::logic_error&) { expired_rejected = true; }
    check(expired_rejected, "copied factory cannot use a destroyed entry");
    EffectsFixture json_effects(http.port);
    ConfiguredServer generic(entry(json_doc.dump()), BackendContext{"error"}, {std::make_shared<FixtureLaunchServices>(json_effects), json_effects.effects().make_process});
    load(generic);
    generic.set_model_metadata("fixture", "fixture/repo", ModelType::LLM, DEVICE_CPU, empty_options());
    check(generic.chat_completion({{"literal", 0}}) == json{{"witness", true}} && http.last_path() == "/json" && http.last_body() == R"({"fixed":2,"literal":0})", "same generic class executes independently declared JSON operation");
    for (bool configured : {false, true}) {
        EffectsFixture fixture(http.port);
        std::unique_ptr<WrappedServer> control;
        if (!configured) { control = arm(false, fixture); load(*control); }
        WrappedServer& server = configured ? static_cast<WrappedServer&>(generic) : *control;
        http.delay(); json response;
        std::thread worker([&] { response = configured ? generic.chat_completion({{"literal", 0}}) : Access::post(server, {{"fixed", 2}, {"literal", 0}}); });
        const bool entered = http.wait_for_request();
        check(entered && server.get_backend_health_state() == "busy" && !server.was_watchdog_triggered(), "A control/D configured JSON buffered transfer active");
        http.unblock(); worker.join();
        check(response == json{{"witness", true}} && server.get_backend_health_state() == "ready" && http.last_body() == R"({"fixed":2,"literal":0})", "A control/D configured JSON response and accounting released");
    }
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
