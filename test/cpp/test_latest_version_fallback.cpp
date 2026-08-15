// Standalone unit tests for lemon::resolve_latest_pin() — the pure policy that
// decides how a "latest" *_bin config pin resolves to a concrete release tag
// when the live GitHub lookup succeeds, fails (e.g. HTTP 504), or is skipped
// (offline). Regression coverage for lemonade-sdk/lemonade#2265: a transient
// GitHub failure must fall back to the installed binary instead of refusing to
// load a model that is otherwise ready.
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release build the CI `default` preset uses, where
// -DNDEBUG would compile assert() to a no-op.
//
// Compile with:
//   g++ -std=c++17 -I src/cpp/include \
//       test/cpp/test_latest_version_fallback.cpp -o latest_version_fallback_test
//
// Run with:
//   ./latest_version_fallback_test

#include <cstdio>
#include <string>

#include <lemon/backend_version_policy.h>

using lemon::LatestPinResolution;
using lemon::resolve_latest_pin;

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
    const std::string recipe = "llamacpp";
    const std::string backend = "vulkan";

    printf("=== resolve_latest_pin() Unit Tests ===\n\n");

    // Online, GitHub lookup succeeds → use the freshly-fetched tag (no fallback).
    {
        LatestPinResolution res = resolve_latest_pin(recipe, backend, /*offline=*/false,
                                                     /*fetched_latest=*/"b9700",
                                                     /*installed_version=*/"b9632");
        r.check(res.version == "b9700", "online ok: uses fetched tag");
        r.check(res.error.empty(), "online ok: no error");
        r.check(!res.used_installed_fallback, "online ok: not a fallback");
    }

    // Online, GitHub lookup FAILS, a binary is installed → fall back to the
    // installed version (the #2265 fix). Must not produce an error.
    {
        LatestPinResolution res = resolve_latest_pin(recipe, backend, /*offline=*/false,
                                                     /*fetched_latest=*/"",
                                                     /*installed_version=*/"b9632");
        r.check(res.version == "b9632", "online failed + installed: falls back to installed (#2265)");
        r.check(res.error.empty(), "online failed + installed: no error");
        r.check(res.used_installed_fallback, "online failed + installed: flagged as fallback");
    }

    // Online, GitHub lookup FAILS, nothing installed → unresolvable, with a
    // message that names the GitHub failure (not offline).
    {
        LatestPinResolution res = resolve_latest_pin(recipe, backend, /*offline=*/false,
                                                     /*fetched_latest=*/"",
                                                     /*installed_version=*/"");
        r.check(res.version.empty(), "online failed + none: no version");
        r.check(res.error.find("GitHub lookup failed") != std::string::npos,
                "online failed + none: error names GitHub failure");
        r.check(res.error.find("llamacpp:vulkan") != std::string::npos,
                "online failed + none: error names recipe:backend");
        r.check(!res.used_installed_fallback, "online failed + none: not a fallback");
    }

    // Offline, a binary is installed → reuse it (the live result is ignored even
    // if somehow non-empty).
    {
        LatestPinResolution res = resolve_latest_pin(recipe, backend, /*offline=*/true,
                                                     /*fetched_latest=*/"b9700",
                                                     /*installed_version=*/"b9632");
        r.check(res.version == "b9632", "offline + installed: reuses installed, ignores fetched");
        r.check(res.error.empty(), "offline + installed: no error");
        r.check(res.used_installed_fallback, "offline + installed: flagged as fallback");
    }

    // Offline, nothing installed → unresolvable, with the offline-specific
    // message.
    {
        LatestPinResolution res = resolve_latest_pin(recipe, backend, /*offline=*/true,
                                                     /*fetched_latest=*/"",
                                                     /*installed_version=*/"");
        r.check(res.version.empty(), "offline + none: no version");
        r.check(res.error.find("offline mode") != std::string::npos,
                "offline + none: error names offline mode");
        r.check(!res.used_installed_fallback, "offline + none: not a fallback");
    }

    printf("\n%d/%d checks passed\n", r.passed, r.passed + r.failed);
    return r.failed == 0 ? 0 : 1;
}
