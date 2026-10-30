#include "lemon/backends/fastflowlm/flm_arg_resolver.h"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

using lemon::backends::FLMArgResolution;
using lemon::backends::resolve_flm_args;

namespace {

std::string join(const std::vector<std::string>& values) {
    std::string output;
    for (const auto& value : values) {
        if (!output.empty()) {
            output += " ";
        }
        output += value;
    }
    return output;
}

bool expect_args(const char* name, const std::string& input, int ctx_size,
                 const std::string& expected) {
    try {
        FLMArgResolution resolution = resolve_flm_args(input, ctx_size);
        std::string actual = join(resolution.args);
        bool ok = actual == expected;
        std::printf("[%s] %s\n  got:  %s\n  want: %s\n",
                    ok ? "PASS" : "FAIL", name, actual.c_str(), expected.c_str());
        return ok;
    } catch (const std::exception& e) {
        std::printf("[FAIL] %s\n  unexpected error: %s\n", name, e.what());
        return false;
    }
}

bool expect_error(const char* name, const std::string& input, int ctx_size,
                  const std::string& expected_substring) {
    try {
        (void)resolve_flm_args(input, ctx_size);
    } catch (const std::exception& e) {
        std::string message = e.what();
        bool ok = message.find(expected_substring) != std::string::npos;
        std::printf("[%s] %s\n  error: %s\n",
                    ok ? "PASS" : "FAIL", name, message.c_str());
        return ok;
    }

    std::printf("[FAIL] %s\n  expected error containing: %s\n",
                name, expected_substring.c_str());
    return false;
}

} // namespace

int main() {
    int failures = 0;

    failures += !expect_args("empty args", "", 8192, "");
    failures += !expect_args(
        "all safe args",
        "--pmode balanced --prefill-chunk-len 4096 --img-pre-resize 3 "
        "--socket 20 --q-len 15 --preemption true",
        8192,
        "--pmode balanced --prefill-chunk-len 4096 --img-pre-resize 3 "
        "--socket 20 --q-len 15 --preemption 1");
    failures += !expect_args(
        "canonicalizes numeric and boolean values",
        "--socket 0010 --preemption false",
        8192,
        "--socket 10 --preemption 0");
    failures += !expect_args(
        "prefill auto value",
        "--prefill-chunk-len -1",
        8192,
        "--prefill-chunk-len -1");

    failures += !expect_error("host is blocked", "--host 0.0.0.0", 8192, "not allowed");
    failures += !expect_error("port is blocked", "--port 8000", 8192, "not allowed");
    failures += !expect_error("context override is blocked", "--ctx-len 16384", 8192, "not allowed");
    failures += !expect_error("model mode is blocked", "--asr 1", 8192, "not allowed");
    failures += !expect_error("embedding mode is blocked", "--embed 1", 8192, "not allowed");
    failures += !expect_error("quiet is blocked", "--quiet 1", 8192, "not allowed");
    failures += !expect_error("file input is blocked", "--prompt /etc/passwd", 8192, "not allowed");
    failures += !expect_error("positional command is blocked", "serve model", 8192, "canonical long flags");
    failures += !expect_error("short aliases are blocked", "-q 20", 8192, "canonical long flags");
    failures += !expect_error("equals syntax is blocked", "--q-len=20", 8192, "--flag=value");
    failures += !expect_error("unknown flag is blocked", "--future-flag 1", 8192, "not allowed");
    failures += !expect_error("duplicate flag is blocked", "--q-len 10 --q-len 20", 8192, "duplicate");
    failures += !expect_error("missing value is blocked", "--q-len", 8192, "requires a value");
    failures += !expect_error("quoted values are blocked", "--pmode \"balanced\"", 8192, "unquoted");
    failures += !expect_error("control characters are blocked", "--pmode balanced\n", 8192, "unquoted");
    failures += !expect_error("invalid power mode is blocked", "--pmode maximum", 8192, "must be one of");
    failures += !expect_error("zero prefill is blocked", "--prefill-chunk-len 0", 8192, "positive integer");
    failures += !expect_error("prefill over context is blocked", "--prefill-chunk-len 8193", 8192, "ctx_size");
    failures += !expect_error("negative resize is blocked", "--img-pre-resize -1", 8192, "between 0 and 4");
    failures += !expect_error("large resize is blocked", "--img-pre-resize 5", 8192, "between 0 and 4");
    failures += !expect_error("zero sockets are blocked", "--socket 0", 8192, "between 1 and 1024");
    failures += !expect_error("large queue is blocked", "--q-len 1025", 8192, "between 1 and 1024");
    failures += !expect_error("non-integer queue is blocked", "--q-len ten", 8192, "integer value");
    failures += !expect_error("invalid boolean is blocked", "--preemption yes", 8192, "0, 1, false, true");
    failures += !expect_error(
        "too many arguments are blocked",
        "--pmode balanced --prefill-chunk-len 1 --img-pre-resize 1 --socket 1 "
        "--q-len 1 --preemption 1 --pmode turbo",
        8192,
        "too many arguments");

    if (failures != 0) {
        std::printf("%d FLM argument resolver test(s) failed\n", failures);
        return 1;
    }

    std::printf("All FLM argument resolver tests passed\n");
    return 0;
}
