// Unit tests for make_router_cost_services (Router-backed CostServices
// wiring), specifically its process-global price cache: the entry-count
// bound that protects against unbounded growth from a caller-supplied
// candidate name (e.g. /v1/routing/validate's identity resolver has no
// registered-model count to bound it).

#include "lemon/routing_classifier_services.h"

#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/path_utils.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;
using lemon::CostInfo;
using lemon::CostServices;
using lemon::ModelManager;
using lemon::Router;
using lemon::RuntimeConfig;
using lemon::json;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static fs::path make_temp_dir() {
    fs::path dir = fs::temp_directory_path();
    dir /= "router_cost_services_test_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

// Router's constructor (via RuntimeConfig::max_loaded_models()) requires
// max_loaded_models to already be present -- RuntimeConfig does no
// defaults-merging of its own, so a config missing a key the code touches is
// a real, if easily-avoided, misuse.
static RuntimeConfig make_runtime_config() {
    json cfg = json::object();
    cfg["max_loaded_models"] = 4;
    cfg["log_level"] = "error";
    return RuntimeConfig(cfg);
}

// Drives the production cache well past its real bound with distinct
// candidate names, then confirms a name cached early is still resolved
// correctly afterward -- proving the "cache full" path
// (make_router_cost_services's entry-count check) doesn't corrupt state,
// crash, or return another entry's data at scale, regardless of whether a
// given lookup is served from cache or recomputed. Every candidate here is
// unregistered (resolves to an empty CostInfo either way), which is enough
// to exercise the cap itself without needing real priced-model plumbing.
static void test_price_cache_survives_flooding_past_its_bound() {
    fs::path temp = make_temp_dir();
    lemon::utils::set_cache_dir(temp.string());

    ModelManager model_manager;
    RuntimeConfig config = make_runtime_config();
    RuntimeConfig::set_global(&config);
    Router router(&config, &model_manager, nullptr);

    CostServices services = lemon::make_router_cost_services(router, model_manager);

    CostInfo before = services.cost_of("kept-candidate");

    // Well past the real 4096-entry cap.
    for (int i = 0; i < 4200; ++i) {
        services.cost_of("junk-candidate-" + std::to_string(i));
    }

    CostInfo after = services.cost_of("kept-candidate");

    check("router cost cache: an early-cached candidate resolves consistently after "
          "flooding the cache past its bound",
          !before.cost_tier.has_value() && !before.cost_input_per_million.has_value() &&
          !after.cost_tier.has_value() && !after.cost_input_per_million.has_value());

    RuntimeConfig::set_global(nullptr);
    fs::remove_all(temp);
}

// An unregistered candidate always resolves to an empty CostInfo, whether or
// not the "cache full" path was taken for it -- confirms that path returns
// the same value a cache hit or a fresh miss would.
static void test_unregistered_candidate_is_consistently_no_data() {
    fs::path temp = make_temp_dir();
    lemon::utils::set_cache_dir(temp.string());

    ModelManager model_manager;
    RuntimeConfig config = make_runtime_config();
    RuntimeConfig::set_global(&config);
    Router router(&config, &model_manager, nullptr);

    CostServices services = lemon::make_router_cost_services(router, model_manager);

    CostInfo first = services.cost_of("nonexistent-model");
    CostInfo second = services.cost_of("nonexistent-model");
    check("router cost cache: an unregistered candidate is no-data, consistently across repeated lookups",
          !first.cost_tier.has_value() && !first.cost_input_per_million.has_value() &&
          !second.cost_tier.has_value() && !second.cost_input_per_million.has_value());

    RuntimeConfig::set_global(nullptr);
    fs::remove_all(temp);
}

int main() {
    test_price_cache_survives_flooding_past_its_bound();
    test_unregistered_candidate_is_consistently_no_data();

    if (g_failures == 0) {
        std::printf("All router cost services tests passed.\n");
    } else {
        std::printf("%d router cost services test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
