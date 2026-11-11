// Cost and latency hints must survive user-model registration and `lemonade
// import`. They reach the router through ModelInfo::extras rather than a typed
// field, so a key missing from either allowlist is dropped silently and the
// model loses its price.

#include "lemon/cost_metadata.h"
#include "lemon/model_manager.h"
#include "lemon/routing_classifier_services.h"
#include "lemon/utils/path_utils.h"
#include "lemon_cli/recipe_import.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;
using lemon::ModelManager;
using lemon::json;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static bool approx_equal(double a, double b) {
    return a > b - 1e-9 && a < b + 1e-9;
}

static bool approx_equal(const std::optional<double>& a, double b) {
    return a.has_value() && approx_equal(*a, b);
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
    manager.register_user_model("user.PricedModel", priced_model("user.PricedModel"), "test");

    const auto& e = manager.get_model_info("user.PricedModel").extras;
    check("cost_tier survives registration",
          e.count("cost_tier") && e.at("cost_tier") == "low");
    check("cost_input_per_million survives registration",
          e.count("cost_input_per_million") &&
          approx_equal(e.at("cost_input_per_million").get<double>(), 0.25));
    check("cost_output_per_million survives registration",
          e.count("cost_output_per_million") &&
          approx_equal(e.at("cost_output_per_million").get<double>(), 1.5));
    check("latency_ms_hint survives registration",
          e.count("latency_ms_hint") &&
          approx_equal(e.at("latency_ms_hint").get<double>(), 120.0));
}

// The router has to be able to read a price back out of a model the user
// registered through the API, through the same helper it calls.
static void test_router_resolves_registered_cost(ModelManager& manager) {
    const lemon::CostInfo cost =
        lemon::resolve_model_cost_info(manager.get_model_info("user.PricedModel"));

    check("router resolves a registered model's cost tier",
          cost.cost_tier.has_value() && *cost.cost_tier == "low");
    check("router resolves a registered model's input price",
          approx_equal(cost.cost_input_per_million, 0.25));
    check("router resolves a registered model's output price",
          approx_equal(cost.cost_output_per_million, 1.5));
    check("router resolves a registered model's latency hint",
          approx_equal(cost.latency_ms_hint, 120.0));
}

static void test_typed_price_wins_over_extras() {
    lemon::ModelInfo info;
    info.cost_input_per_million = 2.0;
    info.extras["cost_input_per_million"] = 9.0;
    info.extras["cost_output_per_million"] = 4.0;

    const lemon::CostInfo cost = lemon::resolve_model_cost_info(info);
    check("a typed price wins over the same key in extras",
          approx_equal(cost.cost_input_per_million, 2.0));
    check("extras fill a typed price that is unknown",
          approx_equal(cost.cost_output_per_million, 4.0));
}

// Values the read side refuses are still persisted verbatim; they are filtered
// when resolved and reported at registration, not rejected.
static void test_unusable_values_are_dropped_on_read(ModelManager& manager) {
    json doc = priced_model("user.BadlyPricedModel");
    doc["cost_tier"] = "cheapish";
    doc["cost_input_per_million"] = -3.0;
    manager.register_user_model("user.BadlyPricedModel", doc, "test");

    const lemon::CostInfo cost =
        lemon::resolve_model_cost_info(manager.get_model_info("user.BadlyPricedModel"));
    check("an unrecognized cost_tier resolves to no tier", !cost.cost_tier.has_value());
    check("a negative input price resolves to no price",
          !cost.cost_input_per_million.has_value());
    check("a valid sibling price still resolves",
          approx_equal(cost.cost_output_per_million, 1.5));
}

static void test_unusable_cost_metadata_names_bad_keys() {
    const json entry = {
        {"cost_tier", "LOW"},
        {"cost_input_per_million", "0.25"},
        {"cost_output_per_million", true},
        {"latency_ms_hint", nullptr},
    };
    const auto bad = lemon::unusable_cost_metadata(entry);
    const auto has = [&bad](const char* key) {
        return std::find(bad.begin(), bad.end(), key) != bad.end();
    };
    check("a wrong-case cost_tier is unusable", has("cost_tier"));
    check("a price given as a string is unusable", has("cost_input_per_million"));
    check("a price given as a boolean is unusable", has("cost_output_per_million"));
    check("a null latency counts as unset", !has("latency_ms_hint"));
    check("usable values are not reported",
          lemon::unusable_cost_metadata(priced_model("m")).empty());
}

static void test_import_keeps_cost_props() {
    json doc = priced_model("ImportedPricedModel");
    doc["not_a_model_key"] = 1;
    const bool ok = lemon_cli::validate_and_transform_model_json(doc);

    check("an imported model file is accepted", ok);
    for (const char* key : lemon::kCostMetadataKeys) {
        check((std::string("import keeps ") + key).c_str(), doc.contains(key));
    }
    check("import still drops unknown keys", !doc.contains("not_a_model_key"));
}

int main() {
    fs::path temp = make_temp_dir();
    lemon::utils::set_cache_dir((temp / "cache").string());
    lemon::utils::set_config_dir((temp / "config").string());

    {
        ModelManager manager;
        test_registration_preserves_cost_props(manager);
        test_router_resolves_registered_cost(manager);
        test_unusable_values_are_dropped_on_read(manager);
    }
    test_typed_price_wins_over_extras();
    test_unusable_cost_metadata_names_bad_keys();
    test_import_keeps_cost_props();

    fs::remove_all(temp);

    if (g_failures == 0) {
        std::printf("All model manager cost prop tests passed.\n");
    } else {
        std::printf("%d model manager cost prop test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
