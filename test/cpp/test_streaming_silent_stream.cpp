#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <httplib.h>
#include <lemon/streaming_proxy.h>
#include <lemon/utils/http_client.h>

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
    std::string downstream;
    std::string error_message;
    bool done_called = false;
};

// Runs one SSE proxy request against a mock backend that answers 200
// text/event-stream with backend_body, and reports what reached the client.
StreamResult run_proxy(const std::string& backend_body) {
    httplib::Server backend;
    backend.Post("/v1/chat/completions",
        [&](const httplib::Request&, httplib::Response& res) {
            res.set_chunked_content_provider(
                "text/event-stream",
                [&](size_t, httplib::DataSink& sink) {
                    if (!backend_body.empty()) {
                        sink.write(backend_body.data(), backend_body.size());
                    }
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

size_t count(const std::string& haystack, const std::string& needle) {
    size_t total = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++total;
    }
    return total;
}

// A stream that delivered an event is forwarded as the backend sent it and
// terminated with [DONE], never rewritten into an error.
void expect_accepted(const std::string& name, const std::string& backend_body,
                     const std::string& content) {
    const StreamResult result = run_proxy(backend_body);

    if (!content.empty()) {
        check(contains(result.downstream, content), name + ": content is forwarded");
    }
    check(contains(result.downstream, "data: [DONE]"), name + ": stream ends with [DONE]");
    check(!contains(result.downstream, "\"error\""), name + ": no error event");
    check(result.error_message.empty(), name + ": telemetry reports success");
}

// A stream that delivered nothing produced no response at all, so the client
// gets a framed error event rather than a [DONE] reporting empty success.
void expect_rejected(const std::string& name, const std::string& backend_body) {
    const StreamResult result = run_proxy(backend_body);

    check(contains(result.downstream, "data: {"), name + ": client receives a framed data event");
    check(contains(result.downstream, "\"error\""), name + ": event carries an error object");
    check(!contains(result.downstream, "[DONE]"), name + ": no [DONE] claims success");
    check(!result.error_message.empty(), name + ": telemetry records the failure");
    check(result.done_called, name + ": response is terminated");
}

const std::string kEvent = R"(data: {"choices":[{"delta":{"content":"Hello"}}]})";

}  // namespace

static void test_delivered_events_are_forwarded() {
    expect_accepted("LF terminators", kEvent + "\n\n", "Hello");

    // SSE makes the space after the colon optional.
    expect_accepted("no space after the colon",
                    R"(data:{"choices":[{"delta":{"content":"Hello"}}]})"
                    "\n\n",
                    "Hello");

    // SSE also ends lines with CRLF or a bare CR.
    expect_accepted("CR terminators", kEvent + "\r\r", "Hello");

    // A field name with no colon carries an empty value, so "data" alone still
    // completes an event.
    expect_accepted("bare data field", "data\n\n", "");

    // This backend prefix is not SSE: it has no blank-line terminator, and like
    // any field its value need not be preceded by a space.
    expect_accepted("ChatCompletionChunk without a space",
                    R"(ChatCompletionChunk:{"choices":[{"delta":{"content":"Hello"}}]})"
                    "\n",
                    "Hello");
}

static void test_streams_that_delivered_nothing_are_errors() {
    expect_rejected("empty stream", "");

    // Backends log their own failures into the already committed 200 stream.
    // Those bytes are not SSE, so the client still needs a framed error event.
    expect_rejected("unframed backend output", "backend: model arena alloc failed: out of memory\n");

    // A data field only reaches the client once its blank line arrives, so a
    // stream cut off before that showed nothing.
    expect_rejected("unterminated event", kEvent + "\n");

    // A comment is not an event, so the heartbeat must not pass for a response.
    expect_rejected("comment only", ": ping\n\n");
}

// A backend that sent its own [DONE] has already terminated the stream.
static void test_backend_done_marker_is_not_duplicated() {
    const StreamResult result = run_proxy(kEvent + "\n\n" + "data: [DONE]\n\n");

    check(contains(result.downstream, "Hello"), "backend [DONE]: content is forwarded");
    check(count(result.downstream, "data: [DONE]") == 1, "backend [DONE]: marker is not duplicated");
    check(!contains(result.downstream, "\"error\""), "backend [DONE]: no error event");
    check(result.error_message.empty(), "backend [DONE]: telemetry reports success");
}

// Chunk boundaries are arbitrary, so a CRLF may be split between two reads. The
// parser has to wait for the LF instead of reporting an extra blank line, which
// would terminate the event before its data field arrived.
static void test_line_parser_handles_every_terminator() {
    std::vector<std::string> lines;
    auto collect = [&lines](const std::string& line) { lines.push_back(line); };

    std::string buffer = "lf\ncrlf\r\ncr\rsplit\r";
    lemon::StreamingProxy::process_sse_lines(buffer, collect);
    check(lines == std::vector<std::string>({"lf", "crlf", "cr"}),
          "line parser: LF, CRLF and CR each end a line");
    check(buffer == "split\r", "line parser: a trailing CR is held for its possible LF");

    buffer += "\nlast\r";
    lemon::StreamingProxy::process_sse_lines(buffer, collect);
    check(lines == std::vector<std::string>({"lf", "crlf", "cr", "split"}),
          "line parser: a CRLF split across chunks is one terminator");

    lemon::StreamingProxy::process_sse_lines(buffer, collect, true);
    check(lines == std::vector<std::string>({"lf", "crlf", "cr", "split", "last"}),
          "line parser: end of stream releases the held CR");
}

static void test_cancellation_during_backend_silence(bool sse, bool predicate_only) {
    using namespace std::chrono_literals;
    httplib::Server backend;
    std::atomic<bool> started{false};
    std::atomic<bool> release_backend{false};
    std::atomic<bool> cancelled{false};
    backend.Post("/stream", [&](const httplib::Request&, httplib::Response& res) {
        started.store(true);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!release_backend.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
        res.set_content("", sse ? "text/event-stream" : "audio/wav");
    });
    const int port = backend.bind_to_any_port("127.0.0.1");
    check(port > 0, "cancellation: mock backend bound");
    if (port <= 0) return;
    std::thread backend_thread([&]() { backend.listen_after_bind(); });
    backend.wait_until_ready();
    std::thread interrupter([&]() {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!started.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
        std::this_thread::sleep_for(100ms);
        cancelled.store(true);
    });

    lemon::utils::CancelToken cancel;
    if (predicate_only) {
        cancel.should_cancel = [&]() { return cancelled.load(); };
    } else {
        cancel.flag = &cancelled;
    }
    std::string output;
    httplib::DataSink sink;
    sink.write = [&](const char* data, size_t length) {
        output.append(data, length);
        return true;
    };
    sink.done = []() {};
    sink.is_writable = []() { return true; };
    const auto start = std::chrono::steady_clock::now();
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/stream";
    if (sse) {
        lemon::StreamingProxy::forward_sse_stream(url, "{}", sink, nullptr, 10, nullptr, 0, cancel);
    } else {
        lemon::StreamingProxy::forward_byte_stream(url, "{}", sink, 10, nullptr, cancel);
    }
    const bool cancelled_at_return = cancelled.load();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const bool returned_during_silence = !release_backend.exchange(true) && elapsed < 3s;
    interrupter.join();
    backend.stop();
    backend_thread.join();
    const std::string name = std::string(sse ? "SSE" : "byte stream") +
                             (predicate_only ? " predicate" : " flag");
    check(started.load(), name + ": request reached silent backend");
    check(cancelled_at_return && returned_during_silence, name + ": cancellation stops silent transfer");
    if (!sse) check(output.empty(), name + ": cancellation sends no backend-error payload");
}

static void test_get_cancellation_during_backend_silence(bool predicate_only) {
    using namespace std::chrono_literals;
    httplib::Server backend;
    std::mutex mutex;
    std::condition_variable ready;
    bool started = false;
    bool release_backend = false;
    std::atomic<bool> cancelled{false};
    backend.Get("/silent", [&](const httplib::Request&, httplib::Response& res) {
        std::unique_lock<std::mutex> lock(mutex);
        started = true;
        ready.notify_all();
        ready.wait(lock, [&]() { return release_backend; });
        res.set_content("done", "text/plain");
    });
    const int port = backend.bind_to_any_port("127.0.0.1");
    check(port > 0, "GET cancellation: mock backend bound");
    if (port <= 0) return;
    std::thread backend_thread([&]() { backend.listen_after_bind(); });
    backend.wait_until_ready();
    bool accepted_before_cancel = false;
    std::chrono::steady_clock::time_point cancelled_at;
    std::thread interrupter([&]() {
        std::unique_lock<std::mutex> lock(mutex);
        accepted_before_cancel = ready.wait_for(lock, 20s, [&]() { return started; });
        cancelled_at = std::chrono::steady_clock::now();
        cancelled.store(true);
    });

    lemon::utils::CancelToken cancel;
    if (predicate_only) {
        cancel.should_cancel = [&]() { return cancelled.load(); };
    } else {
        cancel.flag = &cancelled;
    }
    bool caught_cancellation = false;
    int curl_code = CURLE_OK;
    try {
        lemon::utils::HttpClient::get(
            "http://127.0.0.1:" + std::to_string(port) + "/silent", {}, 60,
            lemon::utils::HttpSecurityPolicy::TrustedLoopback, 0, cancel);
    } catch (const lemon::utils::HttpClientCancellationException& e) {
        caught_cancellation = true;
        curl_code = e.curl_code();
    } catch (const std::exception& e) {
        std::printf("[FAIL] GET cancellation: unexpected exception: %s\n", e.what());
        ++g_failures;
    }
    const auto returned_at = std::chrono::steady_clock::now();
    interrupter.join();
    {
        std::lock_guard<std::mutex> lock(mutex);
        release_backend = true;
    }
    ready.notify_all();
    backend.stop();
    backend_thread.join();

    const std::string name = predicate_only ? "GET predicate" : "GET flag";
    check(accepted_before_cancel, name + ": request reached silent backend before cancellation");
    check(caught_cancellation, name + ": threw HttpClientCancellationException");
    check(curl_code == CURLE_ABORTED_BY_CALLBACK, name + ": preserved cancellation curl code");
    check(returned_at >= cancelled_at && returned_at - cancelled_at < 10s,
          name + ": returned within 10s of cancellation with a 60s transfer timeout");
    std::printf("%s cancellation completed in %lld ms\n", name.c_str(),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    returned_at - cancelled_at).count()));
}

int main() {
    test_get_cancellation_during_backend_silence(false);
    test_get_cancellation_during_backend_silence(true);
    test_delivered_events_are_forwarded();
    test_streams_that_delivered_nothing_are_errors();
    test_backend_done_marker_is_not_duplicated();
    test_line_parser_handles_every_terminator();
    test_cancellation_during_backend_silence(false, false);
    test_cancellation_during_backend_silence(false, true);
    test_cancellation_during_backend_silence(true, false);
    test_cancellation_during_backend_silence(true, true);

    if (g_failures == 0) {
        std::printf("All silent stream tests passed.\n");
        return 0;
    }
    std::printf("%d silent stream test(s) failed.\n", g_failures);
    return 1;
}
