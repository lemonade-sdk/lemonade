#pragma once

#include "backend_definition_binding_fixture.h"
#include "backend_definition_runner_internal.h"
#include "lemon/backends/configured/configured_server.h"

#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace runtime_fixture {
using namespace binding_fixture;
using namespace lemon;
namespace rt = lemon::backends::runtime;
inline thread_local std::vector<std::string> entries;
inline std::mutex calls_mutex;

inline Json runtime_document() {
    auto value = document();
    auto& merge = value["merge"];
    merge["install"] = {{"strategy", "none"}};
    merge["artifacts"] = Json::object();
    for (const auto* name : {"install_context", "environment", "system"}) merge["hooks"].erase(name);
    auto stream = merge["operations"][0];
    stream["id"] = "chat.streaming";
    stream["delivery"] = "streaming";
    stream["transport"]["kind"] = "http_sse";
    stream["transport"]["path"] = "/stream";
    stream["request"]["hooks"] = Json::array();
    stream.erase("response");
    for (auto& op : merge["operations"]) if (op["id"] == "chat.streaming") op = stream;
    merge["operations"][1]["request"]["hooks"] = Json::array();
    merge["operations"][1]["response"]["hooks"] = Json::array();
    for (auto& op : merge["operations"]) op["transport"]["timeout"] = 10;
    merge["launch"]["watchdog"]["policy"] = "none";
    merge["launch"]["argv"][1]["value"] = {{"string", {{"ref", "/launch/hook/context_size"}}}};
    return value;
}

