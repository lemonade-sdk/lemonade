#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>
#include <lemon/runtime_config.h>

#include "test_config_helpers.h"

using json = nlohmann::json;
using lemon::RuntimeConfig;
using test_helpers::check;
using test_helpers::report_results;

static int64_t getter_for(const json& cfg) {
    return RuntimeConfig(cfg).download_rate_limit_bytes_per_second();
}

static bool set_throws(const json& cfg, const json& changes) {
    try {
        RuntimeConfig(cfg).set(changes);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

int main() {
    std::puts("=== RUNNING DOWNLOAD RATE LIMIT CONFIG TESTS ===");

    const int64_t KIB = 1024LL;
    const int64_t MIB = 1024LL * 1024LL;
    const int64_t GIB = 1024LL * 1024LL * 1024LL;

    auto cfg_with = [](const std::string& limit) {
        return json::object({{"download_rate_limit", limit}});
    };

    // --- Getter reads download_rate_limit string ---
    check(getter_for(cfg_with("512")) == 512, "'512' -> 512 B/s");
    check(getter_for(cfg_with("100K")) == 100 * KIB, "'100K' -> 102400 B/s");
    check(getter_for(cfg_with("100k")) == 100 * KIB, "'100k' -> 102400 B/s");
    check(getter_for(cfg_with("100KB")) == 100 * KIB, "'100KB' -> 102400 B/s");
    check(getter_for(cfg_with("10M")) == 10 * MIB, "'10M' -> 10 MiB/s");
    check(getter_for(cfg_with("10MB")) == 10 * MIB, "'10MB' -> 10 MiB/s");
    check(getter_for(cfg_with("2g")) == 2 * GIB, "'2g' -> 2 GiB/s");
    check(getter_for(cfg_with("1.5G")) == GIB + GIB / 2, "'1.5G' -> 1.5 GiB/s");
    check(getter_for(cfg_with("0.12345K")) == 126, "fractional precision kept within unit resolution");
    check(getter_for(cfg_with("  10M  ")) == 10 * MIB, "trims surrounding whitespace");
    check(getter_for(cfg_with("0")) == 0, "'0' -> unlimited");
    check(getter_for(cfg_with("")) == 0, "empty string -> unlimited");
    check(getter_for(cfg_with("-5")) == 0, "invalid '-5' -> unlimited");
    check(getter_for(cfg_with("10M5")) == 0, "invalid '10M5' -> unlimited");
    check(getter_for(cfg_with("1e3")) == 0, "invalid scientific -> unlimited");
    check(getter_for(cfg_with("1.5")) == 0, "fraction without unit -> invalid (unlimited)");

    // Missing key / not a string -> unlimited.
    check(getter_for(json::object({{"download_rate_limit", 123}})) == 0, "non-string value -> 0");
    check(getter_for(json::object({})) == 0, "missing key -> 0");

    // --- validate()/set() accepts valid values ---
    check(!set_throws(cfg_with("10M"), json::object({{"download_rate_limit", "20M"}})), "set accepts change");
    check(!set_throws(cfg_with("10M"), json::object({{"download_rate_limit", ""}})), "set accepts clearing");
    check(!set_throws(cfg_with(""), json::object({{"download_rate_limit", "10M"}})), "set accepts setting from empty");
    check(!set_throws(cfg_with("10M"), json::object({{"download_rate_limit", "0"}})), "set accepts explicit '0'");

    // --- validate()/set() rejects invalid values ---
    check(set_throws(cfg_with("10M"), json::object({{"download_rate_limit", 123}})), "rejects non-string value");
    check(set_throws(cfg_with("10M"), json::object({{"download_rate_limit", "10M5"}})), "rejects malformed value");
    check(set_throws(cfg_with("10M"), json::object({{"download_rate_limit", "-5"}})), "rejects negative value");

    // --- apply_changes updates the cap ---
    {
        RuntimeConfig rc(cfg_with("10M"));
        rc.set(json::object({{"download_rate_limit", "50M"}}));
        check(getter_for(rc.snapshot()) == 50 * MIB, "cap update applied");

        rc.set(json::object({{"download_rate_limit", "50M"}}));
        check(getter_for(rc.snapshot()) == 50 * MIB, "idempotent re-apply");

        rc.set(json::object({{"download_rate_limit", ""}}));
        check(getter_for(rc.snapshot()) == 0, "clearing applied");
    }

    return report_results("download rate limit");
}
