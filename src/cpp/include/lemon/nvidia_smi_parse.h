#pragma once

#include <string>
#include <vector>

namespace lemon {

/// One row of `nvidia-smi --query-gpu=index,uuid,name,compute_cap,driver_version,
/// memory.total,memory.used --format=csv,noheader,nounits`.
struct NvidiaSmiGpuInfo {
    int index = -1;
    std::string uuid;          // e.g. "GPU-..."
    std::string name;
    std::string compute_cap;   // e.g. "8.6"
    std::string driver_version;
    double vram_gb = 0.0;
    // -1 when the driver reports the field as "[Not Supported]" or "N/A", which must
    // stay distinguishable from a genuinely idle card.
    double vram_used_gb = -1.0;
};

inline std::string nvidia_smi_trim(std::string s) {
    const size_t start = s.find_first_not_of(" \t\r\n");
    const size_t end = s.find_last_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

/// `fallback_index` is used when the row's own index field is absent or unparseable.
///
/// GPU names may themselves contain commas ("NVIDIA T400, 4GB"), so the four trailing
/// fields (compute_cap, driver_version, memory.total, memory.used) are peeled off the
/// right before the left side (index, uuid, name) is split.
inline NvidiaSmiGpuInfo parse_nvidia_smi_line(const std::string& raw_line, int fallback_index) {
    NvidiaSmiGpuInfo info;
    const std::string line = nvidia_smi_trim(raw_line);
    if (line.empty()) return info;

    std::string remaining = line;
    std::vector<std::string> tail;
    for (int i = 0; i < 4; i++) {
        const size_t pos = remaining.rfind(", ");
        if (pos == std::string::npos) break;
        tail.insert(tail.begin(), nvidia_smi_trim(remaining.substr(pos + 2)));
        remaining = remaining.substr(0, pos);
    }
    if (tail.size() != 4) return info;

    const size_t first_comma = remaining.find(", ");
    const size_t second_comma = first_comma == std::string::npos
        ? std::string::npos
        : remaining.find(", ", first_comma + 2);

    if (first_comma != std::string::npos && second_comma != std::string::npos) {
        try {
            info.index = std::stoi(nvidia_smi_trim(remaining.substr(0, first_comma)));
        } catch (...) {
            info.index = fallback_index;
        }
        info.uuid = nvidia_smi_trim(remaining.substr(first_comma + 2, second_comma - first_comma - 2));
        info.name = nvidia_smi_trim(remaining.substr(second_comma + 2));
    } else if (first_comma != std::string::npos) {
        try {
            info.index = std::stoi(nvidia_smi_trim(remaining.substr(0, first_comma)));
        } catch (...) {
            info.index = fallback_index;
        }
        info.name = nvidia_smi_trim(remaining.substr(first_comma + 2));
    } else {
        info.index = fallback_index;
        info.name = nvidia_smi_trim(remaining);
    }

    info.compute_cap = tail[0];
    info.driver_version = tail[1];
    try {
        info.vram_gb = std::stod(tail[2]) / 1024.0;
    } catch (...) {}
    try {
        info.vram_used_gb = std::stod(tail[3]) / 1024.0;
    } catch (...) {}

    return info;
}

}  // namespace lemon
