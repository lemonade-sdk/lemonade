#include "lemon/alias_manager.h"
#include "lemon/utils/path_utils.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using lemon::AliasManager;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

#ifdef _WIN32
static void set_test_env(const char* key, const char* val) {
    _putenv_s(key, val);
}
#else
static void set_test_env(const char* key, const char* val) {
    setenv(key, val, 1);
}
#endif

static fs::path make_temp_dir() {
    fs::path dir = fs::temp_directory_path();
    dir /= "lemonade_test_alias_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

static void test_alias_manager_basic() {
    fs::path temp_dir = make_temp_dir();
    std::string cache_dir = temp_dir.string();

    AliasManager mgr(cache_dir);
    std::string err;

    check("set_alias valid", mgr.set_alias("coding", "Qwen3-8B-GGUF", err));
    check("has_alias coding", mgr.has_alias("coding"));
    check("resolve_alias coding", mgr.resolve_alias("coding") == "Qwen3-8B-GGUF");
    check("resolve_alias unmapped returns nullopt", !mgr.resolve_alias("unknown").has_value());

    auto all = mgr.get_all_aliases();
    check("get_all_aliases count == 1", all.size() == 1);
    check("get_all_aliases coding key", all["coding"] == "Qwen3-8B-GGUF");

    AliasManager mgr2(cache_dir);
    check("reloaded has_alias coding", mgr2.has_alias("coding"));
    check("reloaded resolve_alias coding", mgr2.resolve_alias("coding") == "Qwen3-8B-GGUF");

    check("remove_alias returns true", mgr2.remove_alias("coding"));
    check("remove_alias removes key", !mgr2.has_alias("coding"));

    AliasManager mgr3(cache_dir);
    check("reloaded after remove is empty", mgr3.get_all_aliases().empty());

    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}

static void test_alias_manager_validation_and_cycles() {
    fs::path temp_dir = make_temp_dir();
    std::string cache_dir = temp_dir.string();

    AliasManager mgr(cache_dir);
    std::string err;

    check("self-referential rejected", !mgr.set_alias("self", "self", err));
    check("empty alias rejected", !mgr.set_alias("", "target", err));
    check("empty target rejected", !mgr.set_alias("alias", "", err));

    check("set A -> B", mgr.set_alias("aliasA", "aliasB", err));
    check("set B -> A cycle rejected", !mgr.set_alias("aliasB", "aliasA", err));

    check("set B -> C", mgr.set_alias("aliasB", "targetC", err));
    check("chained resolve A -> C", mgr.resolve_alias("aliasA") == "targetC");

    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}

static void test_alias_corrupted_json_recovery() {
    fs::path temp_dir = make_temp_dir();
    std::string cache_dir = temp_dir.string();

    fs::path alias_path = temp_dir / "aliases.json";
    {
        std::ofstream file(alias_path);
        file << "{ corrupted json object without closing brace: ";
    }

    AliasManager mgr(cache_dir);
    check("corrupted json recovers with empty map", mgr.get_all_aliases().empty());
    check("corrupted file renamed", fs::exists(temp_dir / "aliases.json.corrupted"));

    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}

static void test_alias_manager_standalone_resolution() {
    fs::path temp_dir = make_temp_dir();
    std::string cache_dir = temp_dir.string();

    AliasManager alias_mgr(cache_dir);
    std::string err;
    alias_mgr.set_alias("coding-test", "Tiny-Test-Model-GGUF", err);

    auto res_alias = alias_mgr.resolve_alias("coding-test");
    check("resolve_alias standalone resolves target", res_alias.has_value() && res_alias.value() == "Tiny-Test-Model-GGUF");

    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}

static void test_alias_disk_cycle_pruning() {
    fs::path temp_dir = make_temp_dir();
    std::string cache_dir = temp_dir.string();

    fs::path alias_path = temp_dir / "aliases.json";
    {
        std::ofstream file(alias_path);
        file << "{\n  \"cycleA\": \"cycleB\",\n  \"cycleB\": \"cycleA\"\n}\n";
    }

    AliasManager mgr(cache_dir);
    check("disk loaded cycle pruned from map", mgr.get_all_aliases().empty());
    check("disk loaded cyclic alias resolves to nullopt", !mgr.resolve_alias("cycleA").has_value());

    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}

int main() {
    std::printf("Running Standalone AliasManager Unit Tests...\n");

    test_alias_manager_basic();
    test_alias_manager_validation_and_cycles();
    test_alias_corrupted_json_recovery();
    test_alias_manager_standalone_resolution();
    test_alias_disk_cycle_pruning();

    if (g_failures > 0) {
        std::printf("\n!!! %d TEST(S) FAILED !!!\n", g_failures);
        return 1;
    }

    std::printf("\nALL TESTS PASSED!\n");
    return 0;
}
