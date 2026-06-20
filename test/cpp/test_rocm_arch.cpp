// Standalone test for lemon::normalize_rocm_family (src/cpp/include/lemon/rocm_arch.h).
//
// Guards the regression in #2319: identify_rocm_arch_from_name() must return the
// ROCm *family* download target (gfx103X / gfx110X / gfx120X) for RDNA2/3/4 dGPUs,
// not the specific arch (gfx1030 / gfx1100 / gfx1201). Commit 2a7aa18c (#2093)
// removed ROCM_ARCH_MAPPING, so the gfx-regex and KFD numeric-ISA detection paths
// began returning the specific arch, which no longer matched the support set and
// made ROCm report "Unsupported GPU: gfx1100" for e.g. an RX 7900 XT. This header
// restores the specific->family normalization those paths apply.
//
// Compile with: cl /std:c++17 /EHsc /I src/cpp/include test/cpp/test_rocm_arch.cpp
// or:          g++ -std=c++17 -I src/cpp/include test/cpp/test_rocm_arch.cpp -o rocm_arch_test

#include "lemon/rocm_arch.h"

#include <cstdio>
#include <string>

using lemon::normalize_rocm_family;

static int g_failures = 0;

static void expect(const char* name, const std::string& got, const std::string& want) {
    bool ok = (got == want);
    if (!ok) ++g_failures;
    std::printf("[%s] %s (got \"%s\", want \"%s\")\n",
                ok ? "PASS" : "FAIL", name, got.c_str(), want.c_str());
}

int main() {
    std::printf("=== normalize_rocm_family tests ===\n");

    // RDNA3 (gfx110X) — the #2319 reporter's family + the ai3 RX 7900 XT repro.
    expect("gfx1100 -> gfx110X", normalize_rocm_family("gfx1100"), "gfx110X");
    expect("gfx1101 -> gfx110X", normalize_rocm_family("gfx1101"), "gfx110X");
    expect("gfx1102 -> gfx110X", normalize_rocm_family("gfx1102"), "gfx110X");
    expect("gfx1103 -> gfx110X", normalize_rocm_family("gfx1103"), "gfx110X");

    // RDNA2 (gfx103X) — the full gfx1030-gfx1036 range, all mapped to the
    // gfx103X-all archive in backend_versions.json (#2319 review: gfx1033/1035/1036
    // must NOT be dropped, a published bundle covers them).
    expect("gfx1030 -> gfx103X", normalize_rocm_family("gfx1030"), "gfx103X");
    expect("gfx1031 -> gfx103X", normalize_rocm_family("gfx1031"), "gfx103X");
    expect("gfx1032 -> gfx103X", normalize_rocm_family("gfx1032"), "gfx103X");
    expect("gfx1033 -> gfx103X", normalize_rocm_family("gfx1033"), "gfx103X");
    expect("gfx1034 -> gfx103X", normalize_rocm_family("gfx1034"), "gfx103X");
    expect("gfx1035 -> gfx103X", normalize_rocm_family("gfx1035"), "gfx103X");
    expect("gfx1036 -> gfx103X", normalize_rocm_family("gfx1036"), "gfx103X");

    // RDNA4 (gfx120X).
    expect("gfx1200 -> gfx120X", normalize_rocm_family("gfx1200"), "gfx120X");
    expect("gfx1201 -> gfx120X", normalize_rocm_family("gfx1201"), "gfx120X");

    // iGPU exact targets pass through unchanged (ship as exact binaries).
    expect("gfx1150 unchanged", normalize_rocm_family("gfx1150"), "gfx1150");
    expect("gfx1151 unchanged", normalize_rocm_family("gfx1151"), "gfx1151");
    expect("gfx1152 unchanged", normalize_rocm_family("gfx1152"), "gfx1152");

    // Idempotent on an already-collapsed family (the name-heuristic paths already
    // return families) and pass-through for unrelated / empty input.
    expect("gfx110X idempotent", normalize_rocm_family("gfx110X"), "gfx110X");
    expect("gfx90a unchanged", normalize_rocm_family("gfx90a"), "gfx90a");
    expect("empty unchanged", normalize_rocm_family(""), "");

    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
