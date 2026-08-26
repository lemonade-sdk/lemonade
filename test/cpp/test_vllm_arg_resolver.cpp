// Test for lemon::backends::resolve_vllm_args().
// Build with: cmake --build --preset default --target test_vllm_arg_resolver
// Run with: ctest --test-dir build -R vllm_arg_resolver --output-on-failure

#include "lemon/backends/vllm/vllm_arg_resolver.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

using lemon::backends::VLLMArgResolution;
using lemon::backends::resolve_vllm_args;
using lemon::backends::shared_memory_gpu_utilization;
using nlohmann::json;

static json test_config() {
    return json::parse(R"({
        "schema_version": 1,
        "enable_checkpoint_regex_match": true,
        "families": {
            "qwen3": {
                "match": [{"checkpoint_regex": "Qwen3\\."}],
                "args": "--enable-auto-tool-choice --tool-call-parser qwen3_coder --quantization awq --max-num-seqs 4"
            },
            "memory_family": {
                "match": [{"checkpoint_regex": "^Memory/"}],
                "args": "--gpu-memory-utilization 0.8 --tool-call-parser hermes"
            }
        },
        "models": {
            "Qwen3.5-4B-vLLM": {
                "family": "qwen3"
            },
            "Qwen3.5-2B-vLLM": {
                "family": "qwen3",
                "args": "--tool-call-parser hermes --quantization gptq"
            },
            "NoFamily-vLLM": {
                "disable_family_match": true,
                "args": "--tool-call-parser hermes"
            }
        }
    })");
}

static std::string join(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) out += " ";
        out += value;
    }
    return out;
}

static bool expect_args(const char* name,
                        const VLLMArgResolution& actual,
                        const std::string& expected,
                        bool expected_memory = false) {
    std::string actual_args = join(actual.args);
    bool ok = actual_args == expected && actual.has_memory_budget_arg == expected_memory;
    std::printf("[%s] %s\n  got:  %s memory=%s\n  want: %s memory=%s\n",
                ok ? "PASS" : "FAIL",
                name,
                actual_args.c_str(),
                actual.has_memory_budget_arg ? "true" : "false",
                expected.c_str(),
                expected_memory ? "true" : "false");
    return ok;
}

static bool expect_dtype_flag(const char* name,
                              const VLLMArgResolution& actual,
                              bool expected_dtype) {
    bool ok = actual.has_dtype_arg == expected_dtype;
    std::printf("[%s] %s\n  got:  has_dtype_arg=%s\n  want: has_dtype_arg=%s\n",
                ok ? "PASS" : "FAIL",
                name,
                actual.has_dtype_arg ? "true" : "false",
                expected_dtype ? "true" : "false");
    return ok;
}

static bool expect_quantization_arg(const char* name,
                                    const VLLMArgResolution& actual,
                                    bool expected_has_quantization,
                                    const std::string& expected_quantization) {
    bool ok = actual.has_quantization_arg == expected_has_quantization &&
              actual.quantization_arg == expected_quantization;
    std::printf("[%s] %s\n  got:  has_quantization_arg=%s quantization=%s\n"
                "  want: has_quantization_arg=%s quantization=%s\n",
                ok ? "PASS" : "FAIL",
                name,
                actual.has_quantization_arg ? "true" : "false",
                actual.quantization_arg.c_str(),
                expected_has_quantization ? "true" : "false",
                expected_quantization.c_str());
    return ok;
}

static bool expect_error(const char* name,
                         const std::string& arg_string,
                         const std::string& expected_substring) {
    try {
        (void)resolve_vllm_args("Qwen3.5-4B-vLLM", "Qwen/Qwen3.5-4B", test_config(), arg_string);
    } catch (const std::exception& e) {
        std::string message = e.what();
        bool ok = message.find(expected_substring) != std::string::npos;
        std::printf("[%s] %s\n  error: %s\n", ok ? "PASS" : "FAIL", name, message.c_str());
        return ok;
    }

    std::printf("[FAIL] %s\n  expected error containing: %s\n", name, expected_substring.c_str());
    return false;
}

static constexpr uint64_t gib = 1024ULL * 1024ULL * 1024ULL;

static uint64_t gib_of(double count) {
    return static_cast<uint64_t>(count * static_cast<double>(gib));
}

