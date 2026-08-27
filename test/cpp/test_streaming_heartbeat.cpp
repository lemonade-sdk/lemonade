#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <lemon/streaming_proxy.h>

using namespace std::chrono_literals;

static int g_failures = 0;

static void check(bool condition, const char* name) {
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++g_failures;
    }
}

static void test_heartbeat_emission_during_prefill() {
    httplib::Server backend;
    std::atomic<bool> release_backend{false};
    std::atomic<int> heartbeat_count{0};

    backend.Post("/v1/chat/completions",
        [&](const httplib::Request&, httplib::Response& res) {
            res.set_chunked_content_provider(
                "text/event-stream",
                [&](size_t, httplib::DataSink& sink) {
                    const auto deadline = std::chrono::steady_clock::now() + 5000ms;
                    while (heartbeat_count.load(std::memory_order_acquire) < 2 &&
                           std::chrono::steady_clock::now() < deadline) {
                        if (release_backend.load(std::memory_order_acquire)) {
                            return false;
                        }
                        std::this_thread::sleep_for(10ms);
                    }

                    const std::string chunk1 = "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n";
                    sink.write(chunk1.data(), chunk1.size());
                    std::this_thread::sleep_for(50ms);

                    const std::string chunk2 = "data: [DONE]\n\n";
                    sink.write(chunk2.data(), chunk2.size());
                    sink.done();
                    return false;
                });
        });

    const int port = backend.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::printf("[FAIL] failed to bind mock backend\n");
        ++g_failures;
        return;
    }

    std::thread backend_thread([&backend]() {
        backend.listen_after_bind();
    });
    backend.wait_until_ready();

    std::vector<std::string> received_writes;
    int data_chunk_count = 0;
    int done_count = 0;

    httplib::DataSink downstream;
    downstream.write = [&](const char* data, size_t len) {
        std::string s(data, len);
        received_writes.push_back(s);
        if (s == ": ping\n\n") {
            heartbeat_count.fetch_add(1, std::memory_order_release);
        } else if (s.find("Hello") != std::string::npos) {
            ++data_chunk_count;
        }
        if (s.find("[DONE]") != std::string::npos) {
            ++done_count;
        }
        return true;
    };
    downstream.done = []() {};
    downstream.is_writable = []() { return true; };

    lemon::StreamingProxy::TelemetryData telemetry_result;
    const auto start = std::chrono::steady_clock::now();

    lemon::StreamingProxy::forward_sse_stream(
        "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions",
        R"({"model":"test-model","stream":true})",
        downstream,
        [&](const lemon::StreamingProxy::TelemetryData& tel) {
            telemetry_result = tel;
        },
        10,
        nullptr,
        1000
    );

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    release_backend.store(true, std::memory_order_release);
    backend.stop();
    backend_thread.join();

    check(heartbeat_count.load(std::memory_order_acquire) >= 2, "heartbeat comments emitted during prefill (>= 2)");
    check(data_chunk_count == 1, "data chunk received intact after prefill");
    check(done_count == 1, "done marker received");
    check(telemetry_result.error_message.empty(), "stream completed without error");
    check(telemetry_result.time_to_first_token >= 2.0, "telemetry TTFT measured accurately");

    std::printf(
        "Prefill test completed in %lld ms with %d heartbeats\n",
        static_cast<long long>(elapsed.count()), heartbeat_count.load());
}

