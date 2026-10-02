#include "lemon/wrapped_server.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace lemon {

class StubWrappedServer : public WrappedServer {
public:
    StubWrappedServer() : WrappedServer("stub", "error", nullptr, nullptr) {
        set_state(ModelState::READY);
    }

    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override {}

    void unload() override {}
};

}  // namespace lemon

using lemon::ModelState;
using lemon::StubWrappedServer;

static int failures = 0;

static void check(const char* name, bool condition) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        ++failures;
    }
}

static void test_completion_refreshes_idle_clock() {
    StubWrappedServer server;
    server.update_access_time();
    const auto request_started = server.get_last_access_time();

    check("request can acquire a ready server", server.acquire_for_inference());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    const auto release_started = std::chrono::steady_clock::now();
    server.release_inference();
    const auto completed = server.get_last_access_time();

    check("last access is refreshed when the request completes",
          completed >= release_started && completed > request_started);
    check("completed request leaves the server ready",
          server.get_state() == ModelState::READY);
}

static void test_overlapping_requests_refresh_on_last_release() {
    StubWrappedServer server;
    server.update_access_time();
    check("first overlapping request acquires", server.acquire_for_inference());
    check("second overlapping request acquires", server.acquire_for_inference());
    const auto before_first_release = server.get_last_access_time();

    server.release_inference();
    const auto after_first_release = server.get_last_access_time();
    check("idle clock does not start while another request is active",
          after_first_release == before_first_release &&
              server.get_state() == ModelState::IN_USE);

    const auto last_release_started = std::chrono::steady_clock::now();
    server.release_inference();
    const auto after_last_release = server.get_last_access_time();
    check("idle clock starts after the last overlapping request completes",
          after_last_release >= last_release_started &&
              after_last_release > after_first_release &&
              server.get_state() == ModelState::READY);
}

int main() {
    test_completion_refreshes_idle_clock();
    test_overlapping_requests_refresh_on_last_release();
    return failures == 0 ? 0 : 1;
}
