#pragma once

#include "lemon/backends/backend_effects.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/server.h"
#include "lemon/wrapped_server.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lemon {
struct MediaResponseTestHook {
    static void serve(httplib::Response& response, const std::function<void(httplib::DataSink&)>& generate) {
        Server::serve_media_or_error(response, "audio/wav", generate);
    }
};
}

namespace thinksound_oracle {

using namespace lemon;
inline int checks = 0;
inline int failures = 0;
inline void check(bool value, const std::string& label) {
    ++checks;
    if (!value) ++failures;
    std::printf("%s %s\n", value ? "PASS" : "FAIL", label.c_str());
}

struct Capture {
    std::string bytes;
    int done = 0;
    httplib::DataSink sink;
    Capture() {
        sink.write = [this](const char* data, size_t size) { bytes.append(data, size); return true; };
        sink.is_writable = [] { return true; };
        sink.done = [this] { ++done; };
    }
};

class HttpFixture {
public:
    HttpFixture() {
        http.Get("/health", [](const auto&, auto& response) { response.set_content("{}", "application/json"); });
        http.Post("/generate", [this](const auto& request, auto& response) {
            std::lock_guard<std::mutex> lock(mutex);
            path = request.path;
            method = request.method;
            body = request.body;
            ++requests;
            response.status = status;
            response.set_content(reply, "audio/wav");
        });
        for (const auto* endpoint : {"/v1/chat/completions", "/v1/completions", "/v1/responses"}) {
            http.Post(endpoint, [this](const auto& request, auto& response) {
                std::lock_guard<std::mutex> lock(mutex);
                path = request.path;
                body = request.body;
                ++requests;
                response.set_content("data: {\"fixture\":true}\n\ndata: [DONE]\n\n", "text/event-stream");
            });
        }
        port = http.bind_to_any_port("127.0.0.1");
        if (port <= 0) throw std::runtime_error("Cannot bind fake transport");
        thread = std::thread([this] { http.listen_after_bind(); });
        http.wait_until_ready();
    }
    ~HttpFixture() { http.stop(); thread.join(); }
    void respond(int code, std::string bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        status = code;
        reply = std::move(bytes);
    }
    int port;
    std::string path, method, body;
    size_t requests = 0;
private:
    httplib::Server http;
    std::thread thread;
    std::mutex mutex;
    int status = 200;
    std::string reply = std::string("RIFF\0\xff\r\nWAVE", 12);
};

struct EffectsFixture {
    std::vector<std::string> events;
    ServerCommand command;
    ProcessOutput output;
    std::string cwd;
    std::string external;
    std::string arch = "gfx-fixture";
    std::optional<std::string> inherited = "old";
    std::vector<std::string> supported = {"vulkan", "cuda"};
    bool model_exists = true;
    bool binary_exists = true;
    bool child_running = true;
    int port;
    int stops = 0;
    explicit EffectsFixture(int value) : port(value) {}

    class Process final : public ServerProcess {
    public:
        explicit Process(EffectsFixture& fixture) : ServerProcess(fixture.output), fixture_(fixture) {}
        ~Process() override { handle_ = {nullptr, 0}; }
        std::string start(const ServerCommand& command) override {
            fixture_.events.push_back("start");
            fixture_.command = command;
            command_line_ = {command.program};
            command_line_.insert(command_line_.end(), command.args.begin(), command.args.end());
            handle_.pid = 4242;
            return "127.0.0.1";
        }
        void stop() override { ++fixture_.stops; handle_ = {nullptr, 0}; }
        bool running() const override { return handle_.pid && fixture_.child_running; }
    private:
        EffectsFixture& fixture_;
    };

    backends::BackendEffects effects() {
        backends::BackendEffects value;
        value.exists = [this](const auto& path) { events.push_back("model:" + path); return model_exists; };
        value.binary = {
            [this](const auto& recipe, const auto& backend) { events.push_back("external:" + recipe + ":" + backend); return external; },
            [this](const auto& path) { events.push_back("binary-exists:" + path); return binary_exists; },
            [this](const auto& recipe, const auto& backend) { events.push_back("install:" + recipe + ":" + backend); },
            [this](const auto& spec, const auto& backend) { events.push_back("managed:" + spec.recipe + ":" + backend); return std::string("/bin dir/ts-server"); }
        };
        value.supported_backends = [this](const auto& recipe) { events.push_back("supported:" + recipe); return supported; };
        value.validate_backend = [this](const auto& recipe, const auto& backend) { events.push_back("validate:" + recipe + ":" + backend); };
        value.allocate_port = [this] { events.push_back("port"); return port; };
        value.rocm_arch = [this] { events.push_back("arch"); return arch; };
        value.runtime_directories = [this](const auto& a) { events.push_back("runtime:" + a); return std::string("/runtime/core:/runtime/libs"); };
        value.inherited_loader = [this] { events.push_back("inherited"); return inherited; };
        value.append_cuda = [this](auto& env, const auto& category) {
            events.push_back("cuda:" + category);
            env.emplace_back("CUDA_VISIBLE_DEVICES", "2,0");
            env.emplace_back("__NV_PRIME_RENDER_OFFLOAD", "1");
        };
        value.make_process = [this](ProcessOutput result, const auto& directory) {
            events.push_back("process"); output = result; cwd = directory;
            return std::make_unique<Process>(*this);
        };
        return value;
    }
};

inline RecipeOptions empty_options() {
    RecipeOptions options;
    options.set_option("thinksound_backend", "");
    return options;
}

inline ModelInfo model() {
    ModelInfo info;
    info.checkpoints["main"] = "fixture/repo";
    info.resolved_paths["main"] = u8"/model dir/雪\"trailing\\";
    return info;
}

inline const std::vector<std::pair<json, std::string>> mapping_cases = {
    {json{{"model", "fixture"}, {"prompt", "rain"}, {"cot", "distant thunder"}, {"duration", 0}, {"steps", nullptr}, {"cfg", false}, {"seed", -1}, {"ignored", "drop"}},
     R"({"caption":"rain","cfg":false,"description":"distant thunder","duration":0,"seed":-1,"steps":null})"},
    {json{{"prompt", "rain"}, {"description", nullptr}, {"cot", "ignored"}}, R"({"caption":"rain","description":null})"},
    {json{{"prompt", "rain"}, {"description", false}}, R"({"caption":"rain","description":false})"},
    {json{{"prompt", "rain"}, {"description", 0}}, R"({"caption":"rain","description":0})"},
    {json{{"prompt", "rain"}, {"description", ""}}, R"({"caption":"rain","description":""})"},
    {json::object(), R"({"caption":"","description":""})"},
    {json{{"prompt", u8"雪\"\\\n"}, {"response_format", "wav"}, {"stream", true}}, u8R"({"caption":"雪\"\\\n","description":"雪\"\\\n"})"}
};

