// Standalone unit tests for the pure musl backend asset resolvers
// (lemon::backends::musl::*). A musl build of lemond must never resolve a glibc
// ("ubuntu"/"Ubuntu") release asset, and the GPU backends that only ship glibc
// binaries (llamacpp cuda/rocm, whispercpp rocm) have no musl build at all — the
// resolver must throw rather than fall through to a glibc name. Regression
// coverage for the musl backend-selection gating.
//
// The resolvers are pure and dependency-free, so these assertions run on any
// host regardless of libc (they take the target arch as a parameter).
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release build the CI `default` preset uses, where -DNDEBUG
// would compile assert() to a no-op.
//
// Compile with:
//   g++ -std=c++17 -I src/cpp/include \
//       test/cpp/test_musl_install_params.cpp -o musl_install_params_test
//
// Run with:
//   ./musl_install_params_test

#include <cstdio>
#include <functional>
#include <string>

#include <lemon/backends/musl_assets.h>

namespace musl = lemon::backends::musl;

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

static bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

// A musl asset name must name musl and must never carry a glibc or foreign-OS
// token. This is the core invariant the whole gating change exists to guarantee.
static void check_is_musl_asset(TestResult& r, const std::string& label,
                                const musl::Asset& asset) {
    r.check(contains(asset.filename, "musl"), label + ": names musl");
    r.check(!contains(asset.filename, "ubuntu"), label + ": no lowercase ubuntu");
    r.check(!contains(asset.filename, "Ubuntu"), label + ": no capital Ubuntu");
    r.check(!contains(asset.filename, "windows"), label + ": no windows");
    r.check(!contains(asset.filename, "darwin"), label + ": no darwin");
    r.check(!contains(asset.filename, "macos"), label + ": no macos");
    r.check(asset.repo.rfind("lemonade-sdk/", 0) == 0, label + ": lemonade-sdk repo");
}

static void check_throws(TestResult& r, const std::string& label,
                         const std::function<void()>& fn) {
    bool threw = false;
    try {
        fn();
    } catch (const std::exception&) {
        threw = true;
    }
    r.check(threw, label + ": no musl build -> throws");
}

int main() {
    TestResult r;
    const std::string v = "b9700";       // llama/whisper/moonshine version form
    const std::string sd_v = "master-1f9ee88";  // sd short_version form

    printf("=== musl install-param resolver Unit Tests ===\n\n");

    for (musl::Arch arch : {musl::Arch::X86_64, musl::Arch::Aarch64}) {
        const std::string a = (arch == musl::Arch::Aarch64) ? "[aarch64]" : "[x86_64]";

        // llama.cpp: only cpu and vulkan have musl builds; cuda/rocm must throw.
        check_is_musl_asset(r, a + " llamacpp cpu", musl::llamacpp("cpu", v, arch));
        check_is_musl_asset(r, a + " llamacpp vulkan", musl::llamacpp("vulkan", v, arch));
        check_throws(r, a + " llamacpp cuda", [&] { musl::llamacpp("cuda", v, arch); });
        check_throws(r, a + " llamacpp rocm-stable", [&] { musl::llamacpp("rocm-stable", v, arch); });
        check_throws(r, a + " llamacpp rocm-nightly", [&] { musl::llamacpp("rocm-nightly", v, arch); });

        // whisper.cpp: only cpu and vulkan; rocm/npu must throw.
        check_is_musl_asset(r, a + " whispercpp cpu", musl::whispercpp("cpu", v, arch));
        check_is_musl_asset(r, a + " whispercpp vulkan", musl::whispercpp("vulkan", v, arch));
        check_throws(r, a + " whispercpp rocm", [&] { musl::whispercpp("rocm", v, arch); });
        check_throws(r, a + " whispercpp npu", [&] { musl::whispercpp("npu", v, arch); });

        // sd.cpp remaps every backend to a musl CPU/Vulkan asset (never throws,
        // never glibc): an explicit cuda/rocm request must not leak an ubuntu name.
        check_is_musl_asset(r, a + " sdcpp cpu", musl::sdcpp("cpu", sd_v, arch));
        check_is_musl_asset(r, a + " sdcpp vulkan", musl::sdcpp("vulkan", sd_v, arch));
        check_is_musl_asset(r, a + " sdcpp rocm", musl::sdcpp("rocm", sd_v, arch));
        check_is_musl_asset(r, a + " sdcpp cuda", musl::sdcpp("cuda", sd_v, arch));

        // kokoro: only cpu; metal must throw.
        check_is_musl_asset(r, a + " kokoro cpu", musl::kokoro("cpu", arch));
        check_throws(r, a + " kokoro metal", [&] { musl::kokoro("metal", arch); });

        // moonshine: cpu-only bundle, always a musl asset.
        check_is_musl_asset(r, a + " moonshine", musl::moonshine(v, arch));
    }

    // Spot-check the exact asset shapes so a rename on either side (fork CI vs.
    // this resolver) is caught, not just the "contains musl" invariant.
    r.check(musl::llamacpp("vulkan", v, musl::Arch::X86_64).filename ==
                "llama-b9700-bin-linux-musl-vulkan-x64.tar.gz",
            "llamacpp vulkan x86_64 exact name");
    r.check(musl::whispercpp("cpu", v, musl::Arch::Aarch64).filename ==
                "whisper-b9700-linux-musl-cpu-aarch64.tar.gz",
            "whispercpp cpu aarch64 exact name");
    r.check(musl::sdcpp("vulkan", sd_v, musl::Arch::X86_64).filename ==
                "sd-master-1f9ee88-bin-Linux-musl-vulkan-x86_64.zip",
            "sdcpp vulkan x86_64 exact name");
    r.check(musl::kokoro("cpu", musl::Arch::Aarch64).filename ==
                "kokoros-linux-musl-arm64.tar.gz",
            "kokoro cpu aarch64 exact name");
    r.check(musl::moonshine(v, musl::Arch::X86_64).filename ==
                "moonshine-server-b9700-linux-musl-x64.tar.gz",
            "moonshine x86_64 exact name");

    printf("\n=== %d passed, %d failed ===\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
