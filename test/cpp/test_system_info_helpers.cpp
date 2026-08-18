// C++ coverage for the pure SystemInfo CUDA mapping and device-family matching
// helpers. These helpers are used directly by system_info.cpp, so the test covers
// the production implementation instead of a Python replica.
//
// Build: cmake --build --preset default --target test_system_info_helpers
// Run:   ctest --test-dir build -R '^SystemInfoHelpersTest$' --output-on-failure

#include "system_info_utils.h"

#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

using lemon::system_info_detail::compute_cap_to_sm;
using lemon::system_info_detail::cuda_supported_archs;
using lemon::system_info_detail::device_matches_constraint;
using lemon::system_info_detail::identify_cuda_arch_from_name;

static bool expect_string(const char* name,
                          const std::string& actual,
                          const std::string& expected) {
    const bool ok = actual == expected;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        std::printf("  got:  '%s'\n  want: '%s'\n",
                    actual.c_str(), expected.c_str());
    }
    return ok;
}

static bool expect_bool(const char* name, bool actual, bool expected) {
    const bool ok = actual == expected;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        std::printf("  got:  %s\n  want: %s\n",
                    actual ? "true" : "false",
                    expected ? "true" : "false");
    }
    return ok;
}

static bool expect_membership(const char* name,
                              const std::string& arch,
                              bool expected) {
    const bool actual = cuda_supported_archs().count(arch) > 0;
    return expect_bool(name, actual, expected);
}

int main() {
    int failures = 0;

    const std::vector<std::pair<std::string, std::string>> compute_cap_cases = {
        {"7.5", "sm_75"},
        {"8.0", "sm_80"},
        {"8.6", "sm_86"},
        {"8.9", "sm_89"},
        {"9.0", "sm_90"},
        {"10.0", "sm_100"},
        {"12.0", "sm_120"},
        {"12.1", "sm_121"},
        {"6.1", "sm_61"},
        {"5.0", "sm_50"},
    };
    for (const auto& [input, expected] : compute_cap_cases) {
        const std::string name = "compute_cap " + input;
        failures += !expect_string(name.c_str(), compute_cap_to_sm(input), expected);
    }

    for (const char* input : {"", "86", "8", ".", "abc", "8.x"}) {
        const std::string name = std::string("invalid compute_cap ") + input;
        failures += !expect_string(name.c_str(), compute_cap_to_sm(input), "");
    }

    for (const char* cap : {"7.5", "8.0", "8.6", "8.9", "9.0", "10.0", "12.0", "12.1"}) {
        const std::string arch = compute_cap_to_sm(cap);
        const std::string name = "supported CUDA arch " + arch;
        failures += !expect_membership(name.c_str(), arch, true);
    }
    for (const char* cap : {"6.1", "7.0", "5.0"}) {
        const std::string arch = compute_cap_to_sm(cap);
        const std::string name = "unsupported CUDA arch " + arch;
        failures += !expect_membership(name.c_str(), arch, false);
    }

    const std::vector<std::pair<std::string, std::string>> gpu_name_cases = {
        {"NVIDIA GeForce RTX 3080", "sm_86"},
        {"NVIDIA GeForce RTX 4090", "sm_89"},
        {"NVIDIA GeForce RTX 2080 Ti", "sm_75"},
        {"NVIDIA GeForce RTX 5090", "sm_120"},
        {"NVIDIA GeForce RTX 5080", "sm_120"},
        {"NVIDIA GTX 1660 Super", "sm_75"},
        {"NVIDIA H100 PCIe", "sm_90"},
        {"NVIDIA A100-SXM4-80GB", "sm_80"},
        {"NVIDIA A10", "sm_86"},
        {"NVIDIA A40", "sm_86"},
        {"NVIDIA GB10", "sm_121"},
        {"GB10", "sm_121"},
        {"NVIDIA RTX PRO 3000 Blackwell Generation Laptop GPU", "sm_120"},
        {"NVIDIA RTX PRO 6000 Blackwell", "sm_120"},
        {"NVIDIA RTX PRO 4000 Blackwell", "sm_120"},
        {"NVIDIA RTX PRO 1000", "sm_120"},
        {"NVIDIA B200", "sm_100"},
        {"NVIDIA B200 (Blackwell)", "sm_100"},
        {"NVIDIA B100", "sm_100"},
        {"NVIDIA B100 Blackwell", "sm_100"},
        {"NVIDIA B200 Blackwell", "sm_100"},
    };
    for (const auto& [input, expected] : gpu_name_cases) {
        const std::string name = "GPU name " + input;
        failures += !expect_string(
            name.c_str(), identify_cuda_arch_from_name(input), expected);
    }

    for (const char* input : {
             "AMD Radeon RX 7900 XTX", "Intel Arc A770", "Apple M3", "",
             "NVIDIA GTX 1080 Ti", "NVIDIA GTX 980", "NVIDIA Tesla K80"}) {
        const std::string name = std::string("unsupported GPU name ") + input;
        failures += !expect_string(
            name.c_str(), identify_cuda_arch_from_name(input), "");
    }

    failures += !expect_bool(
        "gfx1103 matches gfx110X",
        device_matches_constraint("gfx1103", {"gfx110X"}), true);
    failures += !expect_bool(
        "gfx1201 matches gfx120X",
        device_matches_constraint("gfx1201", {"gfx120X"}), true);
    failures += !expect_bool(
        "gfx1151 exact match",
        device_matches_constraint("gfx1151", {"gfx1151"}), true);
    failures += !expect_bool(
        "gfx1152 exact match",
        device_matches_constraint("gfx1152", {"gfx1152"}), true);
    failures += !expect_bool(
        "gfx1151 does not match gfx110X",
        device_matches_constraint("gfx1151", {"gfx110X"}), false);
    failures += !expect_bool(
        "empty allowed family matches everything",
        device_matches_constraint("gfx1151", {}), true);
    failures += !expect_bool(
        "matches one of multiple wildcard families",
        device_matches_constraint("gfx1103", {"gfx103X", "gfx110X"}), true);
    failures += !expect_bool(
        "matches second wildcard family",
        device_matches_constraint("gfx1201", {"gfx110X", "gfx120X"}), true);
    failures += !expect_bool(
        "does not match any wildcard family",
        device_matches_constraint("gfx1151", {"gfx103X", "gfx110X"}), false);

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
