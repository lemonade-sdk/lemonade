#include "lemon/backends/llamacpp/llamacpp_server.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/http_client.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

using namespace lemon;
using namespace lemon::backends;

namespace {
int checks = 0;
int failures = 0;
void check(bool passed, const std::string& label) {
    ++checks;
    if (!passed) {
        ++failures;
        std::cerr << "FAIL " << label << '\n';
    }
}

const std::string sse_bytes = "data: {\"model\":\"backend-model\"}\n\ndata: [DONE]\n\n";

class Upstream {
public:
    Upstream() {
        http.Get("/health", [&](const auto&, auto& response) {
            ++health_requests;
            response.set_content("{}", "application/json");
        });
        http.Get("/slots", [&](const auto& request, auto& response) { respond(request, response); });
        http.Post(".*", [&](const auto& request, auto& response) { respond(request, response); });
        port = http.bind_to_any_port("127.0.0.1");
        if (port <= 0) throw std::runtime_error("oracle loopback bind failed");
        thread = std::thread([&] { http.listen_after_bind(); });
        http.wait_until_ready();
    }
    ~Upstream() { http.stop(); thread.join(); }
    void respond(const httplib::Request& request, httplib::Response& response) {
        std::lock_guard<std::mutex> lock(mutex);
        ++requests;
        last = {{"method", request.method}, {"path", request.target}};
        if (request.method == "POST") last["body"] = streaming ? json(request.body) : json::parse(request.body);
        response.set_content(streaming ? sse_bytes : reply.dump(), streaming ? "text/event-stream" : "application/json");
    }
    void configure(bool stream, const json& response) {
        std::lock_guard<std::mutex> lock(mutex);
        streaming = stream;
        reply = response;
    }
    json recorded() {
        std::lock_guard<std::mutex> lock(mutex);
        return last;
    }
    httplib::Server http;
    int port;
    std::atomic<int> health_requests{0};
    std::atomic<int> requests{0};
private:
    std::mutex mutex;
    std::thread thread;
    bool streaming = false;
    json reply;
    json last;
};

struct ProcessState {
    int starts = 0;
    int stops = 0;
    bool running = true;
    ServerCommand command;
};

class FakeProcess : public ServerProcess {
public:
    explicit FakeProcess(std::shared_ptr<ProcessState> state) : ServerProcess({false, true}), state_(std::move(state)) {}
    std::string start(const ServerCommand& command) override {
        ++state_->starts;
        state_->command = command;
        return "127.0.0.1";
    }
    void stop() override { ++state_->stops; }
    bool running() const override { return state_->running; }
private:
    std::shared_ptr<ProcessState> state_;
};

class Transport : public LlamaCppServer {
public:
    explicit Transport(int port) : LlamaCppServer("error", nullptr, nullptr) {
        port_ = port;
        set_model_metadata("loaded-model", "fixture/repo", ModelType::LLM, DEVICE_CPU, RecipeOptions("llamacpp", {}));
        set_state(ModelState::READY);
    }
    bool is_backend_alive() const override { return true; }
    void start_fake(std::shared_ptr<ProcessState> state, int port) {
        ServerCommand command;
        command.program = "never-executed";
        command.port = port;
        start_server(std::make_unique<FakeProcess>(std::move(state)), command, 1);
    }
    void stop_fake() { stop_server(); }
};

json buffered(Transport& server, int operation, const json& input) {
    if (operation <= 2) return server.dispatch_completion_operation(static_cast<BackendOperation>(operation), input);
    if (operation == 3) return server.embeddings(input);
    if (operation == 4) return server.reranking(input);
    if (operation == 15) return server.get_slots();
    if (operation == 16) return server.slots_action(2, "save", input);
    return server.tokenize(input);
}
}

int main() {
    try {
        AixLog::Log::init({});
        std::ifstream file(std::filesystem::path(LEMON_ORACLE_FIXTURES) / "linux_x64.json");
        const json protocol = json::parse(file).at("protocol");
        Upstream upstream;
        Transport server(upstream.port);
        for (const auto& [name, record] : protocol["buffered"].items()) {
            const auto separator = name.find(':');
            const int operation = std::stoi(name.substr(0, separator));
            const int variant = std::stoi(name.substr(separator + 1));
            upstream.configure(false, variant == 3 ? json::array({1}) : json{{"model", "backend-model"}, {"witness", true}});
            check(buffered(server, operation, record["input"]) == record["response"], "buffered response " + name);
            check(upstream.recorded() == record["forwarded"], "buffered forwarding " + name);
        }
        for (const auto& [name, record] : protocol["streaming"].items()) {
            const std::string path = record["forwarded"]["path"];
            upstream.configure(true, nullptr);
            std::string output;
            httplib::DataSink sink;
            sink.write = [&](const char* data, size_t size) { output.append(data, size); return true; };
            sink.is_writable = [] { return true; };
            sink.done = [] {};
            WrappedServer::TelemetryCallback callback = record["forwarded"]["telemetry"].get<bool>() ? WrappedServer::TelemetryCallback([](const auto&) {}) : nullptr;
            server.forward_streaming_request(path, record["input"], sink, record["forwarded"]["sse"], record["forwarded"]["timeout"], callback);
            const auto actual = upstream.recorded();
            check(actual["body"] == record["forwarded"]["body"] && actual["path"] == path && actual["method"] == "POST", "streaming forwarding " + name);
            check(output == record["bytes"], "byte-preserving stream " + name);
        }
        upstream.configure(false, json::object());
        for (int operation : {0, 1, 2}) {
            const int before = upstream.requests;
            WrappedServer::RequestCancelScope scope(utils::RequestCancelToken{nullptr, [] { return true; }});
            const auto response = buffered(server, operation, {{"model", "client"}});
            check(response["error"]["message"] == "Request cancelled by client" && upstream.requests == before,
                  "cancelled before forwarding " + std::to_string(operation));
        }
        auto live = std::make_shared<ProcessState>();
        server.start_fake(live, upstream.port);
        check(live->starts == 1 && live->command.ready_endpoint == "/health" && upstream.health_requests > 0,
              "fake launch uses the actual readiness and watchdog endpoint");
        server.stop_fake();
        check(live->stops == 1, "fake process stopped once");
        auto dead = std::make_shared<ProcessState>();
        dead->running = false;
        std::string error;
        try { server.start_fake(dead, upstream.port); }
        catch (const std::runtime_error& exception) { error = exception.what(); }
        check(error == "llama-server failed to start" && dead->starts == 1 && dead->stops == 1,
              "terminated fake process fails readiness and is stopped");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
    std::cout << checks - failures << '/' << checks << " transport checks passed\n";
    return failures ? 1 : 0;
}
