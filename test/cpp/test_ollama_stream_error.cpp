#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/server/gateway_conversion.h"
#include "lemon/streaming_proxy.h"

using json = nlohmann::json;

static int g_failures = 0;

static void check(bool condition, const std::string& name) {
    if (condition) {
        std::printf("[PASS] %s\n", name.c_str());
    } else {
        std::printf("[FAIL] %s\n", name.c_str());
        ++g_failures;
    }
}

namespace {

struct StreamResult {
    std::vector<json> lines;
    std::string error_message;
    bool done_called = false;
};

// Runs one Ollama stream against a mock backend that answers 200
// text/event-stream with backend_body, and reports what reached the client
// and what the proxy's telemetry recorded.
StreamResult run_ollama_stream(const std::string& backend_body) {
    httplib::Server backend;
    backend.Post("/v1/chat/completions",
        [&](const httplib::Request&, httplib::Response& res) {
            res.set_chunked_content_provider(
                "text/event-stream",
                [&](size_t, httplib::DataSink& sink) {
                    sink.write(backend_body.data(), backend_body.size());
                    sink.done();
                    return false;
                });
        });

    StreamResult result;

    const int port = backend.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::printf("[FAIL] failed to bind mock backend\n");
        ++g_failures;
        return result;
    }

    std::thread backend_thread([&backend]() { backend.listen_after_bind(); });
    backend.wait_until_ready();

    std::string ndjson;
    httplib::DataSink client;
    client.write = [&ndjson](const char* data, size_t len) {
        ndjson.append(data, len);
        return true;
    };
    client.done = [&result]() { result.done_called = true; };
    client.is_writable = []() { return true; };

    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
    lemon::stream_sse_to_ndjson(
        R"({"model":"test-model","stream":true})", client,
        [](const json& chunk) {
            json line = {{"done", false}, {"response", ""}};
            if (chunk.contains("choices") && !chunk["choices"].empty()) {
                const auto& delta = chunk["choices"][0].value("delta", json::object());
                if (delta.contains("content") && delta["content"].is_string()) {
                    line["response"] = delta["content"];
                }
            }
            return line;
        },
        [](int, int) { return json{{"done", true}}; },
        [&url, &result](const std::string& body, httplib::DataSink& sink) {
            lemon::StreamingProxy::forward_sse_stream(
                url, body, sink,
                [&result](const lemon::StreamingProxy::TelemetryData& telemetry) {
                    result.error_message = telemetry.error_message;
                },
                10, nullptr, 0);
        });

    backend.stop();
    backend_thread.join();

    size_t start = 0;
    for (size_t end = ndjson.find('\n'); end != std::string::npos;
         start = end + 1, end = ndjson.find('\n', start)) {
        if (end > start) {
            result.lines.push_back(json::parse(ndjson.substr(start, end - start)));
        }
    }
    return result;
}

bool has_done_line(const std::vector<json>& lines) {
    for (const auto& line : lines) {
        if (line.value("done", false)) {
            return true;
        }
    }
    return false;
}

const std::string kContentEvent = R"(data: {"choices":[{"delta":{"content":"Hello"}}]})" "\n\n";

}  // namespace

// A backend that fails after streaming some content: the client gets that
// content, then one error line, and the failure is the backend's, not a
// client disconnect.
static void test_error_after_content() {
    const StreamResult result =
        run_ollama_stream(kContentEvent + R"(data: {"error":{"message":"boom"}})" "\n\n");

    check(result.lines.size() == 2, "error after content: content line then error line");
    check(!result.lines.empty() && result.lines[0].value("response", "") == "Hello",
          "error after content: content is relayed");
    check(!result.lines.empty() && result.lines.back() == json{{"error", "boom"}},
          "error after content: last line is the backend's error message");
    check(!has_done_line(result.lines), "error after content: no done line");
    check(result.done_called, "error after content: response is terminated");
    check(result.error_message == "Backend reported an error during stream",
          "error after content: telemetry records a backend error, not a client disconnect");
}

static void test_clean_stream_is_unchanged() {
    const StreamResult result = run_ollama_stream(kContentEvent + "data: [DONE]\n\n");

    check(!result.lines.empty() && result.lines[0].value("response", "") == "Hello",
          "clean stream: content is relayed");
    check(has_done_line(result.lines), "clean stream: ends with a done line");
    check(result.error_message.empty(), "clean stream: telemetry reports success");
    check(result.done_called, "clean stream: response is terminated");
}

int main() {
    std::printf("=== Ollama Stream Error Tests ===\n\n");

    test_error_after_content();
    test_clean_stream_is_unchanged();

    std::printf("\n%s\n", g_failures == 0 ? "All tests passed" : "Some tests failed");
    return g_failures == 0 ? 0 : 1;
}
