#include <cstdio>
#include <functional>
#include <string>
#include <thread>

#include <httplib.h>
#include <lemon/streaming_proxy.h>

static int g_failures = 0;

static void check(bool condition, const char* name) {
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++g_failures;
    }
}

namespace {

struct StreamResult {
    std::string downstream;
    std::string error_message;
    bool done_called = false;
};

// Runs one SSE proxy request against a mock backend whose response body is
// produced by backend_body, and reports what reached the client.
StreamResult run_proxy(const std::function<void(httplib::DataSink&)>& backend_body) {
    httplib::Server backend;
    backend.Post("/v1/chat/completions",
        [&](const httplib::Request&, httplib::Response& res) {
            res.set_chunked_content_provider(
                "text/event-stream",
                [&](size_t, httplib::DataSink& sink) {
                    backend_body(sink);
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

    httplib::DataSink downstream;
    downstream.write = [&result](const char* data, size_t len) {
        result.downstream.append(data, len);
        return true;
    };
    downstream.done = [&result]() { result.done_called = true; };
    downstream.is_writable = []() { return true; };

    lemon::StreamingProxy::forward_sse_stream(
        "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions",
        R"({"model":"test-model","stream":true})",
        downstream,
        [&result](const lemon::StreamingProxy::TelemetryData& telemetry) {
            result.error_message = telemetry.error_message;
        },
        10,
        nullptr,
        0
    );

    backend.stop();
    backend_thread.join();
    return result;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// A backend that answers 200 text/event-stream and then closes without emitting
// a single event has failed the request, even though the transport was clean.
static void test_empty_stream_is_reported_as_an_error() {
    const StreamResult result = run_proxy([](httplib::DataSink&) {});

    check(contains(result.downstream, "data: {"), "empty stream: client receives a framed data event");
    check(contains(result.downstream, "\"error\""), "empty stream: event carries an error object");
    check(!contains(result.downstream, "[DONE]"), "empty stream: no [DONE] claims success");
    check(!result.error_message.empty(), "empty stream: telemetry records the failure");
    check(result.done_called, "empty stream: response is terminated");
}

// Backends log their own failures into the already-committed 200 stream. Those
// bytes are not SSE, so the client still needs a framed error event.
static void test_unframed_backend_output_is_reported_as_an_error() {
    const StreamResult result = run_proxy([](httplib::DataSink& sink) {
        const std::string noise = "backend: model arena alloc failed: out of memory\n";
        sink.write(noise.data(), noise.size());
    });

    check(contains(result.downstream, "\"error\""), "unframed output: event carries an error object");
    check(!contains(result.downstream, "[DONE]"), "unframed output: no [DONE] claims success");
    check(!result.error_message.empty(), "unframed output: telemetry records the failure");
}

// A backend that streamed content but omitted [DONE] served a real response, so
// the marker is still synthesized rather than turned into an error.
static void test_stream_without_done_marker_still_completes() {
    const StreamResult result = run_proxy([](httplib::DataSink& sink) {
        const std::string chunk = "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n";
        sink.write(chunk.data(), chunk.size());
    });

    check(contains(result.downstream, "Hello"), "missing [DONE]: content is forwarded");
    check(contains(result.downstream, "data: [DONE]"), "missing [DONE]: marker is synthesized");
    check(!contains(result.downstream, "\"error\""), "missing [DONE]: no error event");
    check(result.error_message.empty(), "missing [DONE]: telemetry reports success");
}

static void test_complete_stream_is_unchanged() {
    const StreamResult result = run_proxy([](httplib::DataSink& sink) {
        const std::string chunk = "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n";
        sink.write(chunk.data(), chunk.size());
        const std::string done = "data: [DONE]\n\n";
        sink.write(done.data(), done.size());
    });

    check(contains(result.downstream, "Hello"), "complete stream: content is forwarded");
    check(!contains(result.downstream, "\"error\""), "complete stream: no error event");
    check(result.error_message.empty(), "complete stream: telemetry reports success");
}

int main() {
    test_empty_stream_is_reported_as_an_error();
    test_unframed_backend_output_is_reported_as_an_error();
    test_stream_without_done_marker_still_completes();
    test_complete_stream_is_unchanged();

    if (g_failures == 0) {
        std::printf("All silent stream tests passed.\n");
        return 0;
    }
    std::printf("%d silent stream test(s) failed.\n", g_failures);
    return 1;
}
