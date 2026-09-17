// Standalone test for lemon::utils custom arg parsing helpers.
// Build with: cmake --build --preset default --target test_custom_args
// Run with: ctest --test-dir build -R '^CustomArgsTest$' --output-on-failure

#include "lemon/utils/custom_args.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using lemon::utils::build_custom_args_map;
using lemon::utils::custom_args_has_flag;
using lemon::utils::map_to_args_string;
using lemon::utils::merge_args_maps;
using lemon::utils::parse_custom_args;

using ArgMap = lemon::utils::CustomArgsMap;

static std::string dump(const ArgMap& m) {
    std::string s = "{";
    for (const auto& [flag, occurrences] : m) {
        s += " " + flag + ":";
        for (const auto& vals : occurrences) {
            s += " [";
            for (size_t i = 0; i < vals.size(); ++i) {
                if (i) s += ", ";
                s += vals[i];
            }
            s += "]";
        }
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

static bool expect_merge(const char* name, const std::string& target,
                         const std::string& incoming,
                         const std::string& expected) {
    ArgMap target_map = build_custom_args_map(parse_custom_args(target));
    ArgMap incoming_map = build_custom_args_map(parse_custom_args(incoming));
    std::string actual = map_to_args_string(merge_args_maps(target_map, incoming_map));
    bool ok = (actual == expected);
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        std::printf("  got:  %s\n  want: %s\n",
                    actual.c_str(), expected.c_str());
    }
    return ok;
}

static bool expect_has_flag(const char* name, const std::string& input,
                            const std::string& flag, bool expected) {
    bool actual = custom_args_has_flag(parse_custom_args(input), flag);
    bool ok = (actual == expected);
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        std::printf("  got:  %d\n  want: %d\n", actual, expected);
    }
    return ok;
}

int main() {
    int failures = 0;

    // Negative numbers attach to their flag and never become phantom keys.
    failures += !expect_map(
        "negative integer value", "--cache-ram -1", {{"--cache-ram", {{"-1"}}}});
    failures += !expect_map(
        "negative float value", "--temp -0.5", {{"--temp", {{"-0.5"}}}});
    failures += !expect_map(
        "negative leading-dot float", "--temp -.5", {{"--temp", {{"-.5"}}}});

    // Scientific notation.
    failures += !expect_map(
        "scientific notation", "--threshold -1e5", {{"--threshold", {{"-1e5"}}}});
    failures += !expect_map(
        "scientific notation with negative exponent",
        "--threshold -1.5e-3", {{"--threshold", {{"-1.5e-3"}}}});
    failures += !expect_map(
        "scientific notation capital E",
        "--val -1E10", {{"--val", {{"-1E10"}}}});

    // Identical negatives under two flags must both survive (std::map keys
    // are unique; treating -1 as a flag would dedupe it, dropping values).
    failures += !expect_map(
        "duplicate negative under two flags (no phantom key)",
        "--cache-ram -1 --reasoning-budget -1",
        {{"--cache-ram", {{"-1"}}}, {"--reasoning-budget", {{"-1"}}}});

    // Sanity: short and long flags still parse as flags with their values.
    failures += !expect_map(
        "short flag with value", "-ngl 99", {{"-ngl", {{"99"}}}});
    failures += !expect_map(
        "long flag with value", "--threads 8", {{"--threads", {{"8"}}}});

    // Counterexamples: incomplete numerics are flags, not values.
    failures += !expect_map(
        "-1foo is a flag, not a value",
        "--foo -1foo", {{"--foo", {{}}}, {"-1foo", {{}}}});
    failures += !expect_map(
        "-.future is a flag, not a value",
        "--foo -.future", {{"--foo", {{}}}, {"-.future", {{}}}});

    failures += !expect_map(
        "repeatable flag occurrences",
        "--override-kv a=bool:false --override-kv b=bool:false",
        {{"--override-kv", {{"a=bool:false"}, {"b=bool:false"}}}});

    failures += !expect_merge(
        "target repeatable flag takes precedence",
        "--override-kv a=bool:false --override-kv b=bool:false --threads 8",
        "--override-kv default=bool:true --threads 4 --cache-type-k q8_0",
        "--cache-type-k q8_0 --override-kv a=bool:false --override-kv b=bool:false --threads 8");
    failures += !expect_merge(
        "incoming repeatable flag is preserved",
        "--threads 8",
        "--override-kv a=bool:false --override-kv b=bool:false",
        "--override-kv a=bool:false --override-kv b=bool:false --threads 8");
    failures += !expect_merge(
        "binary negation precedence is preserved",
        "--no-mmap",
        "--mmap --override-kv a=bool:false --override-kv b=bool:false",
        "--no-mmap --override-kv a=bool:false --override-kv b=bool:false");

    // Overridable-arg detection must compare complete flag tokens, not
    // substrings, so a flag name appearing only inside a value or file path
    // does not suppress a Lemonade default (regression for llama.cpp arg
    // handling, e.g. "--load-mode none" and the -lm / --mmap aliases).
    failures += !expect_has_flag(
        "real long alias token matches",
        "--no-mmap", "--no-mmap", true);
    failures += !expect_has_flag(
        "real short alias token matches",
        "-lm", "-lm", true);
    failures += !expect_has_flag(
        "key with equals-value form matches",
        "--load-mode=auto", "--load-mode", true);
    failures += !expect_has_flag(
        "equals-value alias matches",
        "--mmap=auto", "--mmap", true);
    failures += !expect_has_flag(
        "alias inside path does not match",
        "--lora /models/alma-lm-adapter.gguf", "-lm", false);
    failures += !expect_has_flag(
        "long alias inside value does not match",
        "--override-kv tokenizer.mmap=auto", "--mmap", false);
    failures += !expect_has_flag(
        "missing alias does not match",
        "--threads 8", "--mmap", false);

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
