// Tests for ModelManager collection registration validation relevant to
// collection.router policy loading (#2383).

#include "lemon/model_manager.h"
#include "lemon/utils/path_utils.h"

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

static fs::path make_temp_dir() {
    fs::path dir = fs::temp_directory_path();
    dir /= "model_manager_collection_validation_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

static json component_def(const std::string& name) {
    return json{
        {"model_name", name},
        {"recipe", "llamacpp"},
        {"checkpoint", "example/" + name + ":Q4_K_M"},
    };
}

static json valid_router_collection() {
    return json{
        {"model_name", "user.RouterKit"},
        {"version", "1"},
        {"recipe", "collection.router"},
        {"components", {"local", "remote", "pii-detector"}},
        {"models", {
            component_def("local"),
            component_def("remote"),
            component_def("pii-detector"),
        }},
        {"routing", {
            {"candidates", {"local", "remote"}},
            {"default_model", "local"},
            {"classifiers", {{
                {"id", "pii"},
                {"type", "classifier"},
                {"model", "pii-detector"},
                {"labels", {"PII", "NO_PII"}},
                {"default_label", "PII"},
                {"on_error", "match_true"},
            }}},
            {"rules", {{
                {"id", "private-local"},
                {"match", {{"classifier", "pii"}, {"min_score", 0.5}}},
                {"route_to", "local"},
                {"outputs", {{"verdict", "warn"}}},
            }, {
                {"id", "code-remote"},
                {"match", {{"keywords_any", {"def ", "stack trace"}}}},
                {"route_to", "remote"},
            }}},
        }},
    };
}

static bool error_contains(const std::optional<std::string>& error,
                           const std::string& needle) {
    return error.has_value() && error->find(needle) != std::string::npos;
}

static void test_accepts_valid_router_policy(ModelManager& manager) {
    json doc = valid_router_collection();
    auto err = manager.validate_collection_request("user.RouterKit", doc);
    check("valid collection.router request passes validation", !err.has_value());
}

static void test_rejects_bad_routing(ModelManager& manager) {
    json doc = valid_router_collection();
    doc["routing"]["rules"][0]["route_to"] = "missing";
    auto err = manager.validate_collection_request("user.RouterKit", doc);
    check("invalid route_to in routing is rejected",
          error_contains(err, "Invalid collection.router routing policy") &&
          error_contains(err, "not declared in collection.components"));

    json bad_band = valid_router_collection();
    bad_band["routing"]["rules"][0]["match"]["min_score"] = 0.9;
    bad_band["routing"]["rules"][0]["match"]["max_score"] = 0.1;
    err = manager.validate_collection_request("user.RouterKit", bad_band);
    check("invalid score band is rejected",
          error_contains(err, "min_score greater than max_score"));

    json bad_regex = valid_router_collection();
    bad_regex["routing"]["rules"][1]["match"] = {{"regex", "(a+)+"}};
    err = manager.validate_collection_request("user.RouterKit", bad_regex);
    check("compile-time leaf validation rejects catastrophic regex",
          error_contains(err, "catastrophic backtracking"));
}

static void test_register_preserves_routing(ModelManager& manager) {
    json doc = valid_router_collection();
    manager.register_user_model("user.RouterKit", doc);
    auto info = manager.get_model_info("user.RouterKit");
    auto it = info.extras.find("routing");
    check("registered router collection preserves routing in ModelInfo extras",
          it != info.extras.end() && it->second == doc["routing"]);
}

int main() {
    fs::path temp = make_temp_dir();
    lemon::utils::set_cache_dir(temp.string());

    ModelManager manager;
    test_accepts_valid_router_policy(manager);
    test_rejects_bad_routing(manager);
    test_register_preserves_routing(manager);

    fs::remove_all(temp);

    if (g_failures == 0) {
        std::printf("All model manager collection validation tests passed.\n");
    } else {
        std::printf("%d model manager collection validation test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
