// Unit tests for lemon::backends::BackendUtils::parse_pip_download_line().
//
// The streamed ROCm wheel install parses pip's "Downloading <name> (<size>)"
// lines to drive a real progress bar. A parser that fails to extract the size
// leaves the running total at zero, which previously caused an unguarded
// divide-by-zero (SIGFPE) on the flagship install path. These tests pin the
// parser's behaviour against pip's actual output shape.

#include <cstddef>
#include <iostream>
#include <string>

#include <lemon/backends/backend_utils.h>

using lemon::backends::BackendUtils;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& msg) {
    if (cond) {
        std::cout << "[ok] " << msg << std::endl;
    } else {
        std::cerr << "[FAIL] " << msg << std::endl;
        ++g_failures;
    }
}

}  // namespace

int main() {
    std::string name;
    std::size_t bytes = 0;

    // Real pip output: uppercase MB unit, parens after the filename token.
    {
        const bool matched = BackendUtils::parse_pip_download_line(
            "Downloading rocm_sdk_core-7.14.0-py3-none-manylinux_2_28_x86_64.whl (414.0 MB)",
            name, bytes);
        check(matched, "matches a real pip Downloading line");
        check(name == "rocm_sdk_core-7.14.0-py3-none-manylinux_2_28_x86_64.whl",
              "extracts the wheel basename");
        check(bytes == static_cast<std::size_t>(414.0 * 1048576.0),
              "parses uppercase MB size (regression: token cut before parens)");
    }

    // A URL-form filename: basename is taken after the last slash.
    {
        const bool matched = BackendUtils::parse_pip_download_line(
            "  Downloading https://repo.amd.com/rocm/whl/rocm_sdk_libraries-7.14.0.whl (1.2 GB)",
            name, bytes);
        check(matched, "matches a leading-whitespace / URL download line");
        check(name == "rocm_sdk_libraries-7.14.0.whl", "strips URL path to basename");
        check(bytes == static_cast<std::size_t>(1.2 * 1073741824.0), "parses GB size");
    }

    // kB unit.
    {
        BackendUtils::parse_pip_download_line("Downloading tiny-1.0.whl (12.5 kB)", name, bytes);
        check(bytes == static_cast<std::size_t>(12.5 * 1024.0), "parses kB size");
    }

    // Download line with no size in parens: valid line, zero bytes (no crash).
    {
        const bool matched =
            BackendUtils::parse_pip_download_line("Downloading nosize-1.0.whl", name, bytes);
        check(matched, "matches a download line without a size");
        check(bytes == 0, "reports zero bytes when the size is absent");
        check(name == "nosize-1.0.whl", "still extracts the filename without a size");
    }

    // Non-download lines are rejected.
    {
        check(!BackendUtils::parse_pip_download_line("Installing collected packages: rocm", name, bytes),
              "rejects a non-download line");
        check(!BackendUtils::parse_pip_download_line("", name, bytes), "rejects an empty line");
    }

    // Malformed size does not throw and yields zero bytes.
    {
        const bool matched =
            BackendUtils::parse_pip_download_line("Downloading weird-1.0.whl (garbage)", name, bytes);
        check(matched, "matches even with a malformed size");
        check(bytes == 0, "malformed size yields zero without throwing");
    }

    if (g_failures > 0) {
        std::cerr << "Total failures: " << g_failures << std::endl;
        return 1;
    }
    std::cout << "All pip download parser unit tests passed!" << std::endl;
    return 0;
}
