#pragma once

#include <string>
#include <vector>

namespace lemon {

/// One row of `nvidia-smi --query-gpu=index,uuid,name,compute_cap,driver_version,
/// memory.total,memory.used --format=csv,noheader,nounits`.
struct NvidiaSmiRow {
    bool valid = false;
    int index = -1;
    std::string uuid;
    std::string name;
    std::string compute_cap;
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
/// fields are peeled off the right before the left side is split.
inline NvidiaSmiRow parse_nvidia_smi_row(const std::string& raw_line, int fallback_index) {
    NvidiaSmiRow row;
    const std::string line = nvidia_smi_trim(raw_line);
    if (line.empty()) return row;

    std::string remaining = line;
    std::vector<std::string> tail;
    for (int i = 0; i < 4; i++) {
        const size_t pos = remaining.rfind(", ");
        if (pos == std::string::npos) break;
        tail.insert(tail.begin(), nvidia_smi_trim(remaining.substr(pos + 2)));
        remaining = remaining.substr(0, pos);
    }
    if (tail.size() != 4) return row;

    const size_t first_comma = remaining.find(", ");
    const size_t second_comma = first_comma == std::string::npos
        ? std::string::npos
        : remaining.find(", ", first_comma + 2);

    if (first_comma != std::string::npos && second_comma != std::string::npos) {
        try {
            row.index = std::stoi(nvidia_smi_trim(remaining.substr(0, first_comma)));
        } catch (...) {
            row.index = fallback_index;
        }
        row.uuid = nvidia_smi_trim(remaining.substr(first_comma + 2, second_comma - first_comma - 2));
        row.name = nvidia_smi_trim(remaining.substr(second_comma + 2));
    } else if (first_comma != std::string::npos) {
        try {
            row.index = std::stoi(nvidia_smi_trim(remaining.substr(0, first_comma)));
        } catch (...) {
            row.index = fallback_index;
        }
        row.name = nvidia_smi_trim(remaining.substr(first_comma + 2));
    } else {
        row.index = fallback_index;
        row.name = nvidia_smi_trim(remaining);
    }

    row.compute_cap = tail[0];
    row.driver_version = tail[1];
    try {
        row.vram_gb = std::stod(tail[2]) / 1024.0;
    } catch (...) {}
    try {
        row.vram_used_gb = std::stod(tail[3]) / 1024.0;
    } catch (...) {}

    row.valid = true;
    return row;
}

}  // namespace lemon
