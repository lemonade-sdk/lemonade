// Standalone test for lemon::utils custom arg parsing helpers.
// Build with: cmake --build --preset default --target test_custom_args
// Run with: ctest --test-dir build -R custom_args --output-on-failure

#include "lemon/utils/custom_args.h"

#include <cstdio>
#include <string>
#include <vector>

using lemon::utils::build_custom_args_map;
using lemon::utils::parse_custom_args;

static bool expect_values(const char* name, const std::string& input,
                          const std::string& flag,
                          const std::vector<std::string>& expected) {
    auto m = build_custom_args_map(parse_custom_args(input));
    auto it = m.find(flag);
    bool ok = (it != m.end()) && (it->second == expected);
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    return ok;
}

int main() {
    int failures = 0;

    // Regression: a '-' followed by a digit or '.' is a value, not a flag.
    failures += !expect_values(
        "negative integer value", "--cache-ram -1", "--cache-ram", {"-1"});
    failures += !expect_values(
        "negative float value", "--temp -0.5", "--temp", {"-0.5"});
    failures += !expect_values(
        "negative leading-dot float", "--temp -.5", "--temp", {"-.5"});

    // Regression: identical negative values under two flags must both survive
    // (treating -1 as a flag would dedupe it as a map key, dropping values).
    {
        auto m = build_custom_args_map(
            parse_custom_args("--cache-ram -1 --reasoning-budget -1"));
        bool ok = m["--cache-ram"] == std::vector<std::string>{"-1"}
               && m["--reasoning-budget"] == std::vector<std::string>{"-1"};
        std::printf("[%s] duplicate negative value under two flags\n",
                    ok ? "PASS" : "FAIL");
        failures += !ok;
    }

    // Sanity: short and long flags still parse as flags with their values.
    failures += !expect_values("short flag", "-ngl 99", "-ngl", {"99"});
    failures += !expect_values("long flag", "--threads 8", "--threads", {"8"});

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
