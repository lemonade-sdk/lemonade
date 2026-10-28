// Unit tests for the timeout convention shared by lemon::utils::HttpClient's
// request methods. A loopback handler that never responds stands in for a
// silent upstream.
//
// The regression these guard: curl reads CURLOPT_TIMEOUT 0 as "no timeout", so
// a method that forwarded 0 straight through blocked forever and parked the
// calling httplib worker with it.
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release CI build, where -DNDEBUG no-ops assert().

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>

#include <lemon/utils/http_client.h>

using lemon::utils::DownloadOptions;
using lemon::utils::HttpClient;
using lemon::utils::HttpSecurityPolicy;
using lemon::utils::MultipartField;

struct TestResult {
    int passed = 0;
    int failed = 0;

    void check(bool cond, const std::string& name) {
        if (cond) {
            printf("[PASS] %s\n", name.c_str());
            ++passed;
        } else {
            printf("[FAIL] %s\n", name.c_str());
            ++failed;
        }
    }
};

namespace {

// Wall-clock ceiling for a call expected to give up on its own. Generous
// against the 2s timeouts below so a loaded CI machine does not flake, but far
// short of the "never returns" behavior being guarded against.
constexpr int kGiveUpCeilingSeconds = 30;

// Runs fn on a detached thread and reports whether it finished within the
// ceiling. The regression under test is an unbounded block, so calling fn
// directly would hang the test process instead of failing it. A detached
// thread (not std::async, whose future blocks in its destructor) means a stuck
// call cannot hold up the verdict.
template <typename Fn>
bool returns_within_ceiling(Fn&& fn) {
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    std::thread([fn = std::forward<Fn>(fn), done]() mutable {
        try {
            fn();
        } catch (const std::exception&) {
            // A timeout surfaces as a CURL error; how it is reported is the
            // caller's business, only that it returned matters here.
        }
        done->set_value();
    }).detach();
    return finished.wait_for(std::chrono::seconds(kGiveUpCeilingSeconds)) ==
           std::future_status::ready;
}

}  // namespace

