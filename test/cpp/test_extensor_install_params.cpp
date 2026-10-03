#include "lemon/backends/extensor/extensor_install.h"

#include <iostream>
#include <string>
#include <utility>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    std::cout << (condition ? "PASS: " : "FAIL: ") << message << '\n';
    if (!condition) ++failures;
}

void expect_rejected(const std::string& backend, const std::string& version,
                     const std::string& os, const std::string& arch) {
    bool threw = false;
    try {
        lemon::backends::extensor::install_params(backend, version, os, arch);
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, "rejects " + backend + "/" + version + "/" + os + "/" + arch);
}
} // namespace

int main() {
    using lemon::backends::extensor::install_params;
    using lemon::backends::release_asset_url;
    using lemon::backends::release_page_url;

    const std::pair<std::string, std::string> assets[] = {
        {"gfx1151", "https://www.amd.com/content/dam/amd/en/support/downloads/extensor/extensor-v2.3.2-linux-x86_64-rocm7-gfx1151.zip"},
        {"gfx1152", "https://www.amd.com/content/dam/amd/en/support/downloads/extensor/extensor-v2.3.2-linux-x86_64-rocm7-gfx1152.zip"},
        {"gfx1201", "https://www.amd.com/content/dam/amd/en/support/downloads/extensor/extensor-v2.3.2-linux-x86_64-rocm7-gfx1201.zip"},
    };
    for (const auto& [arch, url] : assets) {
        const auto params = install_params("rocm", "v2.3.2", "linux", arch);
        expect(params.download_url == url, arch + " resolves the exact AMD URL");
        expect(params.repo.empty(), arch + " requires no GitHub repository");
        expect(params.filename == url.substr(url.find_last_of('/') + 1),
               arch + " preserves the ZIP filename");
        expect(release_asset_url(params.repo, params.version_override,
                                 params.filename, params.download_url) == url,
               arch + " installer and dry-run use the AMD URL");
        expect(release_page_url(params.repo, params.version_override, params.download_url) == url,
               arch + " status links to the AMD asset");
    }

    const auto unprefixed = install_params("rocm", "2.3.2", "linux", "gfx1151");
    expect(unprefixed.version_override.empty(), "version tracking preserves the configured pin");
    expect(unprefixed.filename == "extensor-v2.3.2-linux-x86_64-rocm7-gfx1151.zip",
           "explicit versions without v select the canonical asset filename");
    expect(install_params("rocm", "v2.4.0", "linux", "gfx1151").filename ==
               "extensor-v2.4.0-linux-x86_64-rocm7-gfx1151.zip",
           "explicit version selects that version's asset");

    for (const auto& arch : {"", "gfx1150", "gfx1200", "gfx120X", "gfx942"}) {
        expect_rejected("rocm", "v2.3.2", "linux", arch);
    }
    for (const auto& os : {"windows", "macos"}) {
        expect_rejected("rocm", "v2.3.2", os, "gfx1151");
    }
    for (const auto& backend : {"cpu", "vulkan", "system"}) {
        expect_rejected(backend, "v2.3.2", "linux", "gfx1151");
    }
    for (const auto& version : {"", "latest", "v2.3", "v2.3.2/other", "../v2.3.2"}) {
        expect_rejected("rocm", version, "linux", "gfx1151");
    }

    expect(release_asset_url("ggml-org/llama.cpp", "b8664", "llama.zip", "") ==
               "https://github.com/ggml-org/llama.cpp/releases/download/b8664/llama.zip",
           "GitHub asset URL construction is preserved");
    expect(release_page_url("ggml-org/llama.cpp", "b8664", "") ==
               "https://github.com/ggml-org/llama.cpp/releases/tag/b8664",
           "GitHub release page construction is preserved");
    return failures == 0 ? 0 : 1;
}
