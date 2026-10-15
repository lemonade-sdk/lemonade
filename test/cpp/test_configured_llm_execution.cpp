#include "llm_configured_fixture.h"
#include "lemon/backends/ds4/ds4_server.h"

using namespace llm_oracle;

namespace lemon {
struct RoutingHelperTestHook {
    static void add(Router& router, std::unique_ptr<WrappedServer> server) {
        std::lock_guard<std::mutex> lock(router.load_mutex_); router.loaded_servers_.push_back(std::move(server));
    }
    static ModelTelemetryIdentity identity(Router& router, WrappedServer* server) { return router.get_telemetry_identity(server); }
};
}
namespace {
void set_env(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}
std::unique_ptr<WrappedServer> arm(bool configured, Fixture& fixture) {
    if (!configured) return std::make_unique<Ds4Server>("error", nullptr, nullptr, fixture.effects());
    return std::make_unique<ConfiguredServer>(definition_entry(k_ds4_definition, {DEVICE_GPU, true}), BackendContext{"error"},
        ConfiguredServerServices{std::make_shared<Services>(fixture, true), fixture.effects().make_process});
}
void load(WrappedServer& server) {
    const RecipeOptions options("ds4", json::object());
    server.set_model_metadata("user.fixture", "fixture/repo", ModelType::LLM, DEVICE_GPU, options);
    server.load("user.fixture", ds4_model(), options, false); server.set_state(ModelState::READY);
}
}
int main() {
    set_env("LEMONADE_BACKEND_WATCHDOG_POLL_SECONDS", "1");
    set_env("LEMONADE_BACKEND_WATCHDOG_GRACE_SECONDS", "10");
    Http http;
    for (bool configured : {false, true}) {
        Fixture fixture(http.port); auto server = arm(configured, fixture);
        check(server->chat_completion(json::object()) == json::parse(R"({"error":{"message":"No model loaded: ds4-server","type":"model_not_loaded"}})"), "literal no loaded model envelope");
        load(*server);
        for (const auto& [operation, endpoint] : std::vector<std::pair<BackendOperation, std::string>>{{BackendOperation::ChatCompletion, "/v1/chat/completions"}, {BackendOperation::Completion, "/v1/completions"}, {BackendOperation::Responses, "/v1/responses"}}) {
            const auto response = server->dispatch_completion_operation(operation, {{"z", 0}, {"model", "user.fixture"}, {"a", nullptr}});
            check(response == json::parse(R"({"choices":[],"usage":{"prompt_tokens":3,"completion_tokens":2}})") && http.body == R"({"a":null,"model":"user.fixture","z":0})" && http.path == endpoint, "buffered identity wire and JSON response literals");
            for (const std::string body : {std::string("{ \"stream\" : true, \"z\":0, \"a\":null }"), std::string("malformed")}) {
                Capture capture; int callbacks = 0;
                server->forward_streaming_request(endpoint, body, capture.sink, true, 5, [&](const auto& telemetry) { ++callbacks; check(telemetry.output_tokens == 2 && telemetry.prompt_tokens == 3, "callback usage frame literals"); });
                check(http.body == body && http.path == endpoint && capture.bytes == http.stream && callbacks == 1, "original identity bytes including malformed input and split SSE frames/DONE");
            }
            Capture bytes; int callbacks = 0;
            server->forward_streaming_request(endpoint, "malformed", bytes.sink, false, 5, [&](const auto&) { ++callbacks; });
            check(bytes.bytes == http.stream && callbacks == 0 && bytes.done == 1, "explicit non-SSE forwarding preserves raw byte mode");
        }
        http.status = 400; http.reply = "bad request";
        check(server->completion(json::object()) == json::parse(R"({"error":{"message":"bad request","type":"invalid_request_error","status_code":400,"details":{"backend":"ds4-server","response":"bad request"}}})"), "literal buffered backend error status/details/identity");
        http.reply = R"({"error":{"message":"context window exceeded","type":"invalid_request_error","code":"context_length_exceeded"}})";
        const auto context_error = server->responses(json::object());
        check(context_error["error"]["code"] == "context_length_exceeded" && context_error["error"]["type"] == "invalid_request_error" && context_error["error"]["details"]["backend"] == "ds4-server", "backend type/code context-window preservation");
        http.status = 200; http.reply = R"({"choices":[],"usage":{"prompt_tokens":3,"completion_tokens":2}})";
        {
            std::atomic<bool> cancel{true}; WrappedServer::RequestCancelScope scope(utils::RequestCancelToken{&cancel});
            check(server->chat_completion(json::object())["error"]["message"] == "Request cancelled by client", "pre-forward cancellation literal");
        }
        {
            http.delay(); std::atomic<bool> cancel{false}; std::promise<json> result; auto finished = result.get_future();
            std::thread worker([&] { WrappedServer::RequestCancelScope scope(utils::RequestCancelToken{&cancel}); result.set_value(server->chat_completion(json::object())); });
            check(http.wait() && server->get_backend_health_state() == "busy", "active buffered request accounting");
            const auto began = std::chrono::steady_clock::now(); cancel = true;
            const bool prompt = finished.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
            http.unblock(); worker.join();
            check(prompt && finished.get()["error"]["message"] == "Request cancelled by client" && std::chrono::steady_clock::now() - began < std::chrono::seconds(4), "slow transfer cancelled upstream before fake response released");
            check(server->get_backend_health_state() == "ready" && !server->was_watchdog_triggered() && fixture.stops == 0, "cancellation releases accounting without reload");
        }
        {
            http.delay(); Capture capture;
            std::thread worker([&] { server->forward_streaming_request("/v1/completions", "malformed", capture.sink); });
            check(http.wait() && server->get_backend_health_state() == "busy", "active streaming accounting before first output");
            http.unblock(); worker.join(); check(server->get_backend_health_state() == "ready", "streaming accounting released");
        }
        {
            http.delay(); Capture capture; std::promise<void> result; auto finished = result.get_future();
            std::thread worker([&] { server->forward_streaming_request("/v1/completions", "malformed", capture.sink, false, 1); result.set_value(); });
            check(http.wait(), "explicit forwarding timeout fixture entered");
            const bool timed_out = finished.wait_for(std::chrono::seconds(8)) == std::future_status::ready;
            http.unblock(); worker.join();
            check(timed_out && capture.bytes.find("streaming_error") != std::string::npos && capture.done == 1 && !server->was_watchdog_triggered(), "incoming one-second timeout preserved independent of global timeout");
        }
        Capture rejected; rejected.sink.write = [](const char*, size_t) { return false; };
        server->forward_streaming_request("/v1/responses", "malformed", rejected.sink);
        check(!server->was_watchdog_triggered() && fixture.stops == 0 && server->get_backend_health_state() == "ready", "sink rejection no reset and released scope");
        server->unload();
        check(fixture.stops == 1 && server->get_process_id() == 0 && server->get_backend_port() == 0 && server->get_launch_command().empty(), "unload clears command/process/port");
        for (int scenario = 0; scenario < 3; ++scenario) {
            Fixture failure(http.port); auto failed = arm(configured, failure); std::atomic<bool> cancel{scenario == 1};
            failed->set_load_cancel_flag(&cancel); if (scenario == 0) failure.child_running = false;
            std::string message;
            try {
                if (scenario < 2) load(*failed);
                else { ServerCommand command; command.program = "fake"; command.port = http.port; command.ready_endpoint = "/unavailable"; Access::start(*failed, failure.effects().make_process({}, ""), command, 1); }
            } catch (const std::runtime_error& error) { message = error.what(); }
            check(message == "ds4-server failed to start" && failure.stops == 1 && failed->get_process_id() == 0, "literal child-exit/cancel/timeout readiness cleanup");
        }
        {
            Fixture crash(http.port); auto idle = arm(configured, crash); load(*idle);
            const int before = http.model_calls;
            http.delay();
            std::thread worker([&] { idle->completion(json::object()); });
            check(http.wait(), "watchdog fixture active buffered work");
            std::this_thread::sleep_for(std::chrono::milliseconds(11200));
            check(http.model_calls > before && http.health_calls == 0, "watchdog keeps promoted models endpoint during active silence");
            http.unblock(); worker.join();
            crash.child_running = false;
            std::unique_lock<std::mutex> lock(crash.stop_mutex);
            check(crash.stopped.wait_for(lock, std::chrono::seconds(3), [&] { return crash.stops == 1; }) && idle->was_watchdog_triggered(), "idle crash watchdog cleanup");
        }
        for (int progress : {0, 1}) {
            Fixture crash(http.port); auto failed = arm(configured, crash); load(*failed);
            http.disconnect = progress; http.on_disconnect = [&] { crash.child_running = false; };
            Capture capture; bool retry = false;
            try { failed->forward_streaming_request("/v1/completions", "malformed", capture.sink); } catch (const BackendStreamRetryableReset&) { retry = true; }
            check(retry == (progress == 0) && crash.stops == 1 && failed->was_watchdog_triggered(), "retry before output only; no replay after output");
            if (progress == 0) check(capture.bytes.empty(), "retry emitted no bytes");
            else check(capture.bytes.find("data: {\"partial\":true}") == 0 && capture.bytes.find("\"backend\":\"ds4-server\"") != std::string::npos && capture.bytes.find("backend_watchdog_reset") != std::string::npos, "partial SSE plus backend reset identity");
            http.disconnect = -1; http.on_disconnect = {};
        }
        { Fixture cleanup(http.port); { auto child = arm(configured, cleanup); load(*child); } check(cleanup.stops == 1, "destructor stops fake child"); }
    }
    const auto root = std::filesystem::temp_directory_path() / ("configured-llm-public-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root); utils::set_cache_dir(root.string()); utils::set_config_dir(root.string());
    const std::string previous = std::getenv("PATH") ? std::getenv("PATH") : ""; set_env("PATH", root.string().c_str()); set_env("LEMONADE_FLM_NPU_BIN", "");
    for (bool configured : {false, true}) {
        auto configuration = ConfigFile::base_defaults();
        configuration.update(json{{"offline", true}, {"disable_model_filtering", true}, {"no_fetch_executables", true}, {"auto_evict", false}, {"auto_check_model_updates", false}, {"inhibit_suspend", false}, {"log_level", "error"}});
        auto config = std::make_shared<RuntimeConfig>(configuration); RuntimeConfig::set_global(config.get());
        {
            Server frontend(config, root.string(), root.string());
            auto& router = MediaResponseTestHook::router(frontend);
            auto& models = MediaResponseTestHook::models(frontend);
            models.register_user_model("user.fixture", {{"recipe", "ds4"}, {"checkpoint", "fixture/repo"}});
            Fixture fixture(http.port); auto server = arm(configured, fixture); load(*server); auto* resident = server.get();
            RoutingHelperTestHook::add(router, std::move(server));
            const auto identity = RoutingHelperTestHook::identity(router, resident);
            check(identity.model_name == "user.fixture" && identity.checkpoint == "fixture/repo" && identity.recipe == "ds4" && identity.device == "gpu", "real router telemetry public recipe/model/checkpoint/device");
            http.delay(); json response;
            std::thread worker([&] { response = router.chat_completion({{"model", "user.fixture"}, {"messages", json::array()}}); });
            check(http.wait() && resident->is_busy(), "real router admission held during buffered fake work");
            http.unblock(); worker.join();
            check(response.contains("choices") && !resident->is_busy(), "real router response and acquisition release");
            http.status = 400; http.reply = "bad request";
            httplib::Request request; request.is_connection_closed = [] { return false; }; request.body = R"({"model":"user.fixture","messages":[]})"; httplib::Response result; result.status = 200;
            MediaResponseTestHook::chat(frontend, request, result);
            check(result.status == 400 && result.body == R"({"error":{"details":{"backend":"ds4-server","response":"bad request"},"message":"bad request","status_code":400,"type":"invalid_request_error"}})" && !resident->is_busy(), "real public handler literal error status/body and release");
            http.status = 200; http.reply = R"({"choices":[],"usage":{"prompt_tokens":3,"completion_tokens":2}})";
        }
        RuntimeConfig::set_global(nullptr);
    }
    set_env("PATH", previous.c_str()); std::filesystem::remove_all(root);
    std::printf("%d/%d checks passed\n", checks - failures, checks); return failures ? 1 : 0;
}
