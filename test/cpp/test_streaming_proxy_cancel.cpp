// Regression test for cancelling a proxied SSE request before the first body byte.
//
// The backend accepts the request but intentionally emits no SSE data. The
// downstream client then becomes unwritable. StreamingProxy must notice that via
// HttpClient's libcurl progress callback and return promptly instead of waiting
// for the backend's first chunk.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <httplib.h>

#include <lemon/streaming_proxy.h>

using namespace std::chrono_literals;

int main() {
    int failures = 0;
    auto check = [&failures](bool condition, const char* name) {
        if (condition) {
            std::printf("[PASS] %s\\n", name);
        } else {
            std::printf("[FAIL] %s\\n", name);
            ++failures;
        }
    };

    httplib::Server backend;
    std::atomic<bool> backend_started{false};
    std::atomic<bool> release_backend{false};

    backend.Post("/v1/chat/completions",
        [&](const httplib::Request&, httplib::Response& res) {
            backend_started.store(true, std::memory_order_release);

            res.set_chunked_content_provider(
                "text/event-stream",
                [&](size_t, httplib::DataSink&) {
                    // Simulate a long prefill/TTFT period: keep the connection
                    // open without sending a single response body byte.
                    while (!release_backend.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(10ms);
                    }
                    return false;
                });
        });

    const int port = backend.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::printf("[FAIL] failed to bind mock backend\\n");
        return 1;
    }

    std::thread backend_thread([&backend]() {
        backend.listen_after_bind();
    });
    backend.wait_until_ready();

    std::atomic<bool> downstream_writable{true};
    int downstream_write_calls = 0;

    httplib::DataSink downstream;
    downstream.write = [&](const char*, size_t) {
        ++downstream_write_calls;
        return downstream_writable.load(std::memory_order_acquire);
    };
    downstream.done = []() {};
    downstream.done_with_trailer = [](const httplib::Headers&) {};
    downstream.is_writable = [&downstream_writable]() {
        return downstream_writable.load(std::memory_order_acquire);
    };

    std::string telemetry_error;

    std::thread disconnect_thread([&]() {
        const auto wait_deadline = std::chrono::steady_clock::now() + 2s;
        while (!backend_started.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < wait_deadline) {
            std::this_thread::sleep_for(5ms);
        }

        // Give the backend enough time to establish the HTTP response while
        // still remaining before any SSE body bytes.
        std::this_thread::sleep_for(100ms);
        downstream_writable.store(false, std::memory_order_release);
    });

    const auto started = std::chrono::steady_clock::now();

    lemon::StreamingProxy::forward_sse_stream(
        "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions",
        R"({"model":"test-model","stream":true})",
        downstream,
        [&](const lemon::StreamingProxy::TelemetryData& telemetry) {
            telemetry_error = telemetry.error_message;
        },
        10);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    disconnect_thread.join();
    release_backend.store(true, std::memory_order_release);
    backend.stop();
    backend_thread.join();

    check(
        backend_started.load(std::memory_order_acquire),
        "mock backend received the streaming request");
    check(
        elapsed < 3000ms,
        "client disconnect cancels upstream before first stream byte");
    check(
        telemetry_error == "Client disconnected during stream",
        "early cancellation is classified as a client disconnect");
    check(
        downstream_write_calls == 0,
        "no payload is written after a pre-first-byte disconnect");

    std::printf(
        "StreamingProxy pre-first-byte cancellation completed in %lld ms\\n",
        static_cast<long long>(elapsed.count()));

    return failures == 0 ? 0 : 1;
}
