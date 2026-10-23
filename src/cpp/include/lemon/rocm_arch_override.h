#pragma once

#include "lemon/system_info.h"

#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace lemon {

inline const char* rocm_visibility_override_env_name(
    const char* rocr_visible_devices,
    const char* hip_visible_devices,
    const char* cuda_visible_devices,
    const char* gpu_device_ordinal) {
#ifdef _WIN32
    (void)rocr_visible_devices;
    (void)cuda_visible_devices;
    (void)gpu_device_ordinal;
    return hip_visible_devices ? "HIP_VISIBLE_DEVICES" : nullptr;
#else
    if (rocr_visible_devices) {
        return "ROCR_VISIBLE_DEVICES";
    }
    if (hip_visible_devices) {
        return "HIP_VISIBLE_DEVICES";
    }
    if (cuda_visible_devices) {
        return "CUDA_VISIBLE_DEVICES";
    }
    return gpu_device_ordinal ? "GPU_DEVICE_ORDINAL" : nullptr;
#endif
}

// An existing visibility environment changes ROCm's ordinal space. Leave both
// architecture detection and UUID pinning to that explicit caller-owned policy.
inline const char* existing_rocm_visibility_override_env_name() {
    return rocm_visibility_override_env_name(
        std::getenv("ROCR_VISIBLE_DEVICES"),
        std::getenv("HIP_VISIBLE_DEVICES"),
        std::getenv("CUDA_VISIBLE_DEVICES"),
        std::getenv("GPU_DEVICE_ORDINAL"));
}

inline bool should_apply_rocm_device_arch_override(
    const std::string& device,
    const std::string& existing_arch_override,
    const char* visibility_override) {
    if (!existing_arch_override.empty() || visibility_override) {
        return false;
    }

    const auto start = device.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return false;
    }
    const auto end = device.find_last_not_of(" \t\r\n");
    std::string normalized = device.substr(start, end - start + 1);
    for (char& character : normalized) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return normalized != "rocm" && normalized.rfind("rocm", 0) == 0;
}

class RocmArchOverrideGuard {
public:
    explicit RocmArchOverrideGuard(const std::string& device)
        : previous_(SystemInfo::get_rocm_arch_override()) {
        if (!should_apply_rocm_device_arch_override(
                device, previous_, existing_rocm_visibility_override_env_name())) {
            return;
        }

        const std::string arch = SystemInfo::get_rocm_arch_for_device(device);
        if (arch.empty()) {
#ifdef _WIN32
            throw std::invalid_argument(
                "Cannot use ROCm device '" + device + "'. Check that each ROCmN exists and is a "
                "supported AMD GPU. On Windows, explicit ROCmN architecture selection requires "
                "exactly one detected AMD GPU because WMI does not expose HIP ordinals.");
#else
            throw std::invalid_argument(
                "Cannot use ROCm device '" + device + "'. Check that each ROCmN exists and is a "
                "supported AMD GPU. If you listed more than one GPU, they must all use the same "
                "ROCm build family; for mixed GPUs, use the Vulkan backend.");
#endif
        }
        SystemInfo::set_rocm_arch_override(arch);
        active_ = true;
    }

    ~RocmArchOverrideGuard() {
        if (active_) {
            SystemInfo::set_rocm_arch_override(previous_);
        }
    }

    RocmArchOverrideGuard(const RocmArchOverrideGuard&) = delete;
    RocmArchOverrideGuard& operator=(const RocmArchOverrideGuard&) = delete;

private:
    std::string previous_;
    bool active_ = false;
};

}  // namespace lemon
