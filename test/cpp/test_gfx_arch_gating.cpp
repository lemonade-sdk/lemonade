// Unit tests for lemon::backends::BackendUtils::is_concrete_gfx_arch().
//
// This predicate gates the ROCm wheel install: a concrete gfx target maps to a
// rocm-sdk-device wheel, while a family placeholder (gfx110X) has none and must
// fall back to the tarball. Getting it wrong either skips the wheel path for a
// supported GPU or attempts a wheel install that can't resolve a device package.

#include <iostream>
#include <string>

#include <lemon/backends/backend_utils.h>

using lemon::backends::BackendUtils;

namespace {

int g_failures = 0;

void expect(const std::string& arch, bool want) {
    const bool got = BackendUtils::is_concrete_gfx_arch(arch);
    if (got == want) {
        std::cout << "[ok] " << arch << " -> " << (got ? "concrete" : "family/invalid") << std::endl;
    } else {
        std::cerr << "[FAIL] " << arch << " -> " << got << " (wanted " << want << ")" << std::endl;
        ++g_failures;
    }
}

}  // namespace

int main() {
    // Concrete targets: all hex after the gfx prefix.
    expect("gfx1151", true);
    expect("gfx90a", true);   // trailing hex letter
    expect("gfx908", true);
    expect("gfx942", true);
    expect("gfx1100", true);

    // Family placeholders: the non-hex 'X' rules them out.
    expect("gfx110X", false);
    expect("gfx103X", false);
    expect("gfx120X", false);

    // Malformed / non-gfx.
    expect("gfx", false);
    expect("gf", false);
    expect("", false);
    expect("radeon", false);
    expect("gfxzzzz", false);  // non-hex letters

    if (g_failures > 0) {
        std::cerr << "Total failures: " << g_failures << std::endl;
        return 1;
    }
    std::cout << "All gfx arch gating unit tests passed!" << std::endl;
    return 0;
}
