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
    svr.Post("/silent", [&shutting_down](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider(
            "text/event-stream", [&shutting_down](size_t, httplib::DataSink&) {
                for (int i = 0; i < kGiveUpCeilingSeconds * 20 && !shutting_down; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                return false;
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

    // 0 means "use the configured default", matching get(). Before the fix this
    // call never returned.
    {
        const long saved = HttpClient::get_default_timeout();
        HttpClient::set_default_timeout(2);
        const bool returned = returns_within_ceiling([&] {
            HttpClient::post(base + "/silent", "{}", {}, 0, policy);
        });
        HttpClient::set_default_timeout(saved);
        r.check(returned,
                "post: timeout 0 falls back to the default rather than blocking forever");
    }

    {
        const long saved = HttpClient::get_default_timeout();
        HttpClient::set_default_timeout(2);
        std::vector<MultipartField> fields;
        MultipartField field;
        field.name = "f";
        field.data = "v";
        fields.push_back(field);
        const bool returned = returns_within_ceiling([&] {
            HttpClient::post_multipart(base + "/silent", fields, 0, policy);
        });
        HttpClient::set_default_timeout(saved);
        r.check(returned,
                "post_multipart: timeout 0 falls back to the default");
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

    // timeout_seconds=0 bounds silence with the configured default timeout.
    {
        const long saved = HttpClient::get_default_timeout();
        HttpClient::set_default_timeout(2);
        const bool returned = returns_within_ceiling([&] {
            HttpClient::post_stream(
                base + "/silent", "{}",
                [](const char*, size_t) { return true; },
                {}, 0, nullptr, policy);
        });
        HttpClient::set_default_timeout(saved);
        r.check(returned,
                "post_stream: timeout 0 bounds silence with the default timeout");
    }

    // The sentinel is the only way to ask for an unbounded transfer, so it must
    // not be confused with 0.
    r.check(HttpClient::kNoTimeout != 0,
            "kNoTimeout is distinct from the 0-means-default sentinel");

    shutting_down = true;
    svr.stop();
    server_thread.join();

    printf("\n=== %d passed, %d failed ===\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
