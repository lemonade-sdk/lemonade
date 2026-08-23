#include <cassert>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <lemon/config_file.h>
#include <lemon/server.h>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/path_utils.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
using lemon::ConfigFile;
using lemon::RuntimeConfig;
using lemon::Server;
using lemon::utils::JsonUtils;

static int passed = 0;
static int failures = 0;

static void check(bool cond, const char* desc) {
    if (cond) {
        std::fprintf(stderr, "[PASS] %s\n", desc);
        ++passed;
    } else {
        std::fprintf(stderr, "[FAIL] %s\n", desc);
        ++failures;
    }
    std::fflush(stderr);
}

class ScopedTempDir {
public:
    ScopedTempDir() {
        path_ = fs::temp_directory_path() / ("lemon_test_cfg_" + std::to_string(std::rand()) + "_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
        fs::create_directories(path_);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }
    std::string string() const { return lemon::utils::path_to_utf8(path_); }

private:
    fs::path path_;
};

// ============================================================================
// Deep merge unit tests (JsonUtils::merge)
// ============================================================================

static void test_deep_merge_level1() {
    std::puts("--- test_deep_merge_level1 ---");
    json base = {
        {"port", 13305},
        {"host", "localhost"},
        {"log_level", "info"},
        {"max_loaded_models", 1}
    };
    json overlay = {
        {"port", 9000}
    };

    json merged = JsonUtils::merge(base, overlay);
    check(merged["port"] == 9000, "overrides port");
    check(merged["host"] == "localhost", "preserves host");
    check(merged["log_level"] == "info", "preserves log_level");
    check(merged["max_loaded_models"] == 1, "preserves max_loaded_models");
}

static void test_deep_merge_level2_backend() {
    std::puts("\n--- test_deep_merge_level2_backend ---");
    json base = {
        {"llamacpp", {
            {"backend", "auto"},
            {"args", "--ctx-size 2048"},
            {"cpu_bin", "builtin"},
            {"vulkan_bin", "builtin"},
            {"prefer_system", true}
        }},
        {"whispercpp", {
            {"backend", "auto"},
            {"cpu_bin", "builtin"}
        }}
    };
    json overlay = {
        {"llamacpp", {
            {"backend", "vulkan"},
            {"vulkan_bin", "b8664"}
        }}
    };

    json merged = JsonUtils::merge(base, overlay);
    check(merged["llamacpp"]["backend"] == "vulkan", "overrides llamacpp.backend");
    check(merged["llamacpp"]["vulkan_bin"] == "b8664", "overrides llamacpp.vulkan_bin");
    check(merged["llamacpp"]["args"] == "--ctx-size 2048", "preserves sibling llamacpp.args");
    check(merged["llamacpp"]["cpu_bin"] == "builtin", "preserves sibling llamacpp.cpu_bin");
    check(merged["llamacpp"]["prefer_system"] == true, "preserves sibling llamacpp.prefer_system");
    check(merged["whispercpp"]["backend"] == "auto", "preserves sibling backend whispercpp");
}

static void test_deep_merge_level3_telemetry() {
    std::puts("\n--- test_deep_merge_level3_telemetry ---");
    json base = {
        {"telemetry", {
            {"enabled", false},
            {"hide_inputs", false},
            {"hide_outputs", false},
            {"max_queue_capacity", 1000},
            {"otlp", {
                {"endpoint", "http://localhost:4318/v1/traces"},
                {"protocol", "http/protobuf"},
                {"semantics", json::array({"openinference", "otel_genai"})},
                {"headers", json::object()},
                {"max_retries", 0},
                {"retry_backoff_base_s", 5.0},
                {"send_batch_size", 100},
                {"batch_timeout_s", 1.0}
            }}
        }}
    };
    json overlay = {
        {"telemetry", {
            {"enabled", true},
            {"otlp", {
                {"endpoint", "http://custom-collector:4318/v1/traces"},
                {"headers", {{"Authorization", "Bearer token123"}}}
            }}
        }}
    };

    json merged = JsonUtils::merge(base, overlay);
    check(merged["telemetry"]["enabled"] == true, "overrides telemetry.enabled");
    check(merged["telemetry"]["hide_inputs"] == false, "preserves telemetry.hide_inputs");
    check(merged["telemetry"]["max_queue_capacity"] == 1000, "preserves telemetry.max_queue_capacity");
    check(merged["telemetry"]["otlp"]["endpoint"] == "http://custom-collector:4318/v1/traces", "overrides 3-level deep otlp.endpoint");
    check(merged["telemetry"]["otlp"]["headers"]["Authorization"] == "Bearer token123", "overrides 3-level deep otlp.headers");
    check(merged["telemetry"]["otlp"]["protocol"] == "http/protobuf", "preserves sibling otlp.protocol");
    check(merged["telemetry"]["otlp"]["max_retries"] == 0, "preserves sibling otlp.max_retries");
    check(merged["telemetry"]["otlp"]["semantics"].size() == 2, "preserves sibling otlp.semantics");
}

static void test_deep_merge_array_replacement() {
    std::puts("\n--- test_deep_merge_array_replacement ---");
    json base = {
        {"cloud_providers", json::array({
            {{"name", "providerA"}, {"base_url", "http://a.com"}}
        })},
        {"telemetry", {
            {"otlp", {
                {"semantics", json::array({"openinference", "otel_genai"})}
            }}
        }}
    };
    json overlay = {
        {"cloud_providers", json::array({
            {{"name", "providerB"}, {"base_url", "http://b.com"}},
            {{"name", "providerC"}, {"base_url", "http://c.com"}}
        })},
        {"telemetry", {
            {"otlp", {
                {"semantics", json::array({"openinference"})}
            }}
        }}
    };

    json merged = JsonUtils::merge(base, overlay);
    check(merged["cloud_providers"].size() == 2, "cloud_providers array is replaced");
    check(merged["cloud_providers"][0]["name"] == "providerB", "first item is providerB");
    check(merged["cloud_providers"][1]["name"] == "providerC", "second item is providerC");
    check(merged["telemetry"]["otlp"]["semantics"].size() == 1, "nested array replaced");
    check(merged["telemetry"]["otlp"]["semantics"][0] == "openinference", "correct array value");
}

static void test_deep_merge_non_object_edge_cases() {
    std::puts("\n--- test_deep_merge_non_object_edge_cases ---");

    // Non-object base + object overlay -> returns overlay
    json non_obj_base = "string_value";
    json obj_overlay = {{"key", "val"}};
    json res1 = JsonUtils::merge(non_obj_base, obj_overlay);
    check(res1.is_object() && res1["key"] == "val", "non-object base returns object overlay");

    // Object base + non-object overlay -> returns overlay
    json obj_base = {{"key", "val"}};
    json non_obj_overlay = 42;
    json res2 = JsonUtils::merge(obj_base, non_obj_overlay);
    check(res2.is_number() && res2.get<int>() == 42, "object base returns non-object overlay");

    // Null base + object overlay
    json null_base = nullptr;
    json res3 = JsonUtils::merge(null_base, obj_overlay);
    check(res3.is_object() && res3["key"] == "val", "null base returns object overlay");

    // Object base + null overlay
    json null_overlay = nullptr;
    json res4 = JsonUtils::merge(obj_base, null_overlay);
    check(res4.is_null(), "null overlay replaces object base");

    // Empty object overlay -> preserves base
    json empty_overlay = json::object();
    json res5 = JsonUtils::merge(obj_base, empty_overlay);
    check(res5 == obj_base, "empty object overlay preserves base");

    // Empty object base -> adopts overlay
    json empty_base = json::object();
    json res6 = JsonUtils::merge(empty_base, obj_overlay);
    check(res6 == obj_overlay, "empty object base adopts overlay");
}

// ============================================================================
// ConfigFile lifecycle & sparse storage unit tests
// ============================================================================

static void test_config_file_missing_creates_no_file() {
    std::puts("\n--- test_config_file_missing_creates_no_file ---");
    ScopedTempDir temp;
    fs::path cfg_file = temp.path() / "config.json";

    check(!fs::exists(cfg_file), "config.json initially absent");

    // Calling ConfigFile::load on missing file should return defaults
    json loaded = ConfigFile::load(temp.string(), temp.string());
    check(loaded.contains("port"), "loaded defaults has port");
    check(loaded.contains("llamacpp"), "loaded defaults has llamacpp");
    check(loaded.contains("telemetry"), "loaded defaults has telemetry");

    // CRITICAL INVARIANT: Missing config.json MUST NOT be created / dumped on load
    check(!fs::exists(cfg_file), "config.json was NOT created on load");

    // load_raw on missing file returns empty object
    json raw = ConfigFile::load_raw(temp.string());
    check(raw.is_object() && raw.empty(), "load_raw returns empty object when absent");
}

static void test_config_file_sparse_merge_and_preservation() {
    std::puts("\n--- test_config_file_sparse_merge_and_preservation ---");
    ScopedTempDir temp;
    fs::path cfg_file = temp.path() / "config.json";

    // Write a minimal sparse override
    json sparse_user = {
        {"config_version", 2},
        {"port", 8888},
        {"llamacpp", {{"backend", "vulkan"}}}
    };
    ConfigFile::save(temp.string(), sparse_user);
    check(fs::exists(cfg_file), "config.json created explicitly via save");

    // Read via load_raw -> only contains sparse keys
    json raw = ConfigFile::load_raw(temp.string());
    check(raw["port"] == 8888, "load_raw has user port");
    check(raw["llamacpp"]["backend"] == "vulkan", "load_raw has user backend");
    check(!raw.contains("host"), "load_raw does NOT have unconfigured host");
    check(!raw.contains("telemetry"), "load_raw does NOT have unconfigured telemetry");

    // Read via load -> has all defaults merged in
    json merged = ConfigFile::load(temp.string(), temp.string());
    check(merged["port"] == 8888, "merged has user port override");
    check(merged["llamacpp"]["backend"] == "vulkan", "merged has user backend override");
    check(merged["host"] == "localhost", "merged has default host");
    check(merged["telemetry"]["enabled"] == false, "merged has default telemetry");
    check(merged["sdcpp"]["backend"] == "auto", "merged has default sdcpp");

    // Verify on-disk file was NOT overwritten with full dump
    json raw_after = ConfigFile::load_raw(temp.string());
    check(!raw_after.contains("host"), "on-disk config.json remains sparse after load");
    check(!raw_after.contains("telemetry"), "on-disk config.json remains sparse after load");
}

static void test_config_file_migration_sparse() {
    std::puts("\n--- test_config_file_migration_sparse ---");
    ScopedTempDir temp;

    // Write a v1 sparse file with ctx_size = 4096 and a custom port
    json v1_sparse = {
        {"config_version", 1},
        {"port", 7777},
        {"ctx_size", 4096},
        {"llamacpp", {{"backend", "cpu"}}}
    };
    ConfigFile::save(temp.string(), v1_sparse);

    // load() triggers migration
    json merged = ConfigFile::load(temp.string(), temp.string());
    check(merged["config_version"] == 2, "merged version is 2");
    check(merged["ctx_size"] == -1, "ctx_size migrated 4096 -> -1");
    check(merged["port"] == 7777, "user port 7777 preserved");
    check(merged["llamacpp"]["backend"] == "cpu", "user backend cpu preserved");

    // Verify on-disk file was updated with migrated version, but STILL sparse!
    json raw_after = ConfigFile::load_raw(temp.string());
    check(raw_after["config_version"] == 2, "on-disk config_version updated to 2");
    check(raw_after["ctx_size"] == -1, "on-disk ctx_size updated to -1");
    check(raw_after["port"] == 7777, "on-disk user port preserved");
    check(!raw_after.contains("host"), "on-disk file did not inject host");
    check(!raw_after.contains("telemetry"), "on-disk file did not inject telemetry");
    check(!raw_after.contains("whispercpp"), "on-disk file did not inject whispercpp");
}

static void test_config_file_corrupt_handling() {
    std::puts("\n--- test_config_file_corrupt_handling ---");
    ScopedTempDir temp;
    fs::path cfg_file = temp.path() / "config.json";
    fs::path backup_file = temp.path() / "config.json.corrupted";

    // Write invalid JSON
    {
        std::ofstream f(cfg_file);
        f << "{ invalid json !!! }";
    }

    json raw = ConfigFile::load_raw(temp.string());
    check(raw.is_object() && raw.empty(), "corrupt file returns empty object");
    check(fs::exists(backup_file), "corrupted file renamed to .corrupted");

    // load() should return valid defaults
    json merged = ConfigFile::load(temp.string(), temp.string());
    check(merged["port"] == 13305, "load returns default port after corruption");
}

// ============================================================================
// Multi-layer distro precedence test
// ============================================================================

static void test_distro_layer_precedence() {
    std::puts("\n--- test_distro_layer_precedence ---");

    json base_defaults = {
        {"port", 13305},
        {"host", "localhost"},
        {"ctx_size", -1},
        {"llamacpp", {
            {"backend", "auto"},
            {"cpu_bin", "builtin"},
            {"vulkan_bin", "builtin"}
        }}
    };

    json distro_defaults = {
        {"llamacpp", {
            {"backend", "vulkan"},
            {"vulkan_bin", "/usr/bin/llama-server-vulkan"}
        }}
    };

    json user_overrides = {
        {"port", 8080},
        {"llamacpp", {
            {"vulkan_bin", "/opt/custom/llama-server"}
        }}
    };

    // Layer 1: base + distro
    json with_distro = JsonUtils::merge(base_defaults, distro_defaults);
    check(with_distro["port"] == 13305, "distro keeps base port");
    check(with_distro["llamacpp"]["backend"] == "vulkan", "distro overrides backend");
    check(with_distro["llamacpp"]["vulkan_bin"] == "/usr/bin/llama-server-vulkan", "distro overrides vulkan_bin");

    // Layer 2: with_distro + user
    json effective = JsonUtils::merge(with_distro, user_overrides);
    check(effective["port"] == 8080, "user overrides port");
    check(effective["host"] == "localhost", "base host preserved");
    check(effective["ctx_size"] == -1, "base ctx_size preserved");
    check(effective["llamacpp"]["backend"] == "vulkan", "distro backend preserved when user does not override");
    check(effective["llamacpp"]["vulkan_bin"] == "/opt/custom/llama-server", "user overrides distro vulkan_bin");
    check(effective["llamacpp"]["cpu_bin"] == "builtin", "base cpu_bin preserved");
}

// ============================================================================
// Pruning matching defaults and unsetting overrides
// ============================================================================

static void test_prune_matching_unit() {
    std::puts("\n--- test_prune_matching_unit ---");

    json defaults = {
        {"port", 13305},
        {"host", "localhost"},
        {"llamacpp", {
            {"backend", "auto"},
            {"args", ""}
        }}
    };

    // Case 1: user config has override that matches default -> gets pruned
    json user_cfg1 = {
        {"port", 13305},
        {"host", "0.0.0.0"}
    };
    JsonUtils::prune_matching(user_cfg1, defaults);
    check(!user_cfg1.contains("port"), "matching port pruned");
    check(user_cfg1["host"] == "0.0.0.0", "non-matching host preserved");

    // Case 2: nested object where some keys match default and some differ
    json user_cfg2 = {
        {"llamacpp", {
            {"backend", "auto"},
            {"args", "--ctx-size 8192"}
        }}
    };
    JsonUtils::prune_matching(user_cfg2, defaults);
    check(!user_cfg2["llamacpp"].contains("backend"), "matching backend pruned");
    check(user_cfg2["llamacpp"]["args"] == "--ctx-size 8192", "custom args preserved");

    // Case 3: nested object where ALL keys match default -> parent object pruned
    json user_cfg3 = {
        {"llamacpp", {
            {"backend", "auto"},
            {"args", ""}
        }}
    };
    JsonUtils::prune_matching(user_cfg3, defaults);
    check(user_cfg3.empty(), "all-matching nested object pruned completely");

    // Case 4: config_version pruned if only config_version remains
    json user_cfg4 = {
        {"config_version", 2},
        {"port", 13305}
    };
    JsonUtils::prune_matching(user_cfg4, defaults);
    check(user_cfg4.empty(), "only-version user config cleared completely");
}

static void test_unset_override_to_default() {
    std::puts("\n--- test_unset_override_to_default ---");
    ScopedTempDir temp;

    // Step 1: User previously configured custom port 9000 and custom backend vulkan
    json user_cfg = {
        {"port", 9000},
        {"llamacpp", {{"backend", "vulkan"}}}
    };
    ConfigFile::save(temp.string(), user_cfg);

    json loaded1 = ConfigFile::load(temp.string(), temp.string());
    check(loaded1["port"] == 9000, "custom port 9000 active");
    check(loaded1["llamacpp"]["backend"] == "vulkan", "custom backend vulkan active");

    // Step 2: User resets port back to default 13305 via handle_config_set workflow
    json disk_cfg = ConfigFile::load_raw(temp.string());
    disk_cfg = JsonUtils::merge(disk_cfg, json{{"port", 13305}});
    JsonUtils::prune_matching(disk_cfg, ConfigFile::get_defaults());
    ConfigFile::save(temp.string(), disk_cfg);

    // Verify on disk: port is no longer pinned as an override!
    json disk_after_port_reset = ConfigFile::load_raw(temp.string());
    check(!disk_after_port_reset.contains("port"), "port override removed from disk");
    check(disk_after_port_reset["llamacpp"]["backend"] == "vulkan", "backend override retained");

    // Step 3: User resets backend back to "auto"
    disk_cfg = JsonUtils::merge(disk_after_port_reset, json{{"llamacpp", {{"backend", "auto"}}}});
    JsonUtils::prune_matching(disk_cfg, ConfigFile::get_defaults());
    ConfigFile::save(temp.string(), disk_cfg);

    // Verify on disk: llamacpp override removed, leaving clean sparse config
    json disk_after_all_reset = ConfigFile::load_raw(temp.string());
    check(disk_after_all_reset.empty(), "all overrides reset -> empty config on disk");

    // Step 4: load() still returns full defaults seamlessly
    json loaded_final = ConfigFile::load(temp.string(), temp.string());
    check(loaded_final["port"] == 13305, "load() returns default port");
    check(loaded_final["llamacpp"]["backend"] == "auto", "load() returns default backend");
}

static void test_cli_startup_persistence_idempotent() {
    std::puts("\n--- test_cli_startup_persistence_idempotent ---");
    ScopedTempDir temp;

    // Simulate startup with --port 9000
    int cli_port = 9000;
    json user_config = ConfigFile::load_raw(temp.string());
    bool changed = false;
    if (cli_port != -1 && (!user_config.contains("port") || user_config["port"] != cli_port)) {
        user_config["port"] = cli_port;
        changed = true;
    }
    check(changed == true, "first start with --port 9000 marks changed=true");
    if (changed) {
        json defaults = ConfigFile::get_defaults();
        JsonUtils::prune_matching(user_config, defaults);
        ConfigFile::save(temp.string(), user_config);
    }

    json disk1 = ConfigFile::load_raw(temp.string());
    check(disk1["port"] == 9000, "disk contains port 9000");

    // Simulate second start with same --port 9000
    json user_config2 = ConfigFile::load_raw(temp.string());
    bool changed2 = false;
    if (cli_port != -1 && (!user_config2.contains("port") || user_config2["port"] != cli_port)) {
        user_config2["port"] = cli_port;
        changed2 = true;
    }
    check(changed2 == false, "second start with same --port 9000 does not churn (changed=false)");

    // Simulate start with default port --port 13305
    int default_port = 13305;
    json user_config3 = ConfigFile::load_raw(temp.string());
    bool changed3 = false;
    if (default_port != -1 && (!user_config3.contains("port") || user_config3["port"] != default_port)) {
        user_config3["port"] = default_port;
        changed3 = true;
    }
    check(changed3 == true, "start with default port marks changed=true");
    if (changed3) {
        json defaults = ConfigFile::get_defaults();
        JsonUtils::prune_matching(user_config3, defaults);
        ConfigFile::save(temp.string(), user_config3);
    }

    json disk3 = ConfigFile::load_raw(temp.string());
    check(!disk3.contains("port"), "passing default port prunes port override from disk");
}

static void test_server_handle_config_set_direct() {
    std::puts("\n--- test_server_handle_config_set_direct ---");
    ScopedTempDir temp;

    json initial_defaults = ConfigFile::get_defaults();
    initial_defaults["auto_check_model_updates"] = false;
    initial_defaults["broadcast"] = false;
    initial_defaults["websocket_port"] = 9001;
    auto runtime_cfg = std::make_shared<RuntimeConfig>(initial_defaults);
    RuntimeConfig::set_global(runtime_cfg.get());
    Server server(runtime_cfg, temp.string(), temp.string());

    // 1. Valid POST /internal/set modifying port, max_loaded_models, and llamacpp args
    httplib::Request req1;
    req1.method = "POST";
    req1.path = "/internal/set";
    req1.body = json{
        {"port", 9000},
        {"max_loaded_models", 2},
        {"llamacpp", {{"args", "-ngl 99"}}}
    }.dump();
    httplib::Response res1;

    server.handle_config_set(req1, res1);
    check(res1.status == 200 || res1.status == -1, "handle_config_set returns 200 on valid body");
    auto res_json1 = json::parse(res1.body);
    check(res_json1["status"] == "success", "response status is success");
    check(res_json1["updated"]["port"] == 9000, "response updated contains port 9000");
    check(res_json1["updated"]["max_loaded_models"] == 2, "response updated contains max_loaded_models 2");
    check(res_json1["updated"]["llamacpp"]["args"] == "-ngl 99", "response updated contains args -ngl 99");

    // Verify on disk: ONLY the sparse overrides are saved!
    json disk1 = ConfigFile::load_raw(temp.string());
    check(disk1["port"] == 9000, "disk contains sparse port 9000");
    check(disk1["max_loaded_models"] == 2, "disk contains sparse max_loaded_models 2");
    check(disk1["llamacpp"]["args"] == "-ngl 99", "disk contains sparse args -ngl 99");
    check(!disk1.contains("host"), "disk does NOT contain default host");
    check(!disk1.contains("telemetry"), "disk does NOT contain default telemetry");
    check(!disk1.contains("sdcpp"), "disk does NOT contain default sdcpp");

    // 2. Query handle_config_get -> returns full effective config snapshot
    httplib::Request req_get;
    req_get.method = "GET";
    httplib::Response res_get;
    server.handle_config_get(req_get, res_get);
    check(res_get.status == 200 || res_get.status == -1, "handle_config_get returns 200");
    auto snap = json::parse(res_get.body);
    check(snap["port"] == 9000, "snapshot has overridden port 9000");
    check(snap["max_loaded_models"] == 2, "snapshot has overridden max_loaded_models 2");
    check(snap["host"] == "localhost", "snapshot has default host localhost");
    check(snap.contains("telemetry"), "snapshot has default telemetry section");

    // 3. Query handle_config_defaults_get -> returns factory defaults
    httplib::Request req_def;
    req_def.method = "GET";
    httplib::Response res_def;
    server.handle_config_defaults_get(req_def, res_def);
    check(res_def.status == 200 || res_def.status == -1, "handle_config_defaults_get returns 200");
    auto base_def = json::parse(res_def.body);
    check(base_def["port"] == 13305, "defaults has factory port 13305");

    // 4. Send POST /internal/set resetting port back to default 13305 -> prunes from disk
    httplib::Request req2;
    req2.method = "POST";
    req2.path = "/internal/set";
    req2.body = json{{"port", 13305}}.dump();
    httplib::Response res2;

    server.handle_config_set(req2, res2);
    check(res2.status == 200 || res2.status == -1, "resetting port returns 200");

    json disk2 = ConfigFile::load_raw(temp.string());
    check(!disk2.contains("port"), "port override pruned from disk on reset");
    check(disk2["max_loaded_models"] == 2, "max_loaded_models override retained on disk");
    check(disk2["llamacpp"]["args"] == "-ngl 99", "args override retained on disk");

    // 5. Send invalid JSON body -> returns 400
    httplib::Request req_bad_json;
    req_bad_json.method = "POST";
    req_bad_json.body = "{ invalid json body }";
    httplib::Response res_bad_json;
    server.handle_config_set(req_bad_json, res_bad_json);
    check(res_bad_json.status == 400, "invalid JSON body returns 400");

    // 6. Send invalid config value (e.g. out-of-range port) -> returns 400
    httplib::Request req_bad_val;
    req_bad_val.method = "POST";
    req_bad_val.body = json{{"port", 999999}}.dump();
    httplib::Response res_bad_val;
    server.handle_config_set(req_bad_val, res_bad_val);
    check(res_bad_val.status == 400, "invalid port value returns 400");

    RuntimeConfig::set_global(nullptr);
}

int main() {
    std::puts("=== Config Merging and Sparse Lifecycle Unit Tests ===\n");

    test_deep_merge_level1();
    test_deep_merge_level2_backend();
    test_deep_merge_level3_telemetry();
    test_deep_merge_array_replacement();
    test_deep_merge_non_object_edge_cases();

    test_config_file_missing_creates_no_file();
    test_config_file_sparse_merge_and_preservation();
    test_config_file_migration_sparse();
    test_config_file_corrupt_handling();

    test_distro_layer_precedence();

    test_prune_matching_unit();
    test_unset_override_to_default();
    test_cli_startup_persistence_idempotent();
    test_server_handle_config_set_direct();

    std::printf("\n================================================\n");
    if (failures > 0) {
        std::printf("Tests finished: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("All config merging tests PASSED (%d passed).\n", passed);
    return 0;
}
