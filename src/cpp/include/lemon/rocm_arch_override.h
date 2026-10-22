#pragma once

#include "lemon/system_info.h"

#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace lemon {

// An existing visibility environment changes ROCm's ordinal space. Leave both
// architecture detection and UUID pinning to that explicit caller-owned policy.
inline const char* existing_rocm_visibility_override_env_name() {
#ifdef _WIN32
    if (std::getenv("HIP_VISIBLE_DEVICES")) {
        return "HIP_VISIBLE_DEVICES";
    }
#else
    if (std::getenv("ROCR_VISIBLE_DEVICES")) {
        return "ROCR_VISIBLE_DEVICES";
    }
    if (std::getenv("HIP_VISIBLE_DEVICES")) {
        return "HIP_VISIBLE_DEVICES";
    }
#endif
    return nullptr;
}

class RocmArchOverrideGuard {
public:
    explicit RocmArchOverrideGuard(const std::string& device)
        : previous_(SystemInfo::get_rocm_arch_override()) {
        if (!previous_.empty() || existing_rocm_visibility_override_env_name()) {
            return;
        }

        const auto start = device.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) {
            return;
        }
        const auto end = device.find_last_not_of(" \t\r\n");
        std::string normalized = device.substr(start, end - start + 1);
        for (char& character : normalized) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        if (normalized == "rocm" || normalized.rfind("rocm", 0) != 0) {
            return;
        }

        const std::string arch = SystemInfo::get_rocm_arch_for_device(device);
        if (arch.empty()) {
            throw std::invalid_argument(
                "Cannot use ROCm device '" + device + "'. Check that each ROCmN exists and is a "
                "supported AMD GPU. If you listed more than one GPU, they must all use the same "
                "ROCm build family; for mixed GPUs, use the Vulkan backend.");
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
