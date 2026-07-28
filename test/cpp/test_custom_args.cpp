// Standalone test for lemon::utils custom arg parsing helpers.
// Build with: cmake --build --preset default --target test_custom_args
// Run with: ctest --test-dir build -R '^CustomArgsTest$' --output-on-failure

#include "lemon/utils/custom_args.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using lemon::utils::build_custom_args_map;
using lemon::utils::parse_custom_args;

using ArgMap = std::map<std::string, std::vector<std::string>>;

static std::string dump(const ArgMap& m) {
    std::string s = "{";
    for (const auto& [flag, vals] : m) {
        s += " " + flag + ": [";
        for (size_t i = 0; i < vals.size(); ++i) {
            if (i) s += ", ";
            s += vals[i];
        }
        s += "]";
    }
    return s + " }";
}

static bool expect_map(const char* name, const std::string& input,
                       const ArgMap& expected) {
    ArgMap actual = build_custom_args_map(parse_custom_args(input));
    bool ok = (actual == expected);
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        std::printf("  got:  %s\n  want: %s\n",
                    dump(actual).c_str(), dump(expected).c_str());
    }
    return ok;
}

int main() {
    int failures = 0;

    // Negative numbers attach to their flag and never become phantom keys.
    failures += !expect_map(
        "negative integer value", "--cache-ram -1", {{"--cache-ram", {"-1"}}});
    failures += !expect_map(
        "negative float value", "--temp -0.5", {{"--temp", {"-0.5"}}});
    failures += !expect_map(
        "negative leading-dot float", "--temp -.5", {{"--temp", {"-.5"}}});

    // Scientific notation.
    failures += !expect_map(
        "scientific notation", "--threshold -1e5", {{"--threshold", {"-1e5"}}});
    failures += !expect_map(
        "scientific notation with negative exponent",
        "--threshold -1.5e-3", {{"--threshold", {"-1.5e-3"}}});
    failures += !expect_map(
        "scientific notation capital E",
        "--val -1E10", {{"--val", {"-1E10"}}});

    // Identical negatives under two flags must both survive (std::map keys
    // are unique; treating -1 as a flag would dedupe it, dropping values).
    failures += !expect_map(
        "duplicate negative under two flags (no phantom key)",
        "--cache-ram -1 --reasoning-budget -1",
        {{"--cache-ram", {"-1"}}, {"--reasoning-budget", {"-1"}}});

    // Sanity: short and long flags still parse as flags with their values.
    failures += !expect_map(
        "short flag with value", "-ngl 99", {{"-ngl", {"99"}}});
    failures += !expect_map(
        "long flag with value", "--threads 8", {{"--threads", {"8"}}});

    // Counterexamples: incomplete numerics are flags, not values.
    failures += !expect_map(
        "-1foo is a flag, not a value",
        "--foo -1foo", {{"--foo", {}}, {"-1foo", {}}});
    failures += !expect_map(
        "-.future is a flag, not a value",
        "--foo -.future", {{"--foo", {}}, {"-.future", {}}});

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
