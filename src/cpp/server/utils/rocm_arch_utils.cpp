#include <iostream>
#include <string>
#include <regex>
#include "lemon/utils/rocm_arch_utils.h"

namespace lemon{


std::string ROCmArchUtils::transform_isakfd_to_gfx(const std::string& isa) {
    if (!isa.empty() &&
        std::all_of(isa.begin(), isa.end(), ::isdigit)) {
        int v;
        try {
            v = std::stoi(isa);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "Failed to parse gfx_target_version '" + isa + "': " + e.what());
        }
        int major = v / 10000;
        int minor = (v / 100) % 100;
        int step  = v % 100;

        char buf[16];
        std::snprintf(buf, sizeof(buf), "gfx%d%x%x", major, minor, step);
        return std::string(buf);
    } else {
        return "";
    }
}

std::string ROCmArchUtils::get_gfx_from_device_name(const std::string& device_name) {
    std::string device_lower = device_name;
    std::transform(device_lower.begin(), device_lower.end(), device_lower.begin(), ::tolower);

    std::smatch gfx_match;
    // Match 3- or 4-digit gfx tokens; the trailing nibble can be hex (e.g. gfx90a).
    if (std::regex_search(device_lower, gfx_match, std::regex(R"((gfx[0-9a-f]{3,4}))"))) {
        return gfx_match[1].str();
    }
    return "";
} 


bool ROCmArchUtils::rocm_arch_is_valid_gfx(const std::string& gfx_arch) {
    std::string gfx = ROCmArchUtils::get_gfx_from_device_name(gfx_arch);
    return !gfx.empty();
}

std::vector<ROCmDeviceInfo> ROCmArchUtils::rocm_arch_get_active_devices(const json& devices) {
    std::vector<ROCmDeviceInfo> active_devs;

    if (devices.contains("amd_gpu")) {
        if (devices["amd_gpu"].is_array()) {
            for (const auto& amd_gpu : devices["amd_gpu"]) {
                ROCmDeviceInfo dev;
                if (amd_gpu.contains("available") && amd_gpu["available"].is_boolean() && amd_gpu["available"]) {
                    dev.name = ROCmArchUtils::transform_isakfd_to_gfx(amd_gpu["name"]);
                    dev.vram_gb = amd_gpu["vram_gb"];
                    active_devs.push_back(dev);
                }
            }
        }
    }
    return active_devs;
}

} // namespace lemon
