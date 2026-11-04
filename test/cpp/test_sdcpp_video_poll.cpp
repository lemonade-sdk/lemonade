#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include <httplib.h>

#include <lemon/backends/sdcpp/sdcpp_server.h>

namespace {

int g_failures = 0;

void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

class FakeSdServer : public lemon::backends::SDServer {
public:
    explicit FakeSdServer(int port) : lemon::backends::SDServer("error", nullptr, nullptr) {
        port_ = port;
        set_state(lemon::ModelState::READY);
    }

    bool is_backend_alive() const override { return true; }
};

// Answers the first failing_polls polls with a 500, then reports the clip done.
struct FakeBackend {
    httplib::Server server;
    std::thread thread;
    std::atomic<int> submits{0};
    std::atomic<int> polls{0};
    int port = 0;

    explicit FakeBackend(int failing_polls) {
        server.Post("/sdcpp/v1/vid_gen", [this](const httplib::Request&, httplib::Response& res) {
            ++submits;
            res.status = 202;
            res.set_content(R"({"id":"j1","poll_url":"/sdcpp/v1/jobs/j1"})", "application/json");
        });
        server.Get("/sdcpp/v1/jobs/j1", [this, failing_polls](const httplib::Request&,
                                                              httplib::Response& res) {
            if (++polls <= failing_polls) {
                res.status = 500;
                res.set_content(R"({"error":"busy"})", "application/json");
                return;
            }
            res.set_content(
                R"({"id":"j1","status":"completed","error":null,"result":{"b64_json":"AAAA"}})",
                "application/json");
        });
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }

    ~FakeBackend() {
        server.stop();
        thread.join();
    }
};

lemon::json request() {
    return {{"prompt", "a kite"}, {"width", 480}, {"height", 272}, {"video_frames", 17}};
}

void test_a_few_failed_polls_are_tolerated() {
    FakeBackend backend(2);
    FakeSdServer sd(backend.port);
    const lemon::json result = sd.video_generations(request());
    check("failed polls: two in a row do not fail the video", !result.contains("error"));
    check("failed polls: the clip still comes back", result.value("b64_json", "") == "AAAA");
}

void test_persistent_failed_polls_give_up() {
    FakeBackend backend(1000);
    FakeSdServer sd(backend.port);
    const lemon::json result = sd.video_generations(request());
    check("failed polls: persistent failures fail the video", result.contains("error"));
    check("failed polls: it gives up after five", backend.polls.load() == 5);
}

void test_cancel_before_submit_skips_the_backend() {
    FakeBackend backend(0);
    FakeSdServer sd(backend.port);
    std::atomic<bool> cancel{true};
    const lemon::json result = sd.video_generations(request(), &cancel);
    check("early cancel: the video is canceled", result.contains("error"));
    check("early cancel: sd-server never sees the clip", backend.submits.load() == 0);
}

} // namespace

int main() {
    test_a_few_failed_polls_are_tolerated();
    test_persistent_failed_polls_give_up();
    test_cancel_before_submit_skips_the_backend();
    if (g_failures) {
        std::printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("ALL PASS (0 failures)\n");
    return 0;
}
