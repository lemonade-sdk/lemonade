// Unit tests for the directory-backed RoutingPolicyStore (#2383).

#include "fake_classifier_services.h"
#include "lemon/routing_policy_store.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#ifndef ROUTING_FIXTURE_DIR
#define ROUTING_FIXTURE_DIR "test/cpp/fixtures/routing"
#endif

namespace fs = std::filesystem;
using lemon::Decision;
using lemon::RouteContext;
using lemon::RoutingPolicyStore;
using lemon::json;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static json load_fixture(const std::string& name) {
    std::ifstream in(std::string(ROUTING_FIXTURE_DIR) + "/" + name);
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

static void write_json(const fs::path& path, const json& doc) {
    std::ofstream out(path);
    out << doc.dump(2) << "\n";
}

static fs::path make_temp_dir() {
    fs::path dir = fs::temp_directory_path();
    dir /= "routing_policy_store_test_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

static RouteContext request(const std::string& input) {
    RouteContext ctx;
    ctx.input = input;
    ctx.params.chars = input.size();
    return ctx;
}

static void test_reload_swaps_snapshot() {
    fs::path dir = make_temp_dir();
    fs::path policy_path = dir / "router.json";
    json doc = load_fixture("l1_keywords.json");
    write_json(policy_path, doc);

    lemon::testing::FakeClassifierServices fake;
    RoutingPolicyStore store(dir.string(), fake.make());
    auto first_snapshot = store.reload();
    auto first_engine = store.get_engine("user.Router-Keywords");
    check("initial reload loads one engine",
          first_snapshot->engines.size() == 1 && first_engine != nullptr);

    Decision first = first_engine->route(request("please fix this stack trace"), false);
    check("initial engine routes with original policy",
          first.route_to == "vllm.qwen3-32b");

    doc["routing"]["rules"][0]["route_to"] = "Qwen3-8B-GGUF";
    write_json(policy_path, doc);
    auto second_snapshot = store.reload();
    auto second_engine = store.get_engine("user.Router-Keywords");

    check("manual reload swaps engine shared_ptr",
          second_snapshot->engines.size() == 1 && second_engine != nullptr &&
          second_engine != first_engine);
    Decision second = second_engine->route(request("please fix this stack trace"), false);
    check("reloaded engine uses updated policy",
          second.route_to == "Qwen3-8B-GGUF");

    fs::remove_all(dir);
}

static void test_directory_watcher_reload() {
    fs::path dir = make_temp_dir();
    fs::path policy_path = dir / "router.json";
    json doc = load_fixture("l1_keywords.json");
    write_json(policy_path, doc);

    lemon::testing::FakeClassifierServices fake;
    RoutingPolicyStore store(dir.string(), fake.make());
    store.start_watching();
    auto first_engine = store.get_engine("user.Router-Keywords");
    check("watcher initial load has engine", first_engine != nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    doc["routing"]["rules"][0]["route_to"] = "Qwen3-8B-GGUF";
    // Delete first to trigger a directory entry change. macOS's kqueue-based
    // DirectoryWatcher monitors the directory fd and does not detect inline writes.
    fs::remove(policy_path);
    write_json(policy_path, doc);

    std::shared_ptr<const lemon::RoutingPolicyEngine> next_engine;
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        next_engine = store.get_engine("user.Router-Keywords");
        if (next_engine && next_engine != first_engine) {
            break;
        }
    }

    check("DirectoryWatcher triggers engine swap",
          next_engine != nullptr && next_engine != first_engine);
    if (next_engine) {
        Decision routed = next_engine->route(request("please fix this stack trace"), false);
        check("watcher-reloaded engine uses updated policy",
              routed.route_to == "Qwen3-8B-GGUF");
    }
    store.stop_watching();
    fs::remove_all(dir);
}

static void test_invalid_policy_reports_error() {
    fs::path dir = make_temp_dir();
    fs::path policy_path = dir / "bad.json";
    json doc = load_fixture("l1_keywords.json");
    doc["routing"]["rules"][0]["route_to"] = "missing-model";
    write_json(policy_path, doc);

    lemon::testing::FakeClassifierServices fake;
    RoutingPolicyStore store(dir.string(), fake.make());
    auto snapshot = store.reload();
    check("invalid policy does not load an engine", snapshot->engines.empty());
    check("invalid policy records clear error", !snapshot->errors.empty() &&
          snapshot->errors.begin()->second.find("not declared") != std::string::npos);

    fs::remove_all(dir);
}

static void test_uppercase_json_extension_loads_policy() {
    fs::path dir = make_temp_dir();
    fs::path policy_path = dir / "router.JSON";
    json doc = load_fixture("l1_keywords.json");
    write_json(policy_path, doc);

    lemon::testing::FakeClassifierServices fake;
    RoutingPolicyStore store(dir.string(), fake.make());
    auto snapshot = store.reload();

    check("uppercase .JSON policy file extension is accepted",
          snapshot->engines.count("user.Router-Keywords") == 1);

    fs::remove_all(dir);
}

static void test_duplicate_policy_model_names_report_errors() {
    fs::path dir = make_temp_dir();
    json doc = load_fixture("l1_keywords.json");
    write_json(dir / "first.json", doc);
    write_json(dir / "second.json", doc);

    lemon::testing::FakeClassifierServices fake;
    RoutingPolicyStore store(dir.string(), fake.make());
    auto snapshot = store.reload();

    check("duplicate policy model names do not load an arbitrary engine",
          snapshot->engines.empty());
    check("duplicate policy model names record per-file errors",
          snapshot->errors.size() == 2 &&
          snapshot->errors.begin()->second.find("duplicate collection.router model_name") !=
              std::string::npos);

    fs::remove_all(dir);
}

int main() {
    test_reload_swaps_snapshot();
    test_directory_watcher_reload();
    test_invalid_policy_reports_error();
    test_uppercase_json_extension_loads_policy();
    test_duplicate_policy_model_names_report_errors();

    if (g_failures == 0) {
        std::printf("All routing policy store tests passed.\n");
    } else {
        std::printf("%d routing policy store test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
