// Verifies the per-arch install gate on the llamacpp:rocm support row: gfx950
// (MI350) only has a Linux + stable-channel asset published so far, so it must
// not be advertised installable on Windows or on the nightly channel, while the
// established RDNA families and gfx942 keep their full reach.

#include <cstdlib>
#include <iostream>
#include <string>

#include "lemon/system_info.h"

using lemon::SystemInfo;

namespace {

int failures = 0;

void expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << std::endl;
        ++failures;
    } else {
        std::cout << "ok: " << msg << std::endl;
    }
}

}  // namespace

int main() {
#if defined(_WIN32)
    const bool on_linux = false;
#elif defined(__linux__)
    const bool on_linux = true;
#else
    const bool on_linux = false;
#endif

    // RDNA families are unchanged: installable on every OS and channel.
    expect(SystemInfo::backend_supports_arch("llamacpp", "rocm-stable", "gfx1151"),
           "llamacpp gfx1151 stable stays installable");
    expect(SystemInfo::backend_supports_arch("llamacpp", "rocm-nightly", "gfx1151"),
           "llamacpp gfx1151 nightly stays installable");

    // gfx950 (MI350) is gated to Linux + stable until the other assets ship.
    expect(SystemInfo::backend_supports_arch("llamacpp", "rocm-stable", "gfx950") == on_linux,
           "llamacpp gfx950 stable is installable on Linux only");
    expect(!SystemInfo::backend_supports_arch("llamacpp", "rocm-nightly", "gfx950"),
           "llamacpp gfx950 nightly is never advertised (asset not published)");
    expect(SystemInfo::backend_supports_arch("llamacpp", "rocm", "gfx950") == on_linux,
           "llamacpp gfx950 (bare rocm) honors the Linux-only OS gate");

    // gfx942 carries no gate, so its behavior is untouched by this change.
    expect(SystemInfo::backend_supports_arch("llamacpp", "rocm-stable", "gfx942"),
           "llamacpp gfx942 stable is unaffected by the gfx950 gate");

    if (failures == 0) {
        std::cout << "All llamacpp gfx950 gating assertions passed" << std::endl;
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
