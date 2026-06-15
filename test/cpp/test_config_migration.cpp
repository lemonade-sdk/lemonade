// Standalone unit tests for lemon::config_get_version(),
// lemon::config_migrate_v1_to_v2(), and lemon::config_migrate().
// Tests cover ctx_size migration (4096 → -1) and version bumping.
//
// Compile with:
//   g++ -std=c++17 -I src/cpp/include -I src/cpp/build/_deps/json-src/single_include \
//       test/cpp/test_config_migration.cpp -o config_migration_test
//
// Run with:
//   ./config_migration_test

#include <cassert>
#include <cstdio>
#include <iostream>

#include <lemon/config_file.h>

using json = nlohmann::json;
using lemon::config_get_version;
using lemon::config_migrate_v1_to_v2;
using lemon::config_migrate;

// Minimal deep-merge implementation — same logic as JsonUtils::merge,
// pulled in to avoid linking against platform-dependent code.
static json deep_merge(const json& base, const json& overlay) {
    json result = base;
    if (!overlay.is_object()) return overlay;
    for (auto it = overlay.begin(); it != overlay.end(); ++it) {
        if (result.contains(it.key()) && result[it.key()].is_object()
            && it.value().is_object()) {
            result[it.key()] = deep_merge(result[it.key()], it.value());
        } else {
            result[it.key()] = it.value();
        }
    }
    return result;
}

// ============================================================================
// Test helpers
// ============================================================================

static int passed = 0;
static int failures = 0;

static void check(bool cond, const char* desc) {
    if (cond) {
        std::printf("[PASS] %s\n", desc);
        ++passed;
    } else {
        std::printf("[FAIL] %s\n", desc);
        ++failures;
    }
}

// ============================================================================
// config_get_version
// ============================================================================

static void test_config_get_version() {
    std::puts("--- config_get_version ---");

    // Missing key → 0
    json empty;
    check(config_get_version(empty) == 0, "missing config_version → 0");

    // Non-integer → 0
    json str_ver;
    str_ver["config_version"] = "2";
    check(config_get_version(str_ver) == 0, "string config_version → 0");

    // Null → 0
    json null_ver;
    null_ver["config_version"] = json(nullptr);
    check(config_get_version(null_ver) == 0, "null config_version → 0");

    // Valid integers
    json v1;
    v1["config_version"] = 1;
    check(config_get_version(v1) == 1, "config_version = 1 → 1");

    json v2;
    v2["config_version"] = 2;
    check(config_get_version(v2) == 2, "config_version = 2 → 2");

    json big_v;
    big_v["config_version"] = 999;
    check(config_get_version(big_v) == 999, "config_version = 999 → 999");
}

// ============================================================================
// config_migrate_v1_to_v2
// ============================================================================

static void test_migrate_v1_v2_basic() {
    std::puts("\n--- config_migrate_v1_to_v2: basic migration ---");

    // Basic v1 config → v2, version bumped, other fields preserved
    json cfg;
    cfg["config_version"] = 1;
    cfg["port"] = 13305;
    cfg["ctx_size"] = 4096;

    check(config_migrate_v1_to_v2(cfg) == true, "returns true");
    check(cfg["config_version"] == 2, "config_version bumped to 2");
    check(cfg["port"] == 13305, "other fields preserved");
}

static void test_migrate_v1_v2_ctx_size_default() {
    std::puts("\n--- config_migrate_v1_to_v2: ctx_size = 4096 (old default) ---");

    json cfg;
    cfg["config_version"] = 1;
    cfg["ctx_size"] = 4096;

    config_migrate_v1_to_v2(cfg);
    check(cfg["ctx_size"] == -1, "ctx_size 4096 → -1 (auto-tune)");
}

static void test_migrate_v1_v2_ctx_size_user_tuned() {
    std::puts("\n--- config_migrate_v1_to_v2: ctx_size user-tuned values preserved ---");

    // Non-default positive value → preserved
    json cfg1;
    cfg1["config_version"] = 1;
    cfg1["ctx_size"] = 8192;

    config_migrate_v1_to_v2(cfg1);
    check(cfg1["ctx_size"] == 8192, "ctx_size 8192 preserved (user-tuned)");

    // Zero → preserved (edge case)
    json cfg2;
    cfg2["config_version"] = 1;
    cfg2["ctx_size"] = 0;

    config_migrate_v1_to_v2(cfg2);
    check(cfg2["ctx_size"] == 0, "ctx_size 0 preserved");

    // Negative value → preserved
    json cfg3;
    cfg3["config_version"] = 1;
    cfg3["ctx_size"] = -1;

    config_migrate_v1_to_v2(cfg3);
    check(cfg3["ctx_size"] == -1, "ctx_size -1 preserved");

    // String value → not modified (type guard)
    json cfg4;
    cfg4["config_version"] = 1;
    cfg4["ctx_size"] = "auto";

    config_migrate_v1_to_v2(cfg4);
    check(cfg4["ctx_size"] == "auto", "ctx_size string 'auto' preserved");
}

