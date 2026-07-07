#include <iostream>
#include <string>

#include "lemon/system_info.h"

using lemon::SystemInfo;

namespace {

int failures = 0;

void expect(bool condition, const std::string& label) {
    if (condition) {
        std::cout << "PASS: " << label << std::endl;
    } else {
        std::cout << "FAIL: " << label << std::endl;
        ++failures;
    }
}

}  // namespace

int main() {
    expect(SystemInfo::backend_supports_arch("llamacpp", "rocm", "gfx942"),
           "llamacpp:rocm supports gfx942 (MI300X)");

    // Channel-qualified backend names ("rocm-stable"/"rocm-nightly") normalize to
    // "rocm" before the support-matrix lookup — the path install/dry-run resolution
    // actually takes. Assert the suffix-stripping branch, not just the pre-normalized name.
    expect(SystemInfo::backend_supports_arch("llamacpp", "rocm-stable", "gfx942"),
           "llamacpp:rocm-stable normalizes to rocm and supports gfx942");
    expect(SystemInfo::backend_supports_arch("llamacpp", "rocm-nightly", "gfx942"),
           "llamacpp:rocm-nightly normalizes to rocm and supports gfx942");

    expect(SystemInfo::backend_supports_arch("llamacpp", "rocm", "gfx1100"),
           "llamacpp:rocm still supports gfx1100 via gfx110X wildcard");
    expect(SystemInfo::backend_supports_arch("llamacpp", "rocm", "gfx1151"),
           "llamacpp:rocm still supports gfx1151 (Strix Halo)");
    expect(SystemInfo::backend_supports_arch("llamacpp", "rocm", "gfx1036"),
           "llamacpp:rocm still supports gfx1036 via gfx103X wildcard (RDNA2)");

    expect(!SystemInfo::backend_supports_arch("llamacpp", "rocm", "gfx906"),
           "llamacpp:rocm does not support gfx906 (not in the shipped multi-arch build's targets we validate)");
    expect(!SystemInfo::backend_supports_arch("llamacpp", "rocm", "gfx90a"),
           "llamacpp:rocm does not add gfx90a here (tracked separately, e.g. upstream #2092)");
    expect(!SystemInfo::backend_supports_arch("llamacpp", "rocm", "gfx908"),
           "llamacpp:rocm does not add gfx908 (CDNA1, in the build's targets but tracked separately from gfx942)");

    if (failures != 0) {
        std::cout << failures << " assertion(s) failed" << std::endl;
        return 1;
    }
    std::cout << "All llamacpp CDNA gating assertions passed" << std::endl;
    return 0;
}
