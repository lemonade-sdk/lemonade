#pragma once

#include "lemon/backends/install_params.h"

#include <regex>
#include <stdexcept>
#include <string>

namespace lemon::backends::extensor {

inline InstallParams install_params(const std::string& backend,
                                     const std::string& version,
                                     const std::string& os,
                                     const std::string& arch) {
    if (backend != "rocm") {
        throw std::invalid_argument("EXTENSOR only supports the rocm backend");
    }
    if (os != "linux") {
        throw std::runtime_error("EXTENSOR publishes Linux binaries only");
    }
    if (arch != "gfx1151" && arch != "gfx1152" && arch != "gfx1201") {
        throw std::runtime_error(
            "EXTENSOR has no binary for GPU architecture '" + arch +
            "'; supported targets: gfx1151, gfx1152, gfx1201");
    }
    static const std::regex version_pattern(R"(v?[0-9]+\.[0-9]+\.[0-9]+)");
    if (!std::regex_match(version, version_pattern)) {
        throw std::invalid_argument("Invalid EXTENSOR version '" + version +
                                    "'; expected vMAJOR.MINOR.PATCH");
    }
    const std::string tag = version.front() == 'v' ? version : "v" + version;
    InstallParams params;
    params.filename = "extensor-" + tag + "-linux-x86_64-rocm7-" + arch + ".zip";
    params.download_url = "https://www.amd.com/content/dam/amd/en/support/downloads/extensor/" +
                          params.filename;
    return params;
}

} // namespace lemon::backends::extensor