inline void wire_oracle(WrappedServer& server, HttpFixture& http) {
    auto* audio = dynamic_cast<IAudioGenerationServer*>(&server);
    check(audio && audio->supported_audio_formats() == std::vector<std::string>{"wav"}, "formats literal wav");
    for (const auto& [input, expected] : mapping_cases) {
        Capture output;
        audio->audio_generations(input, output.sink);
        check(http.method == "POST" && http.path == "/generate" && http.body == expected, "independent wire body");
        check(output.bytes == std::string("RIFF\0\xff\r\nWAVE", 12) && output.done == 1, "binary bytes and completion");
        check(!server.is_busy(), "request accounting released");
    }
    for (const auto& prompt : {json(nullptr), json(false), json(4), json::array(), json::object()}) {
        bool rejected = false;
        try { Capture capture; audio->audio_generations(json{{"prompt", prompt}}, capture.sink); }
        catch (const json::type_error&) { rejected = true; }
        check(rejected, "direct prompt nlohmann type error");
    }
    const std::vector<std::tuple<int, std::string, std::string>> errors = {
        {200, "", ""},
        {400, R"({"error":{"message":"bad","type":"invalid_request_error"}})", R"({"error":{"message":"bad","status_code":400,"type":"invalid_request_error"}})"},
        {500, R"({"error":{"message":"bad","status_code":422}})", R"({"error":{"message":"bad","status_code":422}})"},
        {400, "plain", R"({"error":{"message":"plain","status_code":400,"type":"backend_error"}})"},
        {500, "", R"({"error":{"message":"backend returned HTTP 500","status_code":500,"type":"backend_error"}})"}
    };
    for (const auto& [code, reply, expected] : errors) {
        http.respond(code, reply);
        Capture output;
        audio->audio_generations(json{{"prompt", "rain"}}, output.sink);
        check(output.bytes == expected && output.done == 1, "literal error envelope");
    }
    http.respond(400, std::string(70000, 'x'));
    Capture capped;
    audio->audio_generations(json{{"prompt", "rain"}}, capped.sink);
    check(json::parse(capped.bytes)["error"]["message"] == std::string(65536, 'x'), "64 KiB backend error cap");
    const std::vector<std::tuple<int, std::string, int, std::string>> public_cases = {
        {200, std::string("RIFF\0\xff\r\nWAVE", 12), 200, std::string("RIFF\0\xff\r\nWAVE", 12)},
        {200, "", 502, R"({"error":{"message":"Generation failed: the backend produced no output (it likely crashed or ran out of GPU memory). Check the server logs.","type":"backend_error"}})"},
        {400, "plain", 400, R"({"error":{"message":"plain","status_code":400,"type":"backend_error"}})"},
        {500, R"({"error":{"message":"bad","status_code":422}})", 422, R"({"error":{"message":"bad","status_code":422}})"}
    };
    for (const auto& [code, reply, expected_status, expected] : public_cases) {
        http.respond(code, reply);
        httplib::Response response;
        response.status = 200;
        MediaResponseTestHook::serve(response, [&](auto& sink) { audio->audio_generations(json{{"prompt", "rain"}}, sink); });
        check(response.status == expected_status && response.body == expected, "public buffered media literal status and body");
        check(response.get_header_value("Content-Type") == (expected_status == 200 ? "audio/wav" : "application/json"), "public media MIME");
    }
    for (const auto* endpoint : {"/v1/chat/completions", "/v1/completions", "/v1/responses"}) {
        const std::string input = "{ \"stream\" : true, \"model\" : \"fixture\" }";
        Capture output;
        server.forward_streaming_request(endpoint, input, output.sink);
        check(http.path == endpoint && http.body == input, "legacy original request bytes");
        check(output.bytes == "data: {\"fixture\":true}\n\ndata: [DONE]\n\n", "legacy complete SSE frames");
    }
    for (size_t op = 0; op < k_backend_operation_count; ++op) {
        for (size_t delivery = 0; delivery < k_operation_delivery_count; ++delivery) {
            for (auto mode : k_operation_model_types) {
                bool expected = (op < 3 && delivery == 1) ||
                    (op == 9 && delivery == 0 && mode == ModelType::AUDIO_GENERATION);
                check(server.supports_operation(static_cast<BackendOperation>(op), static_cast<OperationDelivery>(delivery), mode) == expected,
                      "independent operation matrix");
            }
        }
    }
}

}
