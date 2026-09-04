// Comprehensive test suite for client request cancellation.
//
// Covers:
// 1. End-to-end non-streaming cancellation via RequestCancelScope -> WrappedServer::forward_request()
// 2. Direct HttpClient::post cancellation throwing structured HttpClientCancellationException
// 3. StreamingProxy pre-first-byte SSE stream cancellation

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <httplib.h>
#include <curl/curl.h>

#include <lemon/streaming_proxy.h>
#include <lemon/utils/http_client.h>
#include <lemon/wrapped_server.h>

using namespace std::chrono_literals;

namespace {

class TestCancellationServer : public lemon::WrappedServer {
public:
    explicit TestCancellationServer(int port)
        : lemon::WrappedServer("test", "error", nullptr, nullptr) {
        port_ = port;
        set_state(lemon::ModelState::READY);
    }

    void load(const std::string&, const lemon::ModelInfo&, const lemon::RecipeOptions&, bool) override {}
    void unload() override {}
    bool is_backend_alive() const override { return true; }

    lemon::json test_forward(const std::string& endpoint, const lemon::json& request) {
        return forward_request(endpoint, request);
    }

    lemon::json test_forward_multipart(const std::string& endpoint,
                                       const std::vector<lemon::utils::MultipartField>& fields) {
        return forward_multipart_request(endpoint, fields);
    }
};

bool test_wrapped_server_non_streaming_cancel() {
    int failures = 0;
    auto check = [&failures](bool condition, const char* name) {
        if (condition) {
            std::printf("[PASS] %s\n", name);
        } else {
            std::printf("[FAIL] %s\n", name);
            ++failures;
        }
    };

    httplib::Server backend;
    std::atomic<bool> backend_started{false};
    std::atomic<bool> release_backend{false};

    backend.Post("/v1/chat/completions",
        [&](const httplib::Request&, httplib::Response& res) {
            backend_started.store(true, std::memory_order_release);
            const auto req_start = std::chrono::steady_clock::now();

            while (!release_backend.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(10ms);
                if (std::chrono::steady_clock::now() - req_start > 10s) {
                    break;
                }
            }

            res.set_content(R"({"choices":[{"message":{"content":"done"}}]})", "application/json");
        });

    const int port = backend.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::printf("[FAIL] failed to bind mock backend for non-streaming test\n");
        return false;
    }

    std::thread backend_thread([&backend]() {
        backend.listen_after_bind();
    });
    backend.wait_until_ready();

    TestCancellationServer server(port);
    std::atomic<bool> should_cancel{false};

    std::thread disconnect_thread([&]() {
        const auto wait_deadline = std::chrono::steady_clock::now() + 2s;
        while (!backend_started.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < wait_deadline) {
            std::this_thread::sleep_for(5ms);
        }

        std::this_thread::sleep_for(100ms);
        should_cancel.store(true, std::memory_order_release);
    });

    const auto started = std::chrono::steady_clock::now();
    lemon::json response;

