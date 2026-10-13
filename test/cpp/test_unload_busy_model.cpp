// Unloading a model that is serving a request must not stall the router.
//
// The router lock guards every request dispatch and /health. If unload waits for
// the busy model's in-flight request while holding that lock, every other model
// stops answering for as long as that request runs (minutes for 3D generation).
#include "lemon/router.h"
#include "lemon/runtime_config.h"
#include "lemon/wrapped_server.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using nlohmann::json;

namespace lemon {

class UnloadStubServer : public WrappedServer {
public:
    explicit UnloadStubServer(const std::string& model_name)
        : WrappedServer("stub", "error", nullptr, nullptr) {
        set_model_metadata(model_name, "", ModelType::LLM, DEVICE_CPU, RecipeOptions());
        set_residency_class(ResidencyClass::Standard);
        set_state(ModelState::READY);
    }
    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override {}
    void unload() override { unloaded_.store(true); }
    bool is_backend_alive() const override { return true; }
    bool was_unloaded() const { return unloaded_.load(); }

private:
    std::atomic<bool> unloaded_{false};
};

struct UnloadBusyModelTestHook {
    static UnloadStubServer* add_server(Router& r, const std::string& name) {
        auto s = std::make_unique<UnloadStubServer>(name);
        UnloadStubServer* raw = s.get();
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        r.loaded_servers_.push_back(std::move(s));
        return raw;
    }

    static bool has_model(Router& r, const std::string& name) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        for (const auto& s : r.loaded_servers_) {
            if (s->get_model_name() == name) {
                return true;
            }
        }
        return false;
    }

    // Names are already canonical; the test Router has no ModelManager.
    static void unload(Router& r, const std::string& name) {
        r.unload_model_canonical(name, name);
    }
};

}  // namespace lemon

using lemon::Router;
using lemon::RuntimeConfig;
using lemon::UnloadBusyModelTestHook;
using lemon::UnloadStubServer;

static int failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

static void test_idle_unload_is_immediate(Router& router) {
    UnloadBusyModelTestHook::add_server(router, "idle-model");
    auto unload_done = std::async(std::launch::async, [&] {
        UnloadBusyModelTestHook::unload(router, "idle-model");
    });
    check("idle model unloads without waiting",
          unload_done.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    unload_done.get();
    check("idle model is removed", !UnloadBusyModelTestHook::has_model(router, "idle-model"));
}

static void test_unknown_model_throws(Router& router) {
    bool threw = false;
    try {
        UnloadBusyModelTestHook::unload(router, "never-loaded");
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("not loaded") != std::string::npos;
    }
    check("unknown model reports 'not loaded'", threw);
}

static void test_busy_unload_keeps_router_responsive(Router& router) {
    UnloadStubServer* busy = UnloadBusyModelTestHook::add_server(router, "busy-model");
    UnloadBusyModelTestHook::add_server(router, "other-model");
    busy->acquire_for_inference();

    auto unload_done = std::async(std::launch::async, [&] {
        UnloadBusyModelTestHook::unload(router, "busy-model");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    check("unload waits while the request is in flight",
          unload_done.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

    auto other_lookup = std::async(std::launch::async, [&] {
        return UnloadBusyModelTestHook::has_model(router, "other-model");
    });
    const bool responsive =
        other_lookup.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    check("router lock is free while unload waits", responsive);

    check("busy model is not unloaded mid-request", !busy->was_unloaded());

    busy->release_inference();
    const bool finished =
        unload_done.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    check("unload completes once the request is released", finished);
    check("busy model is removed after release",
          !UnloadBusyModelTestHook::has_model(router, "busy-model"));
    check("other model stays loaded", UnloadBusyModelTestHook::has_model(router, "other-model"));

    if (responsive) {
        other_lookup.get();
    }
    if (finished) {
        unload_done.get();
    }
}

int main() {
    json cfg = json::object();
    cfg["max_loaded_models"] = 4;
    cfg["log_level"] = "error";

    RuntimeConfig config(cfg);
    RuntimeConfig::set_global(&config);

    {
        Router router(&config, nullptr, nullptr);
        test_idle_unload_is_immediate(router);
        test_unknown_model_throws(router);
        test_busy_unload_keeps_router_responsive(router);
    }

    RuntimeConfig::set_global(nullptr);

    if (failures == 0) {
        std::printf("\nAll unload-busy-model tests passed.\n");
    } else {
        std::printf("\n%d unload-busy-model test(s) failed.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
