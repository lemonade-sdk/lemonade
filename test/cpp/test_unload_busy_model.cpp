// Unloading a model that is serving a request must not stall the router.
//
// The router lock guards every request dispatch and /health. If unload waits for
// the busy model's in-flight request while holding that lock, every other model
// stops answering for as long as that request runs (minutes for 3D generation).
#include "lemon/router.h"
#include "lemon/runtime_config.h"
#include "lemon/wrapped_server.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
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

    // Stands in for another path (unload-all, LRU eviction) dropping the model.
    static void remove_server(Router& r, const std::string& name) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        r.loaded_servers_.erase(
            std::remove_if(r.loaded_servers_.begin(), r.loaded_servers_.end(),
                           [&](const std::unique_ptr<WrappedServer>& s) {
                               return s->get_model_name() == name;
                           }),
            r.loaded_servers_.end());
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
static bool hung = false;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

// Detached instead of std::async: a std::future from std::async joins in its
// destructor, so an unload that never returns would hang the test instead of
// failing it.
static std::shared_future<void> run_detached(std::function<void()> fn) {
    auto done = std::make_shared<std::promise<void>>();
    std::shared_future<void> result = done->get_future().share();
    std::thread([done, fn = std::move(fn)] {
        try {
            fn();
            done->set_value();
        } catch (...) {
            done->set_exception(std::current_exception());
        }
    }).detach();
    return result;
}

static bool finishes(const std::shared_future<void>& f, std::chrono::milliseconds timeout) {
    const bool ready = f.wait_for(timeout) == std::future_status::ready;
    if (!ready) hung = true;
    return ready;
}

static bool threw(const std::shared_future<void>& f) {
    try {
        f.get();
    } catch (...) {
        return true;
    }
    return false;
}

static void test_idle_unload_is_immediate(Router& router) {
    UnloadBusyModelTestHook::add_server(router, "idle-model");
    auto unload_done = run_detached([&] { UnloadBusyModelTestHook::unload(router, "idle-model"); });
    const bool finished = finishes(unload_done, std::chrono::seconds(2));
    check("idle model unloads without waiting", finished && !threw(unload_done));
    check("idle model is removed", !UnloadBusyModelTestHook::has_model(router, "idle-model"));
}

static void test_unknown_model_throws(Router& router) {
    bool not_loaded = false;
    try {
        UnloadBusyModelTestHook::unload(router, "never-loaded");
    } catch (const std::runtime_error& e) {
        not_loaded = std::string(e.what()).find("not loaded") != std::string::npos;
    }
    check("unknown model reports 'not loaded'", not_loaded);
}

static void test_busy_unload_keeps_router_responsive(Router& router) {
    UnloadStubServer* busy = UnloadBusyModelTestHook::add_server(router, "busy-model");
    UnloadBusyModelTestHook::add_server(router, "other-model");
    busy->acquire_for_inference();

    auto unload_done = run_detached([&] { UnloadBusyModelTestHook::unload(router, "busy-model"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check("unload waits while the request is in flight",
          unload_done.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

    auto other_lookup = run_detached([&] { UnloadBusyModelTestHook::has_model(router, "other-model"); });
    check("router lock is free while unload waits", finishes(other_lookup, std::chrono::seconds(2)));
    check("busy model is not unloaded mid-request", !busy->was_unloaded());

    busy->release_inference();
    const bool finished = finishes(unload_done, std::chrono::seconds(5));
    check("unload completes once the request is released", finished && !threw(unload_done));
    check("busy model is removed after release",
          !UnloadBusyModelTestHook::has_model(router, "busy-model"));
    check("other model stays loaded", UnloadBusyModelTestHook::has_model(router, "other-model"));
}

static void test_model_removed_while_waiting(Router& router) {
    UnloadStubServer* busy = UnloadBusyModelTestHook::add_server(router, "vanishing-model");
    busy->acquire_for_inference();

    auto unload_done = run_detached([&] { UnloadBusyModelTestHook::unload(router, "vanishing-model"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto removed = run_detached([&] { UnloadBusyModelTestHook::remove_server(router, "vanishing-model"); });
    if (!finishes(removed, std::chrono::seconds(2))) {
        check("router lock is free while unload waits (vanishing model)", false);
        return;
    }

    const bool finished = finishes(unload_done, std::chrono::seconds(5));
    check("unload returns when the model disappears while it waits", finished);
    check("a model removed by another path is not reported as an error",
          finished && !threw(unload_done));
}

static void test_reloaded_model_is_left_alone(Router& router) {
    UnloadStubServer* busy = UnloadBusyModelTestHook::add_server(router, "reloaded-model");
    busy->acquire_for_inference();

    auto unload_done = run_detached([&] { UnloadBusyModelTestHook::unload(router, "reloaded-model"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    UnloadStubServer* fresh = nullptr;
    auto replaced = run_detached([&] {
        UnloadBusyModelTestHook::remove_server(router, "reloaded-model");
        fresh = UnloadBusyModelTestHook::add_server(router, "reloaded-model");
    });
    if (!finishes(replaced, std::chrono::seconds(2))) {
        check("router lock is free while unload waits (reloaded model)", false);
        return;
    }

    const bool finished = finishes(unload_done, std::chrono::seconds(5));
    check("unload returns when the waited-for instance was replaced",
          finished && !threw(unload_done));
    check("a newer instance under the same name stays loaded",
          UnloadBusyModelTestHook::has_model(router, "reloaded-model") && !fresh->was_unloaded());
}

static void test_unload_all_via_public_entry(Router& router) {
    UnloadBusyModelTestHook::add_server(router, "all-a");
    UnloadBusyModelTestHook::add_server(router, "all-b");
    auto unload_done = run_detached([&] { router.unload_model(""); });
    const bool finished = finishes(unload_done, std::chrono::seconds(5));
    check("unload_model(\"\") returns for idle models", finished && !threw(unload_done));
    check("unload_model(\"\") removes every model",
          !UnloadBusyModelTestHook::has_model(router, "all-a") &&
              !UnloadBusyModelTestHook::has_model(router, "all-b") &&
              !UnloadBusyModelTestHook::has_model(router, "other-model"));
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    json cfg = json::object();
    cfg["max_loaded_models"] = 4;
    cfg["log_level"] = "error";

    RuntimeConfig config(cfg);
    RuntimeConfig::set_global(&config);

    {
        Router router(&config, nullptr, nullptr);
        // After a hang the router lock may stay held, so later cases would block
        // in their own setup; stop there and report.
        void (*cases[])(Router&) = {
            test_idle_unload_is_immediate,    test_unknown_model_throws,
            test_busy_unload_keeps_router_responsive, test_model_removed_while_waiting,
            test_reloaded_model_is_left_alone, test_unload_all_via_public_entry,
        };
        for (auto* run_case : cases) {
            if (hung) break;
            run_case(router);
        }

        if (hung) {
            // A detached thread still references the router; destroying it would race.
            std::printf("\n%d unload-busy-model test(s) failed (an unload never returned).\n",
                        failures);
            std::fflush(stdout);
            std::_Exit(1);
        }
    }

    RuntimeConfig::set_global(nullptr);

    if (failures == 0) {
        std::printf("\nAll unload-busy-model tests passed.\n");
    } else {
        std::printf("\n%d unload-busy-model test(s) failed.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