    {
        lemon::WrappedServer::RequestCancelScope cancel_scope([&should_cancel]() {
            return should_cancel.load(std::memory_order_acquire);
        });

        response = server.test_forward(
            "/v1/chat/completions",
            lemon::json::parse(R"({"model":"test-model","messages":[{"role":"user","content":"hi"}]})"));
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    disconnect_thread.join();
    release_backend.store(true, std::memory_order_release);
    backend.stop();
    backend_thread.join();

    check(
        backend_started.load(std::memory_order_acquire),
        "WrappedServer: mock backend received the non-streaming request");
    check(
        response.contains("error") && response["error"].is_object(),
        "WrappedServer: forward_request returned error response on client disconnect");
    check(
        response.contains("error") &&
        response["error"].value("message", "") == "Request cancelled by client",
        "WrappedServer: cancellation error message is 'Request cancelled by client'");
    check(
        elapsed < 2000ms,
        "WrappedServer: non-streaming request aborted promptly without waiting for backend timeout");

    std::printf(
        "WrappedServer non-streaming cancellation completed in %lld ms\n",
        static_cast<long long>(elapsed.count()));

    return failures == 0;
}

bool test_wrapped_server_multipart_cancel() {
    int failures = 0;
    auto check = [&failures](bool condition, const char* name) {
        if (condition) {
            std::printf("[PASS] %s\n", name);
        } else {
            std::printf("[FAIL] %s\n", name);
            ++failures;
        }
    };

    httplib::Server backend;
    std::atomic<bool> backend_started{false};
    std::atomic<bool> release_backend{false};

    backend.Post("/v1/images/edits",
        [&](const httplib::Request&, httplib::Response& res) {
            backend_started.store(true, std::memory_order_release);
            const auto req_start = std::chrono::steady_clock::now();

            while (!release_backend.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(10ms);
                if (std::chrono::steady_clock::now() - req_start > 10s) {
                    break;
                }
            }

            res.set_content(R"({"data":[{"b64_json":"abc"}]})", "application/json");
        });

    const int port = backend.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::printf("[FAIL] failed to bind mock backend for multipart test\n");
        return false;
    }

    std::thread backend_thread([&backend]() {
        backend.listen_after_bind();
    });
    backend.wait_until_ready();

    TestCancellationServer server(port);
    std::atomic<bool> should_cancel{false};

    std::thread disconnect_thread([&]() {
        const auto wait_deadline = std::chrono::steady_clock::now() + 2s;
        while (!backend_started.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < wait_deadline) {
            std::this_thread::sleep_for(5ms);
        }

        std::this_thread::sleep_for(100ms);
        should_cancel.store(true, std::memory_order_release);
    });

    const auto started = std::chrono::steady_clock::now();
    lemon::json response;

    {
        lemon::WrappedServer::RequestCancelScope cancel_scope([&should_cancel]() {
            return should_cancel.load(std::memory_order_acquire);
        });

        std::vector<lemon::utils::MultipartField> fields = {
            {"prompt", "test prompt", "", ""},
            {"image", "dummy_bytes", "image.png", "image/png"}
        };
        response = server.test_forward_multipart("/v1/images/edits", fields);
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    disconnect_thread.join();
    release_backend.store(true, std::memory_order_release);
    backend.stop();
    backend_thread.join();

    check(
        backend_started.load(std::memory_order_acquire),
        "WrappedServer multipart: mock backend received the multipart request");
    check(
        response.contains("error") && response["error"].is_object(),
        "WrappedServer multipart: forward_multipart_request returned error response on client disconnect");
    check(
        response.contains("error") &&
        response["error"].value("message", "") == "Request cancelled by client",
        "WrappedServer multipart: cancellation error message is 'Request cancelled by client'");
    check(
        elapsed < 2000ms,
        "WrappedServer multipart: multipart request aborted promptly without waiting for backend timeout");

    std::printf(
        "WrappedServer multipart cancellation completed in %lld ms\n",
        static_cast<long long>(elapsed.count()));

    return failures == 0;
}

bool test_http_client_direct_cancellation() {
    int failures = 0;
    auto check = [&failures](bool condition, const char* name) {
        if (condition) {
            std::printf("[PASS] %s\n", name);
        } else {
            std::printf("[FAIL] %s\n", name);
            ++failures;
        }
    };

    httplib::Server backend;
    std::atomic<bool> backend_started{false};
    std::atomic<bool> release_backend{false};

    backend.Post("/v1/chat/completions",
        [&](const httplib::Request&, httplib::Response& res) {
            backend_started.store(true, std::memory_order_release);
            const auto req_start = std::chrono::steady_clock::now();

            while (!release_backend.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(10ms);
                if (std::chrono::steady_clock::now() - req_start > 10s) {
                    break;
                }
            }

            res.set_content(R"({"choices":[{"message":{"content":"done"}}]})", "application/json");
        });

    const int port = backend.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::printf("[FAIL] failed to bind mock backend for http_client test\n");
        return false;
    }

