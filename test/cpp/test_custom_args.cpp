// Test for lemon::utils::parse_custom_args() — the quote-aware tokenizer the
// backends use to split a free-form user-args string into argv elements.
// Build with: cmake --build --preset default --target test_custom_args
// Run with: ctest --test-dir build -R custom_args --output-on-failure

#include "lemon/utils/custom_args.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using lemon::utils::parse_custom_args;
using lemon::utils::validate_custom_args;

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

// Mirrors the flags vLLM-Omni manages on the launch command line.
static const std::set<std::string> kReserved = {
    "--host", "--port", "--served-model-name", "--max-model-len", "--deploy-config"};

static bool expect_reserved(const char* name,
                            const std::string& input,
                            bool should_reject) {
    bool rejected = !validate_custom_args(input, kReserved).empty();
    bool ok = rejected == should_reject;
    std::printf("%s: %s (%s)\n", ok ? "ok" : "FAIL", name,
                rejected ? "rejected" : "allowed");
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

    // Reserved-flag validation: managed launch args must be rejected in both
    // the "--flag value" and "--flag=value" forms.
    failures += !expect_reserved("--port <value> is rejected", "--port 9999", true);
    failures += !expect_reserved("--port=<value> is rejected", "--port=9999", true);
    failures += !expect_reserved("--deploy-config is rejected", "--deploy-config /tmp/x.yaml", true);
    failures += !expect_reserved("--served-model-name=<v> is rejected", "--served-model-name=foo", true);
    failures += !expect_reserved("--host is rejected", "--host 0.0.0.0", true);
    failures += !expect_reserved("--max-model-len=<v> is rejected", "--max-model-len=8192", true);
    failures += !expect_reserved("reserved flag among allowed ones is rejected",
                                 "--gpu-memory-utilization 0.8 --port 1234", true);
    failures += !expect_reserved("non-reserved flags are allowed",
                                 "--gpu-memory-utilization 0.8 --enforce-eager", false);
    failures += !expect_reserved("empty args are allowed", "", false);

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