static void test_migrate_v1_v2_no_ctx_size() {
    std::puts("\n--- config_migrate_v1_to_v2: no ctx_size field ---");

    json cfg;
    cfg["config_version"] = 1;
    // no ctx_size field

    config_migrate_v1_to_v2(cfg);
    check(!cfg.contains("ctx_size"), "ctx_size not added when absent");
    check(cfg["config_version"] == 2, "config_version still bumped");
}

static void test_migrate_v1_v2_preserves_other_fields() {
    std::puts("\n--- config_migrate_v1_to_v2: preserves unrelated fields ---");

    json cfg;
    cfg["config_version"] = 1;
    cfg["port"] = 9999;
    cfg["host"] = "0.0.0.0";
    cfg["log_level"] = "debug";
    cfg["llamacpp"] = json{{"backend", "vulkan"}};
    cfg["whispercpp"] = json{{"backend", "cpu"}};

    config_migrate_v1_to_v2(cfg);

    check(cfg["port"] == 9999, "port preserved");
    check(cfg["host"] == "0.0.0.0", "host preserved");
    check(cfg["log_level"] == "debug", "log_level preserved");
    check(cfg["llamacpp"]["backend"] == "vulkan", "llamacpp.backend preserved");
    check(cfg["whispercpp"]["backend"] == "cpu", "whispercpp.backend preserved");
}

// ============================================================================
// config_migrate (top-level)
// ============================================================================

static void test_migrate_already_at_target() {
    std::puts("\n--- config_migrate: already at target version ---");

    json cfg;
    cfg["config_version"] = 2;
    cfg["port"] = 13305;

    // When original_version matches the target, no migration runs.
    bool changed = config_migrate(cfg, cfg, 2);
    check(changed == false, "already at v2 → no changes, returns false");
    check(cfg["config_version"] == 2, "config_version unchanged");

    // When original_version is missing, falls back to reading config
    // (which is v2), so still no migration.
    json cfg2;
    cfg2["config_version"] = 2;
    cfg2["port"] = 13305;
    bool changed2 = config_migrate(cfg2, cfg2, -1);  // -1 = "unknown"
    check(changed2 == false, "unknown original but config says v2 → no changes");
}

static void test_migrate_v0_via_merge() {
    std::puts("\n--- config_migrate: v0 config (no version) → v2 ---");

    // Simulate a pre-version config merged with current defaults
    json user_cfg;
    user_cfg["port"] = 13305;
    user_cfg["host"] = "localhost";
    user_cfg["ctx_size"] = 4096;

    json defaults;
    defaults["config_version"] = 2;
    defaults["port"] = 13305;
    defaults["host"] = "localhost";
    defaults["ctx_size"] = -1;

    // Deep-merge (user values win)
    json merged = deep_merge(defaults, user_cfg);

    // merged has config_version = 2 from defaults, but original user cfg had none.
    bool changed = config_migrate(merged, defaults, 0);  // 0 = no version = v0
    check(changed == true, "v0 → v2 migration triggers");
    check(merged["ctx_size"] == -1, "ctx_size 4096 → -1");
    check(merged["config_version"] == 2, "config_version = 2");
    check(merged["port"] == 13305, "port preserved");
}

static void test_migrate_v1_partial_fields() {
    std::puts("\n--- config_migrate: v1 with only version and port ---");

    json cfg;
    cfg["config_version"] = 1;
    cfg["port"] = 13305;

    json defaults;
    defaults["config_version"] = 2;

    bool changed = config_migrate(cfg, defaults, 1);  // original version = 1
    check(changed == true, "v1 → v2 triggers");
    check(cfg["config_version"] == 2, "config_version = 2");
    check(cfg["port"] == 13305, "port preserved");
}

