#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

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

static std::vector<std::string> options_of(const json& cfg) {
    if (!cfg.contains("download_rate_limit_options") ||
        !cfg["download_rate_limit_options"].is_array()) {
        return {};
    }
    std::vector<std::string> out;
    for (const auto& r : cfg["download_rate_limit_options"]) {
        out.push_back(r.get<std::string>());
    }
    return out;
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

    // Flat form: download_rate_limit (string cap) + download_rate_limit_options (array).
    auto flat = [](const std::string& d, json opts) {
        return json::object({{"download_rate_limit", d},
                             {"download_rate_limit_options", opts}});
    };

    // --- Getter reads download_rate_limit string ---
    check(getter_for(flat("512", json::array())) == 512, "default '512' -> 512 B/s");
    check(getter_for(flat("100K", json::array())) == 100 * KIB, "default '100K' -> 102400 B/s");
    check(getter_for(flat("100k", json::array())) == 100 * KIB, "default '100k' -> 102400 B/s");
    check(getter_for(flat("100KB", json::array())) == 100 * KIB, "default '100KB' -> 102400 B/s");
    check(getter_for(flat("10M", json::array())) == 10 * MIB, "default '10M' -> 10 MiB/s");
    check(getter_for(flat("10MB", json::array())) == 10 * MIB, "default '10MB' -> 10 MiB/s");
    check(getter_for(flat("2g", json::array())) == 2 * GIB, "default '2g' -> 2 GiB/s");
    check(getter_for(flat("1.5G", json::array())) == GIB + GIB / 2, "default '1.5G' -> 1.5 GiB/s");
    check(getter_for(flat("0.12345K", json::array())) == 126, "fractional precision kept within unit resolution");
    check(getter_for(flat("  10M  ", json::array())) == 10 * MIB, "default trims surrounding whitespace");
    check(getter_for(flat("0", json::array())) == 0, "default '0' -> unlimited");
    check(getter_for(flat("", json::array({"10M", "50M"}))) == 0, "empty default -> unlimited (options still configured)");
    check(getter_for(flat("-5", json::array())) == 0, "invalid '-5' default -> unlimited");
    check(getter_for(flat("10M5", json::array())) == 0, "invalid '10M5' default -> unlimited");
    check(getter_for(flat("1e3", json::array())) == 0, "invalid scientific default -> unlimited");
    check(getter_for(flat("1.5", json::array())) == 0, "fraction without unit -> invalid (unlimited)");

    // Missing key / not a string -> unlimited.
    check(getter_for(json::object({{"download_rate_limit", 123}})) == 0, "non-string default -> 0");
    check(getter_for(json::object({})) == 0, "missing key -> 0");

    // --- validate()/set() accepts the flat form ---
    check(!set_throws(flat("10M", json::array()), flat("20M", json::array())), "set accepts default change");
    check(!set_throws(flat("10M", json::array()), json::object({{"download_rate_limit_options", json::array({"50M"})}})), "set accepts options-only change");
    check(!set_throws(flat("10M", json::array()), json::object({{"download_rate_limit", ""}})), "set accepts clearing default");
    check(!set_throws(flat("", json::array({"10M", "50M"})), flat("10M", json::array())), "set accepts setting default from empty");
    check(!set_throws(flat("10M", json::array()), json::object({{"download_rate_limit_options", json::array({"25M", "100M"})}})), "set accepts multi options change");

    // --- validate()/set() rejects invalid default values ---
    check(set_throws(flat("10M", json::array()), json::object({{"download_rate_limit", 123}})), "rejects non-string default");
    check(set_throws(flat("10M", json::array()), json::object({{"download_rate_limit", "10M5"}})), "rejects malformed default");
    check(set_throws(flat("10M", json::array()), json::object({{"download_rate_limit", "-5"}})), "rejects negative default");

    // --- validate()/set() rejects invalid options ---
    check(set_throws(flat("10M", json::array()), json::object({{"download_rate_limit_options", "notarray"}})), "rejects non-array options");
    check(set_throws(flat("10M", json::array()), json::object({{"download_rate_limit_options", json::array({"10M5"})}})), "rejects malformed option");
    check(set_throws(flat("10M", json::array()), json::object({{"download_rate_limit_options", json::array({123})}})), "rejects non-string option");
    check(set_throws(flat("10M", json::array()), json::object({{"download_rate_limit_options", json::array({"10M", "bad"})}})), "rejects one malformed option");

    // --- apply_changes keeps both keys independent ---
    {
        RuntimeConfig rc(flat("10M", json::array({"10M", "50M"})));
        rc.set(json::object({{"download_rate_limit", "50M"}}));
        check(getter_for(rc.snapshot()) == 50 * MIB, "default update applied");
        check(options_of(rc.snapshot()) == std::vector<std::string>{"10M", "50M"}, "options preserved after default update");

        rc.set(json::object({{"download_rate_limit", "50M"}}));
        check(getter_for(rc.snapshot()) == 50 * MIB, "idempotent default re-apply");

        rc.set(json::object({{"download_rate_limit_options", json::array({"25M", "100M"})}}));
        check(getter_for(rc.snapshot()) == 50 * MIB, "default preserved after options update");
        check(options_of(rc.snapshot()) == std::vector<std::string>{"25M", "100M"}, "options updated");
    }

    return report_results("download rate limit");
}
