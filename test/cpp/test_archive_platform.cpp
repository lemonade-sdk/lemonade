#include "lemon/utils/archive_platform.h"

#include <cstdio>
#include <optional>
#include <string>

using lemon::utils::resolve_tarball_strip_components;
using lemon::utils::tarball_listing_timeout_seconds;

namespace {

int failures = 0;

void check(const char* name, bool condition) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        ++failures;
    }
}

} // namespace

int main() {
    check(
        "tarball listing allows large archives to finish",
        tarball_listing_timeout_seconds == 300);

    check(
        "wrapped archive strips one directory",
        resolve_tarball_strip_components(
            0,
            "therock-dist/\n"
            "therock-dist/bin/rocminfo.exe\n"
            "therock-dist/lib/amdhip64.lib\n") == std::optional<int>{1});

    check(
        "root-level archive keeps its layout",
        resolve_tarball_strip_components(
            0,
            "bin/rocminfo\n"
            "lib/libamdhip64.so\n"
            "version.txt\n") == std::optional<int>{0});

    check(
        "failed listing has no safe strip value",
        !resolve_tarball_strip_components(
            -1,
            "therock-dist/\n"
            "therock-dist/bin/rocminfo.exe\n").has_value());

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