int main() {
    TestResult r;
    printf("=== HttpClient timeout convention Unit Tests ===\n\n");

    httplib::Server svr;

    const int port = svr.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        printf("[FAIL] failed to bind loopback test server\n");
        return 1;
    }

    // Accepts the request and then holds it, sending nothing. The wait is
    // polled rather than one long sleep so shutdown does not block on it.
    std::atomic<bool> shutting_down{false};
    // Download endpoint that sends response headers but stalls before body data.
    svr.Get("/download-silent", [&shutting_down](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider(
            "application/octet-stream", [&shutting_down](size_t, httplib::DataSink&) {
                for (int i = 0; i < kGiveUpCeilingSeconds * 20 && !shutting_down; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                return false;
            });
    });
    svr.Post("/silent", [&shutting_down](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider(
            "text/event-stream", [&shutting_down](size_t, httplib::DataSink&) {
                for (int i = 0; i < kGiveUpCeilingSeconds * 20 && !shutting_down; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                return false;
            });
    });

    // This response takes longer than the short explicit timeout tests below.
    // NoTimeout callers must still receive the completed payload.
    svr.Post("/delayed", [](const httplib::Request&, httplib::Response& res) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2300));
        res.set_content("ok", "text/plain");
    });
    svr.Post("/trickle", [](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider("text/plain", [](size_t offset, httplib::DataSink& sink) {
            if (offset >= 5) {
                sink.done();
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            return sink.write("x", 1);
        });
    });

    std::thread server_thread([&svr]() { svr.listen_after_bind(); });
    svr.wait_until_ready();

    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    const auto policy = HttpSecurityPolicy::TrustedLoopback;

    // An explicit timeout is honored.
    {
        r.check(returns_within_ceiling([&] {
                    HttpClient::post(base + "/silent", "{}", {}, 2, policy);
                }),
                "post: explicit timeout abandons a silent upstream");
    }

    // Control-plane calls still have a bounded default, but inference opts out.
    r.check(HttpClient::kDefaultRequestTimeoutSeconds > 0,
            "control-plane request default remains bounded");
    r.check(HttpClient::kDefaultInferenceInactivityTimeoutSeconds >
                HttpClient::kDefaultRequestTimeoutSeconds,
            "uncapped inference retains a separate, longer inactivity guard");

    r.check(returns_within_ceiling([&] {
                HttpClient::post_multipart(base + "/silent",
                    {MultipartField{"f", "v", "", ""}}, 2, policy);
            }), "post_multipart: explicit deadline bounds a silent upstream");

    try {
        auto response = HttpClient::post(base + "/delayed", "{}", {},
                                          HttpClient::kNoTimeout, policy);
        r.check(response.status_code == 200 && response.body == "ok",
                "post: uncapped inference completes after delayed response");
    } catch (const std::exception& e) {
        printf("[FAIL] uncapped post threw: %s\n", e.what());
        ++r.failed;
    }

    try {
        auto response = HttpClient::post_multipart(
            base + "/delayed", {MultipartField{"f", "v", "", ""}},
            HttpClient::kNoTimeout, policy);
        r.check(response.status_code == 200 && response.body == "ok",
                "post_multipart: uncapped inference completes after delay");
    } catch (const std::exception& e) {
        printf("[FAIL] uncapped multipart threw: %s\n", e.what());
        ++r.failed;
    }

    // post_stream bounds silence rather than total duration, so a default-timeout
    // fallback would wrongly cut off a long healthy generation. An explicit
    // timeout still applies.
    {
        r.check(returns_within_ceiling([&] {
                    HttpClient::post_stream(
                        base + "/silent", "{}",
                        [](const char*, size_t) { return true; },
                        {}, 2, nullptr, policy);
                }),
                "post_stream: explicit timeout abandons a silent upstream");
    }

    try {
        std::string body;
        auto response = HttpClient::post_stream(
            base + "/delayed", "{}",
            [&body](const char* data, size_t len) {
                body.append(data, len);
                return true;
            }, {}, HttpClient::kNoTimeout, nullptr, policy);
        r.check(response.status_code == 200 && response.curl_code == 0 && body == "ok",
                "post_stream: uncapped first-token wait completes after delay");
    } catch (const std::exception& e) {
        printf("[FAIL] uncapped stream threw: %s\n", e.what());
        ++r.failed;
    }

    try {
        std::string body;
        auto response = HttpClient::post_stream(
            base + "/trickle", "{}",
            [&body](const char* data, size_t len) {
                body.append(data, len);
                return true;
            }, {}, 2, nullptr, policy);
        r.check(response.status_code == 200 && response.curl_code == 0 && body == "xxxxx",
                "post_stream: sparse received bytes reset the inactivity deadline");
    } catch (const std::exception& e) {
        printf("[FAIL] sparse stream threw: %s\n", e.what());
        ++r.failed;
    }

    // The sentinel is the only way to ask for an unbounded transfer, so it must
    // not be confused with 0.
    r.check(HttpClient::kNoTimeout != 0,
            "kNoTimeout is distinct from the 0-means-default sentinel");

    {
        DownloadOptions options;
        options.max_retries = 0;
        options.no_progress_timeout = 2;
        const std::string path = "timeout-download-" + std::to_string(port) + ".bin";
        std::remove(path.c_str());
        std::remove((path + ".partial").c_str());
        const auto result = HttpClient::download_file(
            base + "/download-silent", path, nullptr, {}, options, policy);
        r.check(!result.success &&
                result.error_message.find("no bytes received for 2 seconds") != std::string::npos,
                "download: no-progress threshold also applies before the first body byte");
        std::remove(path.c_str());
        std::remove((path + ".partial").c_str());
    }

    shutting_down = true;
    svr.stop();
    server_thread.join();

    printf("\n=== %d passed, %d failed ===\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
