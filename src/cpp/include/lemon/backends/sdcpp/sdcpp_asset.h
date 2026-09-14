#pragma once

#include <stdexcept>
#include <string>

namespace lemon {
namespace backends {
namespace sdcpp {

// Pinned installs must name the TheRock runtime exactly (it is what the
// binary runs against); only an explicit "latest" bin pin wildcards it for
// install-time resolution of upstream's newest build.
inline std::string sdcpp_rocm_asset_pattern(const std::string& short_version,
                                            bool wildcard_runtime,
                                            const std::string& therock_version) {
    const std::string runtime = wildcard_runtime ? "*" : therock_version;
#ifdef _WIN32
    return "sd-" + short_version + "-bin-win-rocm-" + runtime + "-x64.zip";
#elif defined(__linux__)
    return "sd-" + short_version + "-bin-Linux-Ubuntu-24.04-x86_64-rocm-" + runtime + ".zip";
#else
    throw std::runtime_error("ROCm sd.cpp only supported on Windows and Linux");
#endif
}

}  // namespace sdcpp
}  // namespace backends
}  // namespace lemon
