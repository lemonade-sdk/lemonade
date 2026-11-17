// StreamingProxy's chunk rewriter: each data event reaches the client edited,
// whole and framed, however the backend split it across writes.

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <lemon/streaming_proxy.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static int g_failures = 0;

static void check(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++g_failures;
}

// Serves `writes` as one SSE response, each element a separate write.
struct FakeBackend {
    httplib::Server server;
    std::thread thread;
    int port = 0;

    explicit FakeBackend(std::vector<std::string> writes) {
        server.Post("/v1/chat/completions",
                    [writes](const httplib::Request&, httplib::Response& res) {
                        res.set_chunked_content_provider(
                            "text/event-stream", [writes](size_t, httplib::DataSink& sink) {
                                for (const auto& w : writes) sink.write(w.data(), w.size());
                                sink.done();
                                return false;
                            });
                    });
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this]() { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~FakeBackend() {
        server.stop();
        thread.join();
    }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
    }
};

struct Received {
    std::string text;
    lemon::StreamingProxy::TelemetryData telemetry;
};

static Received forward(const FakeBackend& backend, lemon::StreamingProxy::ChunkRewriter rewrite,
                        size_t accept_writes = SIZE_MAX) {
    Received received;
    size_t writes = 0;
    httplib::DataSink sink;
    sink.write = [&](const char* data, size_t len) {
        if (writes++ >= accept_writes) return false;
        received.text.append(data, len);
        return true;
    };
    sink.done = []() {};
    sink.is_writable = []() { return true; };
    lemon::StreamingProxy::forward_sse_stream(
        backend.url(), R"({"model":"m","stream":true})", sink,
        [&](const lemon::StreamingProxy::TelemetryData& t) { received.telemetry = t; }, 10,
        nullptr, /*heartbeat_interval_ms=*/0, std::move(rewrite));
    return received;
}

static void rename_reasoning(json& chunk) {
    chunk["model"] = "lemonade-name";
    auto& delta = chunk["choices"][0]["delta"];
    delta["reasoning_content"] = delta["reasoning"];
    delta.erase("reasoning");
}

static void test_rewrites_events_split_across_writes() {
    const std::string event =
        R"({"model":"Org/Model","choices":[{"delta":{"reasoning":"think"}}]})";
    FakeBackend backend({"data: " + event.substr(0, 20), event.substr(20) + "\r", "\n\r\n",
                         ": keep-alive\n\n",
                         R"(data: {"choices":[],"usage":{"prompt_tokens":3,"completion_tokens":2}})"
                         "\n\n",
                         "data: [DONE]\n\n"});
    const Received received = forward(backend, rename_reasoning);

    const std::string first_line = received.text.substr(0, received.text.find('\n'));
    json first = json::parse(first_line.substr(6), nullptr, false);
    check(first_line.rfind("data: ", 0) == 0 && first.is_object() &&
              first["model"] == "lemonade-name" &&
              first["choices"][0]["delta"]["reasoning_content"] == "think" &&
              !first["choices"][0]["delta"].contains("reasoning"),
          "an event split across writes reaches the client rewritten");
    check(received.text.find(": keep-alive\n\n") != std::string::npos &&
              received.text.size() >= 14 &&
              received.text.compare(received.text.size() - 14, 14, "data: [DONE]\n\n") == 0,
          "comments and [DONE] pass through, each event still ends in a blank line");
    check(received.telemetry.output_tokens == 2 && received.telemetry.error_message.empty(),
          "telemetry still reads the backend's usage");
}

static void test_without_rewriter_bytes_pass_through() {
    const std::vector<std::string> writes = {"data: {\"a\":1}\r\n\r\n", "data: [DONE]\n\n"};
    FakeBackend backend(writes);
    check(forward(backend, nullptr).text == writes[0] + writes[1],
          "without a rewriter the client gets the backend's bytes unchanged");
}

static void test_payload_that_is_not_json_passes_unchanged() {
    FakeBackend backend({"data: not json\n\n", "data: [DONE]\n\n"});
    check(forward(backend, rename_reasoning).text == "data: not json\n\ndata: [DONE]\n\n",
          "a payload that is not JSON passes unchanged");
}

static void test_client_disconnect_stops_rewritten_stream() {
    FakeBackend backend({"data: {\"model\":\"x\",\"choices\":[{\"delta\":{}}]}\n\n",
                         "data: {\"model\":\"x\",\"choices\":[{\"delta\":{}}]}\n\n",
                         "data: [DONE]\n\n"});
    const Received received = forward(backend, rename_reasoning, /*accept_writes=*/1);
    check(received.telemetry.error_message == "Client disconnected during stream",
          "a client that stops reading ends a rewritten stream as a disconnect");
}

int main() {
    test_rewrites_events_split_across_writes();
    test_without_rewriter_bytes_pass_through();
    test_payload_that_is_not_json_passes_unchanged();
    test_client_disconnect_stops_rewritten_stream();
    if (g_failures == 0) {
        std::printf("\nAll streaming rewrite checks passed.\n");
        return 0;
    }
    std::printf("\n%d streaming rewrite check(s) failed.\n", g_failures);
    return 1;
}