    std::thread backend_thread([&backend]() {
        backend.listen_after_bind();
    });
    backend.wait_until_ready();

    std::atomic<bool> should_cancel{false};

    std::thread disconnect_thread([&]() {
        const auto wait_deadline = std::chrono::steady_clock::now() + 2s;
        while (!backend_started.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < wait_deadline) {
            std::this_thread::sleep_for(5ms);
        }

        std::this_thread::sleep_for(100ms);
        should_cancel.store(true, std::memory_order_release);
    });

    const auto started = std::chrono::steady_clock::now();
    bool caught_structured_cancellation = false;
    int caught_curl_code = 0;

    try {
        lemon::utils::HttpClient::post(
            "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions",
            R"({"model":"test-model","messages":[{"role":"user","content":"hi"}]})",
            {{"Content-Type", "application/json"}},
            10,
            lemon::utils::HttpSecurityPolicy::TrustedLoopback,
            nullptr,
            [&should_cancel]() {
                return should_cancel.load(std::memory_order_acquire);
            });
    } catch (const lemon::utils::HttpClientCancellationException& e) {
        caught_structured_cancellation = true;
        caught_curl_code = e.curl_code();
    } catch (const std::exception& e) {
        std::printf("[FAIL] unexpected exception type: %s\n", e.what());
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    disconnect_thread.join();
    release_backend.store(true, std::memory_order_release);
    backend.stop();
    backend_thread.join();

    check(
        backend_started.load(std::memory_order_acquire),
        "HttpClient: mock backend received the request");
    check(
        caught_structured_cancellation,
        "HttpClient: threw HttpClientCancellationException on client disconnect");
    check(
        caught_curl_code == CURLE_ABORTED_BY_CALLBACK,
        "HttpClient: preserved CURLE_ABORTED_BY_CALLBACK curl_code");
    check(
        elapsed < 2000ms,
        "HttpClient: post aborted promptly (~1s)");

    std::printf(
        "HttpClient direct cancellation completed in %lld ms\n",
        static_cast<long long>(elapsed.count()));

    return failures == 0;
}

bool test_streaming_proxy_cancel() {
    int failures = 0;
    auto check = [&failures](bool condition, const char* name) {
        if (condition) {
            std::printf("[PASS] %s\n", name);
        } else {
            std::printf("[FAIL] %s\n", name);
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
                    while (!release_backend.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(10ms);
                    }
                    return false;
                });
        });

    const int port = backend.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::printf("[FAIL] failed to bind mock backend for streaming test\n");
        return false;
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
        "StreamingProxy: mock backend received the streaming request");
    check(
        elapsed < 3000ms,
        "StreamingProxy: client disconnect cancels upstream before first stream byte");
    check(
        telemetry_error == "Client disconnected during stream",
        "StreamingProxy: early cancellation is classified as a client disconnect");
    check(
        downstream_write_calls == 0,
        "StreamingProxy: no payload is written after a pre-first-byte disconnect");

    std::printf(
        "StreamingProxy pre-first-byte cancellation completed in %lld ms\n",
        static_cast<long long>(elapsed.count()));

    return failures == 0;
}

} // namespace

int main() {
    bool ok = true;
    std::printf("=== Running Request Cancellation Tests ===\n");

    ok = test_wrapped_server_non_streaming_cancel() && ok;
    ok = test_wrapped_server_multipart_cancel() && ok;
    ok = test_http_client_direct_cancellation() && ok;
    ok = test_streaming_proxy_cancel() && ok;

    if (ok) {
        std::printf("All request cancellation tests passed!\n");
        return 0;
    }
    std::printf("Some request cancellation tests failed!\n");
    return 1;
}