inline std::shared_ptr<const ExecutableDefinition> executable(const Json& document = runtime_document(), bool external = false,
    std::shared_ptr<const FrozenBuiltinCatalog> parents = {}) {
    calls.clear(); allow_entries = false;
    const auto admitted = external ? admit({document}, parents ? std::move(parents) : empty_builtins()) : admit({}, builtins(false, &document));
    if (!admitted.catalog) { diagnostics(admitted.diagnostics); throw std::logic_error("Runtime fixture admission failed"); }
    const auto token = external ? admitted.catalog->v2_definition(document.at("id")) : admitted.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, document.at("id"), 1});
    const auto compiled = compile_execution_definition(token);
    if (!compiled.definition) { diagnostics(compiled.diagnostics); throw std::logic_error("Runtime fixture compile failed"); }
    const auto bound = bind_execution_definition(admitted.catalog, compiled.definition);
    if (!bound.definition) { diagnostics(bound.diagnostics); throw std::logic_error("Runtime fixture bind failed"); }
    zero("RuntimeFixture");
    allow_entries = true;
    return bound.definition;
}
template<class Call>
inline bool throws(Call call, const std::string& contains = "") {
    try { call(); }
    catch (const std::exception& error) { return contains.empty() || std::string(error.what()).find(contains) != std::string::npos; }
    return false;
}
class Launch final : public LaunchServices {
public:
    int port = 1;
    PlatformInfo host = native_platform();
    std::vector<std::string> supported = {"cpu"};
    std::string program = "fake-engine";
    std::string external_program = "fake-engine";
    bool allow_install = false;
    mutable std::vector<std::string> events;
    bool exists(const std::string&) const override { events.push_back("exists"); return true; }
    std::vector<std::string> supported_backends(const std::string& recipe) const override { events.push_back("supported:" + recipe); return supported; }
    void validate_backend(const std::string& recipe, const std::string& backend) const override { events.push_back("validate:" + recipe + ":" + backend); }
    std::string external_binary(const BackendSpec&, const std::string&) const override { events.push_back("external"); return external_program; }
    void install_backend(const std::string& recipe, const std::string& backend) override {
        if (!allow_install) throw std::logic_error("Unexpected installation");
        events.push_back("install:" + recipe + ":" + backend);
    }
    std::string managed_binary(const BackendSpec&, const std::string&) const override { events.push_back("program"); return program; }
    int allocate_port() override { events.push_back("port"); return port; }
    int find_free_port(int start) override { return start; }
    std::vector<std::pair<std::string, std::string>> environment(const std::string&, const std::string&, const std::string&, const std::string&) const override { return {}; }
    PlatformInfo platform() const override { return host; }
    long default_timeout_seconds() const override { return 1; }
};
inline ModelInfo model() {
    ModelInfo result;
    result.model_name = "loaded-model";
    result.recipe = "binding-fixture";
    result.labels = {"mtp"};
    return result;
}
class Process final : public ServerProcess {
public:
    explicit Process(ServerCommand& command) : ServerProcess({}), command_(command) {}
    std::string start(const ServerCommand& command) override { command_ = command; running_ = true; return "127.0.0.1"; }
    bool running() const override { return running_; }
    void stop() override { running_ = false; }
private:
    ServerCommand& command_;
    bool running_ = false;
};
class Upstream {
public:
    httplib::Server http;
    int port;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable condition;
    unsigned entered = 0;
    unsigned aborted = 0;
    bool release = false;
    std::string stream_body;
    void wait_for_abort(const std::function<bool()>& closed) {
        std::unique_lock<std::mutex> lock(mutex);
        ++entered; condition.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release && std::chrono::steady_clock::now() < deadline) {
            lock.unlock(); const bool disconnected = closed(); lock.lock();
            if (disconnected) { ++aborted; condition.notify_all(); return; }
            condition.wait_for(lock, std::chrono::milliseconds(5));
        }
    }
    Upstream() {
        http.Get("/health", [](const auto&, auto& response) { response.set_content("{}", "application/json"); });
        http.Post(R"(/(fake/.*|v1/chat/completions))", [this](const auto& request, auto& response) {
            const auto value = Json::parse(request.body);
            if (value.value("hold", false)) {
                if (value.value("partial", false)) {
                    response.set_content_provider(100, "application/json", [this, closed = request.is_connection_closed](std::size_t offset, std::size_t, auto& sink) {
                        if (offset == 0) { sink.write("{\"model\":", 9); wait_for_abort(closed); }
                        return false;
                    });
                    return;
                }
                wait_for_abort(request.is_connection_closed);
            }
            auto result = value;
            result["model"] = "upstream-model";
            response.set_content(result.dump(), "application/json");
        });
        http.Get("/fake/slots.buffered", [](const auto&, auto& response) { response.set_content("{\"slots\":true}", "application/json"); });
        http.Post("/stream", [this](const auto& request, auto& response) {
            { std::lock_guard<std::mutex> lock(mutex); stream_body = request.body; }
            response.set_content("data: {\"ok\":true}\n\ndata: [DONE]\n\n", "text/event-stream");
        });
        port = http.bind_to_any_port("127.0.0.1");
        if (port <= 0) throw std::runtime_error("Loopback bind failed");
        worker = std::thread([this] { http.listen_after_bind(); });
        http.wait_until_ready();
    }
    ~Upstream() {
        { std::lock_guard<std::mutex> lock(mutex); release = true; condition.notify_all(); }
        http.stop(); worker.join();
    }
};
struct ServerFixture {
    std::shared_ptr<const ConfiguredBackendEntry> entry;
    std::shared_ptr<Launch> launch = std::make_shared<Launch>();
    ServerCommand command;
    std::unique_ptr<ConfiguredServer> server;
    explicit ServerFixture(int port, const Json& document = runtime_document(), bool external = false) : entry(ConfiguredBackendEntry::make(executable(document, external))) {
        launch->port = port;
        server = std::make_unique<ConfiguredServer>(entry, BackendContext{"error"}, ConfiguredServerServices{launch,
            [this](auto, const auto&) { return std::make_unique<Process>(command); }});
        const auto info = model();
        RecipeOptions options;
        server->set_model_metadata(info.model_name, "fixture", ModelType::LLM, DEVICE_CPU, options);
        server->load(info.model_name, info, options);
        server->set_state(ModelState::READY);
    }
    ~ServerFixture() { if (server) server->unload(); }
};
}

void lemon::backends::typed::observe_entry(const char* entry) {
    std::lock_guard<std::mutex> lock(runtime_fixture::calls_mutex);
    ++binding_fixture::calls[entry];
    if (!binding_fixture::allow_entries) throw std::logic_error("HookBearingCompileAndBindInvokeNothing");
    runtime_fixture::entries.emplace_back(entry);
}
