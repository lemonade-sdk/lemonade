// Standalone unit tests for lemon::backends::sdcpp::sdcpp_rocm_asset_pattern() —
// the pure decision of which ROCm runtime token goes into the sd.cpp release
// asset name. sd.cpp publishes per-runtime builds (e.g. -rocm-7.13.0-x64.zip
// alongside -rocm-7.14.0-x64.zip), but the runtime loaded at launch is the
// TheRock pin from backend_versions.json, so a pinned install must name that
// exact runtime. Only an explicit "latest" bin pin wildcards it, letting the
// installer resolve whatever runtime upstream's newest release was built with
// (lemonade-sdk/lemonade#2988).
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release build the CI `default` preset uses, where
// -DNDEBUG would compile assert() to a no-op.
//
// Compile with:
//   g++ -std=c++17 -I src/cpp/include \
//       test/cpp/test_sdcpp_rocm_asset.cpp -o sdcpp_rocm_asset_test
//
// Run with:
//   ./sdcpp_rocm_asset_test

#include <cstdio>
#include <stdexcept>
#include <string>

#include <lemon/backends/sdcpp/sdcpp_asset.h>

using lemon::backends::sdcpp::sdcpp_rocm_asset_pattern;

struct TestResult {
    int passed = 0;
    int failed = 0;

    void check(bool cond, const std::string& name) {
        if (cond) {
            printf("[PASS] %s\n", name.c_str());
            ++passed;
        } else {
            printf("[FAIL] %s\n", name.c_str());
            ++failed;
        }
    }
};

int main() {
    TestResult r;

    printf("=== sdcpp_rocm_asset_pattern() Unit Tests ===\n\n");

#if defined(_WIN32) || defined(__linux__)
    // Pinned install: exact TheRock runtime in the asset name, no wildcard.
    const std::string pinned = sdcpp_rocm_asset_pattern("master-8caa3f9", false, "7.13.0");
    // "latest" bin pin: runtime wildcarded for install-time resolution.
    const std::string latest = sdcpp_rocm_asset_pattern("master-bfbef5b", true, "");

    r.check(pinned.find("*") == std::string::npos,
            "pinned: asset name has no wildcard");
    r.check(latest.find("-rocm-*") != std::string::npos,
            "latest: runtime component is wildcarded");

#ifdef _WIN32
    r.check(pinned == "sd-master-8caa3f9-bin-win-rocm-7.13.0-x64.zip",
            "pinned: exact Windows asset name");
    r.check(latest == "sd-master-bfbef5b-bin-win-rocm-*-x64.zip",
            "latest: wildcard Windows asset name");
#else
    r.check(pinned == "sd-master-8caa3f9-bin-Linux-Ubuntu-24.04-x86_64-rocm-7.13.0.zip",
            "pinned: exact Linux asset name");
    r.check(latest == "sd-master-bfbef5b-bin-Linux-Ubuntu-24.04-x86_64-rocm-*.zip",
            "latest: wildcard Linux asset name");
#endif
#else
    bool threw = false;
    try {
        sdcpp_rocm_asset_pattern("master-8caa3f9", false, "7.13.0");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    r.check(threw, "unsupported platform throws");
#endif

    printf("\n%d passed, %d failed\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
