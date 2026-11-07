#include <atomic>
#include <chrono>
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

// Accepts the clip, then never answers a poll or a cancel until released.
struct StalledBackend {
    httplib::Server server;
    std::thread thread;
    std::atomic<bool> released{false};
    std::atomic<int> cancels{0};
    int port = 0;

    StalledBackend() {
        server.Post("/sdcpp/v1/vid_gen", [](const httplib::Request&, httplib::Response& res) {
            res.status = 202;
            res.set_content(R"({"id":"j1","poll_url":"/sdcpp/v1/jobs/j1"})", "application/json");
        });
        server.Get("/sdcpp/v1/jobs/j1", [this](const httplib::Request&, httplib::Response& res) {
            stall();
            res.set_content(R"({"id":"j1","status":"generating","error":null})",
                            "application/json");
        });
        server.Post("/sdcpp/v1/jobs/j1/cancel", [this](const httplib::Request&,
                                                       httplib::Response& res) {
            ++cancels;
            stall();
            res.set_content(R"({"id":"j1","status":"cancelled"})", "application/json");
        });
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }

    void stall() {
        while (!released) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    ~StalledBackend() {
        released = true;
        server.stop();
        thread.join();
    }
};

// Never answers the clip submission until released.
struct StalledSubmitBackend {
    httplib::Server server;
    std::thread thread;
    std::atomic<bool> released{false};
    std::atomic<int> submits{0};
    int port = 0;

    StalledSubmitBackend() {
        server.Post("/sdcpp/v1/vid_gen", [this](const httplib::Request&, httplib::Response& res) {
            ++submits;
            while (!released) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            res.status = 202;
            res.set_content(R"({"id":"j1","poll_url":"/sdcpp/v1/jobs/j1"})", "application/json");
        });
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }

    ~StalledSubmitBackend() {
        released = true;
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

void test_cancel_is_not_held_by_a_stalled_backend() {
    StalledBackend backend;
    FakeSdServer sd(backend.port);
    std::atomic<bool> cancel{false};
    std::thread canceler([&cancel] {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        cancel = true;
    });
    const auto start = std::chrono::steady_clock::now();
    const lemon::json result = sd.video_generations(request(), &cancel);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    canceler.join();
    check("stalled backend: the video is canceled", result.contains("error"));
    check("stalled backend: the cancel was sent", backend.cancels.load() == 1);
    check("stalled backend: one poll and one cancel timeout, not the 300 s default",
          elapsed < std::chrono::seconds(20));
}

void test_cancel_is_not_held_by_a_stalled_submit() {
    StalledSubmitBackend backend;
    FakeSdServer sd(backend.port);
    std::atomic<bool> cancel{false};
    std::thread canceler([&cancel] {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        cancel = true;
    });
    const auto start = std::chrono::steady_clock::now();
    const lemon::json result = sd.video_generations(request(), &cancel);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    canceler.join();
    check("stalled submit: sd-server got the clip", backend.submits.load() == 1);
    check("stalled submit: the video is canceled",
          result.contains("error") && result.dump().find("canceled") != std::string::npos);
    check("stalled submit: the cancel is not held by the 300 s default",
          elapsed < std::chrono::seconds(20));
}

} // namespace

int main() {
    test_a_few_failed_polls_are_tolerated();
    test_persistent_failed_polls_give_up();
    test_cancel_before_submit_skips_the_backend();
    test_cancel_is_not_held_by_a_stalled_backend();
    test_cancel_is_not_held_by_a_stalled_submit();
    if (g_failures) {
        std::printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("ALL PASS (0 failures)\n");
    return 0;
}