static bool expect_utilization(const char* name,
                               uint64_t free_bytes,
                               uint64_t total_bytes,
                               double expected) {
    double actual = shared_memory_gpu_utilization(free_bytes, total_bytes);
    // A negative expectation only asserts "omit the flag", not an exact value.
    bool ok = expected < 0.0 ? actual < 0.0 : std::fabs(actual - expected) < 1e-9;
    std::printf("[%s] %s\n  got:  %.4f\n  want: %.4f\n",
                ok ? "PASS" : "FAIL", name, actual, expected);
    return ok;
}

int main() {
    int failures = 0;

    failures += !expect_args(
        "checkpoint regex applies family args",
        resolve_vllm_args("Unlisted-vLLM", "Qwen/Qwen3.5-4B", test_config(), ""),
        "--enable-auto-tool-choice --tool-call-parser qwen3_coder --quantization awq --max-num-seqs 4");

    failures += !expect_args(
        "checkpoint regex applies across organizations",
        resolve_vllm_args("Unlisted-vLLM", "unsloth/Qwen3.5-4B-AWQ", test_config(), ""),
        "--enable-auto-tool-choice --tool-call-parser qwen3_coder --quantization awq --max-num-seqs 4");

    failures += !expect_args(
        "checkpoint regex does not overmatch Qwen30",
        resolve_vllm_args("Unlisted-vLLM", "Qwen/Qwen30-4B", test_config(), ""),
        "");

    json manual_only_config = test_config();
    manual_only_config["enable_checkpoint_regex_match"] = false;
    failures += !expect_args(
        "global regex disable ignores unlisted checkpoint match",
        resolve_vllm_args("Unlisted-vLLM", "Qwen/Qwen3.5-4B", manual_only_config, ""),
        "");

    failures += !expect_args(
        "global regex disable still allows exact model family",
        resolve_vllm_args("Qwen3.5-4B-vLLM", "Other/Checkpoint", manual_only_config, ""),
        "--enable-auto-tool-choice --tool-call-parser qwen3_coder --quantization awq --max-num-seqs 4");

    failures += !expect_args(
        "disable_family_match prevents regex family args",
        resolve_vllm_args("NoFamily-vLLM", "Qwen/Qwen3.5-4B", test_config(), ""),
        "--tool-call-parser hermes");

    failures += !expect_args(
        "exact model args override only conflicting family args",
        resolve_vllm_args("Qwen3.5-2B-vLLM", "Qwen/Qwen3.5-2B", test_config(), ""),
        "--enable-auto-tool-choice --max-num-seqs 4 --tool-call-parser hermes --quantization gptq");

    failures += !expect_args(
        "user tool parser overrides config without replacing other config args",
        resolve_vllm_args("Qwen3.5-4B-vLLM", "Qwen/Qwen3.5-4B", test_config(), "--tool-call-parser hermes"),
        "--enable-auto-tool-choice --quantization awq --max-num-seqs 4 --tool-call-parser hermes");

    failures += !expect_args(
        "user quantization overrides config",
        resolve_vllm_args("Qwen3.5-4B-vLLM", "Qwen/Qwen3.5-4B", test_config(), "--quantization gptq"),
        "--enable-auto-tool-choice --tool-call-parser qwen3_coder --max-num-seqs 4 --quantization gptq");

    failures += !expect_args(
        "user generic flag overrides same config flag",
        resolve_vllm_args("Qwen3.5-4B-vLLM", "Qwen/Qwen3.5-4B", test_config(), "--max-num-seqs 8"),
        "--enable-auto-tool-choice --tool-call-parser qwen3_coder --quantization awq --max-num-seqs 8");

    json repeatable_config = json::parse(R"({
        "schema_version": 1,
        "families": {
            "repeat_family": {
                "match": [{"checkpoint_regex": "^Repeat/"}],
                "args": "--repeatable-arg base --repeatable-arg extra --max-num-seqs 4"
            }
        },
        "models": {
            "Repeat-vLLM": {
                "family": "repeat_family",
                "args": "--repeatable-arg model"
            }
        }
    })");
    failures += !expect_args(
        "repeated generic flags are preserved across layers",
        resolve_vllm_args("Repeat-vLLM", "Repeat/Model", repeatable_config,
                          "--repeatable-arg user --max-num-seqs 8"),
        "--repeatable-arg base --repeatable-arg extra --repeatable-arg model "
        "--repeatable-arg user --max-num-seqs 8");

    json incoming_repeatable_config = json::parse(R"({
        "schema_version": 1,
        "families": {
            "single_family": {
                "match": [{"checkpoint_regex": "^Single/"}],
                "args": "--some-generic family"
            }
        }
    })");
    failures += !expect_args(
        "incoming repeated generic flags preserve prior layer value",
        resolve_vllm_args("Single-vLLM", "Single/Model", incoming_repeatable_config,
                          "--some-generic user-a --some-generic user-b"),
        "--some-generic family --some-generic user-a --some-generic user-b");

    json binary_config = json::parse(R"({
        "schema_version": 1,
        "families": {
            "binary_family": {
                "match": [{"checkpoint_regex": "^Binary/"}],
                "args": "--feature-enabled --no-old-feature --valued-flag family"
            }
        }
    })");
    failures += !expect_args(
        "incoming binary negation overrides prior positive flag",
        resolve_vllm_args("Binary-vLLM", "Binary/Model", binary_config, "--no-feature-enabled"),
        "--no-old-feature --valued-flag family --no-feature-enabled");

    failures += !expect_args(
        "incoming positive binary flag overrides prior negation",
        resolve_vllm_args("Binary-vLLM", "Binary/Model", binary_config, "--old-feature"),
        "--feature-enabled --valued-flag family --old-feature");

    failures += !expect_args(
        "binary negation does not remove valued flags",
        resolve_vllm_args("Binary-vLLM", "Binary/Model", binary_config, "--no-valued-flag"),
        "--feature-enabled --no-old-feature --valued-flag family --no-valued-flag");

    failures += !expect_args(
        "user can disable config auto tool choice",
        resolve_vllm_args("Qwen3.5-4B-vLLM", "Qwen/Qwen3.5-4B", test_config(), "--disable-auto-tool-choice"),
        "--tool-call-parser qwen3_coder --quantization awq --max-num-seqs 4 --disable-auto-tool-choice");

    failures += !expect_args(
        "user memory arg is detected",
        resolve_vllm_args("Qwen3.5-4B-vLLM", "Qwen/Qwen3.5-4B", test_config(), "--gpu-memory-utilization 0.9"),
        "--enable-auto-tool-choice --tool-call-parser qwen3_coder --quantization awq --max-num-seqs 4 --gpu-memory-utilization 0.9",
        true);

    failures += !expect_args(
        "config memory arg is detected",
        resolve_vllm_args("Memory-vLLM", "Memory/Model", test_config(), ""),
        "--gpu-memory-utilization 0.8 --tool-call-parser hermes",
        true);

    failures += !expect_error("user --port fails", "--port 12345", "--port");
    failures += !expect_error("user --model fails", "--model Qwen/Qwen3", "--model");
    failures += !expect_error("user --host fails", "--host 0.0.0.0", "--host");

    json invalid_config = test_config();
    invalid_config["models"]["BadConfig-vLLM"] = {{"args", "--port=12345"}};
    try {
        (void)resolve_vllm_args("BadConfig-vLLM", "Other/Model", invalid_config, "");
        std::printf("[FAIL] config --port fails\n  expected error\n");
        ++failures;
    } catch (const std::exception& e) {
        std::string message = e.what();
        bool ok = message.find("--port") != std::string::npos;
        std::printf("[%s] config --port fails\n  error: %s\n", ok ? "PASS" : "FAIL", message.c_str());
        failures += !ok;
    }

    failures += !expect_args(
        "--flag=value canonicalizes like --flag value",
        resolve_vllm_args("Qwen3.5-4B-vLLM", "Qwen/Qwen3.5-4B", test_config(), "--tool-call-parser=hermes"),
        "--enable-auto-tool-choice --quantization awq --max-num-seqs 4 --tool-call-parser hermes");

    failures += !expect_args(
        "negative value is captured with space syntax",
        resolve_vllm_args("Unlisted-vLLM", "Other/Model", test_config(), "--some-threshold -1"),
        "--some-threshold -1");

    failures += !expect_args(
        "negative value is captured with equals syntax",
        resolve_vllm_args("Unlisted-vLLM", "Other/Model", test_config(), "--some-threshold=-1"),
        "--some-threshold -1");

    failures += !expect_args(
        "negative decimal value is captured",
        resolve_vllm_args("Unlisted-vLLM", "Other/Model", test_config(), "--some-threshold -.5"),
        "--some-threshold -.5");

    failures += !expect_args(
        "short vLLM flags still parse as flags",
        resolve_vllm_args("Unlisted-vLLM", "Other/Model", test_config(), "-tp 2 --max-num-seqs 4"),
        "-tp 2 --max-num-seqs 4");

    // has_dtype_arg drives whether the backend forces --dtype float16 for AWQ.
    failures += !expect_dtype_flag(
        "no user --dtype leaves has_dtype_arg false",
        resolve_vllm_args("Unlisted-vLLM", "Other/Model", test_config(), ""),
        false);

    failures += !expect_dtype_flag(
        "user --dtype is detected",
        resolve_vllm_args("Unlisted-vLLM", "Other/Model", test_config(), "--dtype bfloat16"),
        true);

    failures += !expect_quantization_arg(
        "missing --quantization is reported",
        resolve_vllm_args("Unlisted-vLLM", "Other/Model", test_config(), ""),
        false,
        "");

    failures += !expect_quantization_arg(
        "config --quantization is reported",
        resolve_vllm_args("Qwen3.5-4B-vLLM", "Qwen/Qwen3.5-4B", test_config(), ""),
        true,
        "awq");

    failures += !expect_quantization_arg(
        "user --quantization override is reported",
        resolve_vllm_args("Qwen3.5-4B-vLLM", "Qwen/Qwen3.5-4B", test_config(), "--quantization gptq"),
        true,
        "gptq");

    // shared_memory_gpu_utilization scales vLLM's startup free-memory demand to what the
    // device itself has free, so a co-tenant process cannot reject a model that fits.

    // The gfx1151 CI case, verbatim from the failing run: 27.07 of 32.0 GiB free, which
    // vLLM's 0.92 default (29.44 GiB) rejects. 0.79 asks for 25.28 GiB, which fits.
    failures += !expect_utilization(
        "the observed CI reading yields a request that fits",
        gib_of(27.07), gib_of(32.0), 0.79);

    // Same reading: 0.7959 truncates to 0.79 rather than rounding to 0.80, because the
    // flag is emitted at two decimals and rounding up would spend the headroom.
    failures += !expect_utilization(
        "the emitted value truncates rather than rounding past what is free",
        gib_of(27.07), gib_of(32.0), 0.79);

    failures += !expect_utilization(
        "an idle device leaves vLLM's own default in place",
        gib_of(31.5), gib_of(32.0), -1.0);

    // vLLM's pre-flight passes at exactly 0.92 free, so there is nothing to correct.
    failures += !expect_utilization(
        "a device vLLM's default exactly fits keeps that default",
        gib_of(92.0), gib_of(100.0), -1.0);

    failures += !expect_utilization(
        "a device just below the default is scaled down",
        gib_of(58.0), gib_of(64.0), 0.85);

    failures += !expect_utilization(
        "no usable reading leaves vLLM's own default in place", 0, 0, -1.0);

    failures += !expect_utilization(
        "a nonsensical reading leaves vLLM's own default in place",
        gib_of(48.0), gib_of(32.0), -1.0);

    failures += !expect_utilization(
        "an exhausted device leaves vLLM's own default in place",
        0, gib_of(32.0), -1.0);

    // 4 GiB free of 32 GiB scales to 0.07, a 2.24 GiB budget that cannot hold the 4 GiB
    // kv-cache cap emitted beside it. vLLM's own error names the byte counts instead.
    failures += !expect_utilization(
        "a budget too small for the kv-cache cap leaves vLLM's own default in place",
        gib_of(4.0), gib_of(32.0), -1.0);

    // The same 12.5% free share of a large pool is 17.92 GiB, which clears the cap.
    failures += !expect_utilization(
        "the same free share on a large pool is still worth requesting",
        gib_of(32.0), gib_of(256.0), 0.07);

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