static void test_heartbeat_emission_during_pause_between_tokens() {
    httplib::Server backend;
    std::atomic<bool> release_backend{false};
    std::atomic<bool> chunk1_seen{false};
    std::atomic<int> heartbeat_count{0};

    backend.Post("/v1/chat/completions",
        [&](const httplib::Request&, httplib::Response& res) {
            res.set_chunked_content_provider(
                "text/event-stream",
                [&](size_t, httplib::DataSink& sink) {
                    const std::string chunk1 = "data: {\"choices\":[{\"delta\":{\"content\":\"Chunk1\"}}]}\n\n";
                    sink.write(chunk1.data(), chunk1.size());

                    while (!chunk1_seen.load(std::memory_order_acquire)) {
                        if (release_backend.load(std::memory_order_acquire)) {
                            return false;
                        }
                        std::this_thread::sleep_for(10ms);
                    }

                    const auto deadline = std::chrono::steady_clock::now() + 5000ms;
                    while (heartbeat_count.load(std::memory_order_acquire) < 2 &&
                           std::chrono::steady_clock::now() < deadline) {
                        if (release_backend.load(std::memory_order_acquire)) {
                            return false;
                        }
                        std::this_thread::sleep_for(10ms);
                    }

                    const std::string chunk2 = "data: {\"choices\":[{\"delta\":{\"content\":\"Chunk2\"}}]}\n\n";
                    sink.write(chunk2.data(), chunk2.size());
                    std::this_thread::sleep_for(50ms);

                    const std::string done = "data: [DONE]\n\n";
                    sink.write(done.data(), done.size());
                    sink.done();
                    return false;
                });
        });

    const int port = backend.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::printf("[FAIL] failed to bind mock backend\n");
        ++g_failures;
        return;
    }

    std::thread backend_thread([&backend]() {
        backend.listen_after_bind();
    });
    backend.wait_until_ready();

    int chunk1_count = 0;
    int chunk2_count = 0;

    httplib::DataSink downstream;
    downstream.write = [&](const char* data, size_t len) {
        std::string s(data, len);
        if (s.find("Chunk1") != std::string::npos) {
            ++chunk1_count;
            chunk1_seen.store(true, std::memory_order_release);
        } else if (s.find("Chunk2") != std::string::npos) {
            ++chunk2_count;
        } else if (s == ": ping\n\n") {
            if (chunk1_seen.load(std::memory_order_acquire)) {
                heartbeat_count.fetch_add(1, std::memory_order_release);
            }
        }
        return true;
    };
    downstream.done = []() {};
    downstream.is_writable = []() { return true; };

    lemon::StreamingProxy::TelemetryData telemetry_result;
    lemon::StreamingProxy::forward_sse_stream(
        "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions",
        R"({"model":"test-model","stream":true})",
        downstream,
        [&](const lemon::StreamingProxy::TelemetryData& tel) {
            telemetry_result = tel;
        },
        10,
        nullptr,
        1000
    );

    release_backend.store(true, std::memory_order_release);
    backend.stop();
    backend_thread.join();

    check(chunk1_count == 1, "chunk 1 received before pause");
    check(heartbeat_count.load(std::memory_order_acquire) >= 2, "heartbeat comments emitted during pause between tokens (>= 2)");
    check(chunk2_count == 1, "chunk 2 received after pause");
    check(telemetry_result.error_message.empty(), "stream completed without error");
}

static void test_heartbeat_client_disconnect_aborts_upstream() {
    httplib::Server backend;
    std::atomic<bool> release_backend{false};

    backend.Post("/v1/chat/completions",
        [&](const httplib::Request&, httplib::Response& res) {
            res.set_chunked_content_provider(
                "text/event-stream",
                [&](size_t, httplib::DataSink&) {
                    while (!release_backend.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(10ms);
                    }
                    return false;
                });
        });

    const int port = backend.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::printf("[FAIL] failed to bind mock backend\n");
        ++g_failures;
        return;
    }

    std::thread backend_thread([&backend]() {
        backend.listen_after_bind();
    });
    backend.wait_until_ready();

    std::atomic<bool> downstream_writable{true};
    int heartbeat_count = 0;

    httplib::DataSink downstream;
    downstream.write = [&](const char* data, size_t len) {
        std::string s(data, len);
        if (s == ": ping\n\n") {
            ++heartbeat_count;
            downstream_writable.store(false, std::memory_order_release);
            return false;
        }
        return downstream_writable.load(std::memory_order_acquire);
    };
    downstream.done = []() {};
    downstream.is_writable = [&]() {
        return downstream_writable.load(std::memory_order_acquire);
    };

    std::string telemetry_error;
    const auto start = std::chrono::steady_clock::now();

    lemon::StreamingProxy::forward_sse_stream(
        "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions",
        R"({"model":"test-model","stream":true})",
        downstream,
        [&](const lemon::StreamingProxy::TelemetryData& tel) {
            telemetry_error = tel.error_message;
        },
        10,
        nullptr,
        1000
    );

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    release_backend.store(true, std::memory_order_release);
    backend.stop();
    backend_thread.join();

    check(heartbeat_count >= 1, "heartbeat was received by downstream");
    check(telemetry_error == "Client disconnected during stream", "disconnection during heartbeat aborted upstream");

    std::printf(
        "Heartbeat disconnect test completed in %lld ms\n",
        static_cast<long long>(elapsed.count()));
}

int main() {
    test_heartbeat_emission_during_prefill();
    test_heartbeat_emission_during_pause_between_tokens();
    test_heartbeat_client_disconnect_aborts_upstream();

    if (g_failures == 0) {
        std::printf("All streaming heartbeat tests passed.\n");
        return 0;
    }
    std::printf("%d streaming heartbeat test(s) failed.\n", g_failures);
    return 1;
}
