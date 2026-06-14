#pragma once

#include <string>

namespace lemon {

//ROCm Device information structure
struct ROCmDeviceInfo {
    std::string family;
    std::string name;
    std::string 
    float vram_gb;
};

class ROCmArchUtils {

    static std::string rocm_arch_numeric_to_gfx(const std::string& numeric_version);
    static bool rocm_arch_is_valid_gfx(const std::string& gfx_arch);
    static std::vector<ROCmDeviceInfo> rocm_arch_get_active_devices(const json& json_devices);
};

}  // namespace lemon