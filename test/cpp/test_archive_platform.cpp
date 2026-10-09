#include "lemon/utils/archive_platform.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

using lemon::utils::flatten_single_wrapper_directory;
using lemon::utils::resolve_archive_strip_components;
using lemon::utils::resolve_tarball_strip_components;
using lemon::utils::tarball_listing_timeout_seconds;

namespace fs = std::filesystem;

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

    // Zip listings share the strip decision (issue #2856): FastFlowLM-style
    // zips nest the payload under a single wrapper folder.
    check(
        "wrapped zip strips one directory",
        resolve_archive_strip_components(
            0,
            "fastflowlm-windows-0123abc/\n"
            "fastflowlm-windows-0123abc/flm.exe\n"
            "fastflowlm-windows-0123abc/flm.dll\n") == std::optional<int>{1});

    check(
        "root-level zip keeps its layout",
        resolve_archive_strip_components(
            0,
            "flm.exe\n"
            "flm.dll\n") == std::optional<int>{0});

    check(
        "failed zip listing has no safe strip value",
        !resolve_archive_strip_components(-1, "flm.exe\n").has_value());

    // The flatten helper gives extractors without --strip-components
    // (unzip, Expand-Archive) the same end layout.
    const fs::path scratch = fs::temp_directory_path() / "lemonade-test-flatten-wrapper";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    const fs::path wrapper = scratch / "fastflowlm-windows-0123abc";
    fs::create_directories(wrapper);
    { std::ofstream(wrapper / "flm.exe") << "x"; }
    { std::ofstream(wrapper / "flm.dll") << "x"; }

    check(
        "single wrapper directory is flattened",
        flatten_single_wrapper_directory(scratch) &&
            fs::exists(scratch / "flm.exe") &&
            fs::exists(scratch / "flm.dll") &&
            !fs::exists(wrapper));

    check(
        "root-level layout is left untouched",
        !flatten_single_wrapper_directory(scratch) &&
            fs::exists(scratch / "flm.exe"));

    fs::remove_all(scratch, ec);

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