static void test_migrate_v1_no_ctx_change() {
    std::puts("\n--- config_migrate: v1 with ctx_size != 4096 ---");

    json cfg;
    cfg["config_version"] = 1;
    cfg["ctx_size"] = 8192;  // user-tuned, not the old default
    cfg["port"] = 13305;

    json defaults;
    defaults["config_version"] = 2;

    bool changed = config_migrate(cfg, defaults, 1);  // original version = 1
    check(changed == true, "v1 → v2 still triggers (version bump)");
    check(cfg["ctx_size"] == 8192, "ctx_size 8192 not changed");
    check(cfg["config_version"] == 2, "config_version = 2");
}

static void test_migrate_no_version_field() {
    std::puts("\n--- config_migrate: config with no config_version field ---");

    json cfg;
    cfg["port"] = 13305;
    // no config_version

    json defaults;
    defaults["config_version"] = 2;

    // cfg has no config_version field, so config_get_version returns 0.
    // Passing -1 (unknown) should fall back to reading config (→ 0) → still < 2.
    bool changed = config_migrate(cfg, defaults, -1);
    check(changed == true, "missing version → migration runs");
    check(cfg["config_version"] == 2, "config_version set to 2");
}

static void test_migrate_no_changes_when_at_target() {
    std::puts("\n--- config_migrate: already at v2, no-op ---");

    json cfg;
    cfg["config_version"] = 2;
    cfg["port"] = 13305;

    json defaults;
    defaults["config_version"] = 2;
    defaults["port"] = 13305;

    bool changed = config_migrate(cfg, defaults, 2);  // already at v2
    check(changed == false, "at v2 with identical defaults → no changes");
}

static void test_migrate_preserves_user_override() {
    std::puts("\n--- config_migrate: user ctx_size override preserved ---");

    json cfg;
    cfg["config_version"] = 1;
    cfg["ctx_size"] = 16384;  // user explicitly set a different value

    json defaults;
    defaults["config_version"] = 2;
    defaults["ctx_size"] = -1;

    json merged = deep_merge(defaults, cfg);
    bool changed = config_migrate(merged, defaults, 1);  // original = 1
    check(changed == true, "migration ran");
    check(merged["ctx_size"] == 16384, "user ctx_size 16384 preserved despite defaults = -1");
}

// ============================================================================
// Integration: merge + migrate round-trip
// ============================================================================

static void test_round_trip() {
    std::puts("\n--- merge + migrate round-trip ---");

    // Simulate loading an old config from disk.
    // Step 1: merge with fresh defaults
    json old_user_cfg;
    old_user_cfg["config_version"] = 1;
    old_user_cfg["port"] = 9999;
    old_user_cfg["host"] = "0.0.0.0";
    old_user_cfg["ctx_size"] = 4096;
    old_user_cfg["llamacpp"] = json{{"backend", "auto"}};

    json defaults;
    defaults["config_version"] = 2;
    defaults["port"] = 13305;
    defaults["host"] = "localhost";
    defaults["ctx_size"] = -1;

    json merged = deep_merge(defaults, old_user_cfg);
    bool changed = config_migrate(merged, defaults, 1);  // original = 1

    check(changed == true, "migration applied after merge");
    check(merged["port"] == 9999, "user port 9999 preserved (user value wins merge)");
    check(merged["host"] == "0.0.0.0", "user host preserved");
    check(merged["ctx_size"] == -1, "ctx_size migrated from 4096 to -1");
    check(merged["config_version"] == 2, "version = 2");
    check(merged["llamacpp"]["backend"] == "auto", "backend config preserved");

    // Verify a second migration pass is a no-op (merged now says v2)
    bool changed2 = config_migrate(merged, defaults, 2);
    check(changed2 == false, "second migrate pass is no-op");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::puts("=== Config Migration Unit Tests ===\n");

    test_config_get_version();
    test_migrate_v1_v2_basic();
    test_migrate_v1_v2_ctx_size_default();
    test_migrate_v1_v2_ctx_size_user_tuned();
    test_migrate_v1_v2_no_ctx_size();
    test_migrate_v1_v2_preserves_other_fields();
    test_migrate_already_at_target();
    test_migrate_v0_via_merge();
    test_migrate_v1_partial_fields();
    test_migrate_v1_no_ctx_change();
    test_migrate_no_version_field();
    test_migrate_no_changes_when_at_target();
    test_migrate_preserves_user_override();
    test_round_trip();

    std::printf("\n%d passed, %d failures\n", passed, failures);
    return failures == 0 ? 0 : 1;
}
