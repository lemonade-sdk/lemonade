// Cost and latency hints must survive user-model registration. They reach the
// router through ModelInfo::extras rather than a typed field, so a key missing
// from USER_DEFINED_MODEL_PROPS is dropped silently at registration and
// cost_select then ranks the model as having no price at all.

#include "lemon/model_manager.h"
#include "lemon/routing_classifier_services.h"
#include "lemon/utils/path_utils.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using lemon::ModelManager;
using lemon::json;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static bool near(double a, double b) {
    return a > b - 1e-9 && a < b + 1e-9;
}

static fs::path make_temp_dir() {
    fs::path dir = fs::temp_directory_path();
    dir /= "model_manager_cost_props_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

static json priced_model(const std::string& name) {
    return json{
        {"model_name", name},
        {"recipe", "llamacpp"},
        {"checkpoint", "example/" + name + ":Q4_K_M"},
        {"cost_tier", "low"},
        {"cost_input_per_million", 0.25},
        {"cost_output_per_million", 1.5},
        {"latency_ms_hint", 120.0},
    };
}

static void test_registration_preserves_cost_props(ModelManager& manager) {
    json doc = priced_model("user.PricedModel");
    manager.register_user_model("user.PricedModel", doc, "test");

    lemon::ModelInfo info = manager.get_model_info_unfiltered("user.PricedModel");
    const auto& e = info.extras;

    check("cost_tier survives registration",
          e.count("cost_tier") && e.at("cost_tier") == "low");
    check("cost_input_per_million survives registration",
          e.count("cost_input_per_million") &&
          near(e.at("cost_input_per_million").get<double>(), 0.25));
    check("cost_output_per_million survives registration",
          e.count("cost_output_per_million") &&
          near(e.at("cost_output_per_million").get<double>(), 1.5));
    check("latency_ms_hint survives registration",
          e.count("latency_ms_hint") &&
          near(e.at("latency_ms_hint").get<double>(), 120.0));
}

// The whole point of persisting the keys: the router has to be able to read a
// price back out of a model the user registered through the API.
static void test_router_resolves_registered_cost(ModelManager& manager) {
    lemon::ModelInfo info = manager.get_model_info_unfiltered("user.PricedModel");
    lemon::CostInfo cost = lemon::resolve_cost_info(
        info.cost_input_per_million >= 0.0
            ? std::optional<double>{info.cost_input_per_million}
            : std::nullopt,
        info.cost_output_per_million >= 0.0
            ? std::optional<double>{info.cost_output_per_million}
            : std::nullopt,
        info.extras);

    check("router resolves a registered model's cost tier",
          cost.cost_tier.has_value() && *cost.cost_tier == "low");
    check("router resolves a registered model's input price",
          cost.cost_input_per_million.has_value() &&
          near(*cost.cost_input_per_million, 0.25));
    check("router resolves a registered model's output price",
          cost.cost_output_per_million.has_value() &&
          near(*cost.cost_output_per_million, 1.5));
    check("router resolves a registered model's latency hint",
          cost.latency_ms_hint.has_value() && near(*cost.latency_ms_hint, 120.0));
}

// Values the read side already refuses are still persisted verbatim; they are
// filtered when resolved, not at registration. Pinning that so a future change
// to either side is a deliberate one.
static void test_unusable_values_are_dropped_on_read(ModelManager& manager) {
    json doc = priced_model("user.BadlyPricedModel");
    doc["cost_tier"] = "cheapish";
    doc["cost_input_per_million"] = -3.0;
    manager.register_user_model("user.BadlyPricedModel", doc, "test");

    lemon::ModelInfo info = manager.get_model_info_unfiltered("user.BadlyPricedModel");
    lemon::CostInfo cost =
        lemon::resolve_cost_info(std::nullopt, std::nullopt, info.extras);

    check("an unrecognized cost_tier resolves to no tier",
          !cost.cost_tier.has_value());
    check("a negative input price resolves to no price",
          !cost.cost_input_per_million.has_value());
    check("a valid sibling price still resolves",
          cost.cost_output_per_million.has_value() &&
          near(*cost.cost_output_per_million, 1.5));
}

int main() {
    fs::path temp = make_temp_dir();
    lemon::utils::set_cache_dir(temp.string());

    ModelManager manager;
    test_registration_preserves_cost_props(manager);
    test_router_resolves_registered_cost(manager);
    test_unusable_values_are_dropped_on_read(manager);

    fs::remove_all(temp);

    if (g_failures == 0) {
        std::printf("All model manager cost prop tests passed.\n");
    } else {
        std::printf("%d model manager cost prop test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
