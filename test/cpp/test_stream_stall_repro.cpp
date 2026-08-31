// Reproduction harness for issue #3386: post_stream(timeout_seconds=0) must not
// abort a healthy-but-silent stream at a hard-coded 120s, and its silence bound
// must track the configured default timeout (global_timeout).
//
// Unlike the wall-clock-ceiling checks in test_http_client_timeout.cpp, this
// measures the elapsed time so it can tell "some bound fired" apart from "the
// configured bound fired".

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include <httplib.h>

#include <lemon/utils/http_client.h>

using lemon::utils::HttpClient;
using lemon::utils::HttpSecurityPolicy;

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool cond, const std::string& name) {
    printf(cond ? "[PASS] %s\n" : "[FAIL] %s\n", name.c_str());
    cond ? ++g_passed : ++g_failed;
}

// Runs post_stream(timeout=0) against the silent endpoint and returns how long
// it took, or -1 if it was still running after `budget`.
double time_silent_stream(const std::string& url, long default_timeout, int budget_seconds) {
    const long saved = HttpClient::get_default_timeout();
    HttpClient::set_default_timeout(default_timeout);

    auto elapsed = std::make_shared<std::promise<double>>();
    auto future = elapsed->get_future();
    std::thread([url, elapsed]() {
        const auto start = std::chrono::steady_clock::now();
        try {
            HttpClient::post_stream(
                url, "{}", [](const char*, size_t) { return true; }, {}, 0, nullptr,
                HttpSecurityPolicy::TrustedLoopback);
        } catch (const std::exception&) {
        }
        elapsed->set_value(std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count());
    }).detach();

    const bool done = future.wait_for(std::chrono::seconds(budget_seconds)) ==
                      std::future_status::ready;
    HttpClient::set_default_timeout(saved);
    return done ? future.get() : -1.0;
}

}  // namespace

int main() {
    printf("=== post_stream silence-bound reproduction (issue #3386) ===\n\n");

    httplib::Server svr;
    const int port = svr.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        printf("[FAIL] failed to bind loopback test server\n");
        return 1;
    }

    // Stands in for llama-server during a long prefill: the connection is open
    // and the backend is alive, but no SSE bytes are produced yet.
    std::atomic<bool> shutting_down{false};
    svr.Post("/silent", [&shutting_down](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider(
            "text/event-stream", [&shutting_down](size_t, httplib::DataSink&) {
                for (int i = 0; i < 3000 && !shutting_down; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                return false;
            });
    });

    std::thread server_thread([&svr]() { svr.listen_after_bind(); });
    svr.wait_until_ready();
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/silent";

    // 1+2. The bound tracks global_timeout rather than a fixed constant.
    for (const long configured : {5L, 12L}) {
        const double took = time_silent_stream(url, configured, configured + 20);
        printf("       global_timeout=%lds -> post_stream returned after %.1fs\n",
               configured, took);
        check(took > configured * 0.5 && took < configured + 8.0,
              "silence bound follows global_timeout=" + std::to_string(configured) + "s");
    }

    // 3. The reported regression: a 130s silent prefill under the shipped
    //    600s default must survive the old hard-coded 120s abort.
    {
        const double took = time_silent_stream(url, 600, 130);
        printf("       global_timeout=600s, 130s of silence -> %s\n",
               took < 0 ? "still streaming (correct)"
                        : ("aborted after " + std::to_string(took) + "s").c_str());
        check(took < 0, "130s of silence survives under the 600s default (issue #3386)");
    }

    shutting_down = true;
    svr.stop();
    server_thread.join();

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
