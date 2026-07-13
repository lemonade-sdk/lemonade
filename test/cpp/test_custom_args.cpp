// Test for lemon::utils::parse_custom_args() — the quote-aware tokenizer the
// backends use to split a free-form user-args string into argv elements.
// Build with: cmake --build --preset default --target test_custom_args
// Run with: ctest --test-dir build -R custom_args --output-on-failure

#include "lemon/utils/custom_args.h"

#include <cstdio>
#include <string>
#include <vector>

using lemon::utils::parse_custom_args;

static bool expect_tokens(const char* name,
                          const std::string& input,
                          const std::vector<std::string>& expected) {
    std::vector<std::string> got = parse_custom_args(input);
    bool ok = got == expected;
    if (!ok) {
        std::printf("FAIL: %s\n  input:    [%s]\n  expected:", name, input.c_str());
        for (const auto& t : expected) std::printf(" [%s]", t.c_str());
        std::printf("\n  got:     ");
        for (const auto& t : got) std::printf(" [%s]", t.c_str());
        std::printf("\n");
    } else {
        std::printf("ok: %s\n", name);
    }
    return ok;
}

int main() {
    int failures = 0;

    failures += !expect_tokens(
        "plain args split on whitespace",
        "--max-num-seqs 4 --gpu-memory-utilization 0.8",
        {"--max-num-seqs", "4", "--gpu-memory-utilization", "0.8"});

    // The core regression: a double-quoted value with spaces stays one argument.
    failures += !expect_tokens(
        "double-quoted value with spaces is one token",
        "--some-option \"value with spaces\"",
        {"--some-option", "value with spaces"});

    failures += !expect_tokens(
        "single-quoted value with spaces is one token",
        "--some-option 'value with spaces'",
        {"--some-option", "value with spaces"});

    // Single-quote a JSON value so its inner double quotes are preserved.
    failures += !expect_tokens(
        "single-quoted JSON value survives intact",
        "--override '{\"key\": \"a b\"}'",
        {"--override", "{\"key\": \"a b\"}"});

    failures += !expect_tokens(
        "empty string yields no tokens",
        "",
        {});

    // Escaping is done via quotes, not backslashes: an unquoted backslash is a
    // literal character and does NOT join tokens across a space. Documents the
    // shared parser's behavior so callers rely on quoting for spaces.
    failures += !expect_tokens(
        "unquoted backslash is literal; quoting is the escape mechanism",
        "a\\ b",
        {"a\\", "b"});

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
