// Regression test for issue #3733: the auto-eviction idle clock must start
// when the last in-flight request completes, not when it started.
//
// Before the fix, WrappedServer::release_inference() transitioned the server
// back to READY without refreshing last_access_time_, so a request whose
// prefill/generation ran longer than evict_idle_timeout left the model
// instantly evictable the moment it finished (the eviction engine compares
// now - get_last_access_time() against the timeout and only skips IN_USE
// servers). The router updates the access time when a request starts; this
// test pins that completing the request restarts the idle clock, and that a
// partial release (other requests still in flight) does not.

#include "lemon/wrapped_server.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace lemon {

// Minimal WrappedServer that never spawns a subprocess. Only load()/unload()
// are pure virtual.
class IdleClockStubServer : public WrappedServer {
public:
    IdleClockStubServer() : WrappedServer("stub", "error", nullptr, nullptr) {
        set_model_metadata("idle-clock-stub", "", ModelType::LLM, DEVICE_CPU,
                           RecipeOptions());
        set_state(ModelState::READY);
    }

    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override {}
    void unload() override {}
};

} // namespace lemon

namespace {

int failures = 0;

void check(const char* name, bool condition) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        ++failures;
    }
}

} // namespace

int main() {
    using namespace std::chrono;
    lemon::IdleClockStubServer server;

    const auto before = server.get_last_access_time();

    // Simulate a long-running request: acquired, held well past the moment
    // the access time was last stamped, then released.
    std::this_thread::sleep_for(milliseconds(20));
    check("acquire succeeds", server.acquire_for_inference());
    std::this_thread::sleep_for(milliseconds(20));
    server.release_inference();

    check("release restarts the idle clock",
          server.get_last_access_time() > before);

    // With two requests in flight, releasing the first must not restart the
    // clock — the model is still busy. Releasing the last one must.
    const auto busy_since = server.get_last_access_time();
    std::this_thread::sleep_for(milliseconds(20));
    check("first of two acquires succeeds", server.acquire_for_inference());
    check("second acquire succeeds", server.acquire_for_inference());
    server.release_inference();
    check("partial release keeps the previous access time",
          server.get_last_access_time() == busy_since);
    std::this_thread::sleep_for(milliseconds(20));
    server.release_inference();
    check("final release restarts the idle clock",
          server.get_last_access_time() > busy_since);

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
