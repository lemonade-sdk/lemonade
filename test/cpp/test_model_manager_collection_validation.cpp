// Tests for ModelManager collection registration validation relevant to
// collection.router policy loading (#2383), and for the aggregate
// max_context_window computed for collection models at cache-build time.

#include "lemon/config_file.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/path_utils.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
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

static void write_fake_gguf(const fs::path& path, uint32_t context_length) {
    std::ofstream out(path, std::ios::binary);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    auto w32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
    auto w64 = [&](uint64_t v) { out.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
    auto wstr = [&](const std::string& s) {
        w64(s.size());
        out.write(s.data(), static_cast<std::streamsize>(s.size()));
    };
    out.write("GGUF", 4);
    w32(3);  // version
    w64(0);  // tensor count
    w64(2);  // kv count
    wstr("general.architecture");
    w32(8);  // string
    wstr("llama");
    wstr("llama.context_length");
    w32(4);  // uint32
    w32(context_length);
}

static void register_local_gguf(ModelManager& manager, const std::string& name,
                                const fs::path& temp, uint32_t context_length) {
    fs::path gguf = temp / (name + ".gguf");
    write_fake_gguf(gguf, context_length);
    json def = {
        {"recipe", "llamacpp"},
        {"checkpoint", gguf.string()},
        {"source", "local_path"},
    };
    manager.register_user_model("user." + name, def);
}

static void test_collection_aggregate_context_window(ModelManager& manager,
                                                     const fs::path& temp) {
    register_local_gguf(manager, "ctx-large", temp, 32768);
    register_local_gguf(manager, "ctx-small", temp, 8192);
    register_local_gguf(manager, "ctx-classifier", temp, 512);

    json router = {
        {"version", "1"},
        {"recipe", "collection.router"},
        {"components", {"ctx-large", "ctx-small", "ctx-classifier"}},
        {"routing", {
            {"candidates", {"ctx-large", "ctx-small"}},
            {"default_model", "ctx-large"},
            {"classifiers", {{
                {"id", "pii"},
                {"type", "classifier"},
                {"model", "ctx-classifier"},
                {"labels", {"PII", "NO_PII"}},
                {"default_label", "PII"},
                {"on_error", "match_true"},
            }}},
            {"rules", {{
                {"id", "private-small"},
                {"match", {{"classifier", "pii"}, {"min_score", 0.5}}},
                {"route_to", "ctx-small"},
            }}},
        }},
    };
    manager.register_user_model("user.CtxRouter", router);

    auto large = manager.get_model_info("user.ctx-large");
    check("local gguf component resolves its own context window",
          large.max_context_window == 32768);

    auto router_info = manager.get_model_info("user.CtxRouter");
    check("collection.router is marked downloaded when bare component names resolve",
          router_info.downloaded);
    check("collection.router aggregates min candidate context window",
          router_info.max_context_window == 8192);

    json omni = {
        {"recipe", "collection.omni"},
        {"components", {"ctx-large", "ctx-small", "ctx-classifier", "ctx-absent"}},
    };
    manager.register_user_model("user.CtxOmni", omni);
    auto omni_info = manager.get_model_info("user.CtxOmni");
    check("collection.omni aggregates min component context window, skipping unknowns",
          omni_info.max_context_window == 512);

    manager.update_model_in_cache("user.ctx-small", false);
    router_info = manager.get_model_info("user.CtxRouter");
    check("collection.router download state refreshes when a canonical component changes",
          !router_info.downloaded);
    check("collection.router context window refreshes when a canonical component changes",
          router_info.max_context_window == 32768);
}

int main() {
    fs::path temp = make_temp_dir();
    lemon::utils::set_cache_dir(temp.string());

    // Disable hardware-based model filtering so the llamacpp component models
    // used by the aggregate-context-window test survive cache build on any
    // machine this test runs on.
    json config = lemon::ConfigFile::load(temp.string());
    config["disable_model_filtering"] = true;
    auto runtime_config = std::make_unique<lemon::RuntimeConfig>(config);
    lemon::RuntimeConfig::set_global(runtime_config.get());

    ModelManager manager;
    test_accepts_valid_router_policy(manager);
    test_rejects_bad_routing(manager);
    test_register_preserves_routing(manager);
    test_collection_aggregate_context_window(manager, temp);

    lemon::RuntimeConfig::set_global(nullptr);
    fs::remove_all(temp);

    if (g_failures == 0) {
        std::printf("All model manager collection validation tests passed.\n");
    } else {
        std::printf("%d model manager collection validation test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
