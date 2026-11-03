#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace lemon::backends {

struct OmniUmaBudget {
    uint64_t total_ram = 0;
    uint64_t available_ram = 0;
    uint64_t max_gpu_bytes = 0;
    double gpu_fraction = 0.0;
};

inline OmniUmaBudget calculate_omni_uma_budget(uint64_t total_ram,
                                                 uint64_t available_ram) {
    constexpr uint64_t GiB = 1ULL << 30;
    if (total_ram < 16 * GiB || available_ram > total_ram) {
        throw std::runtime_error("Cannot safely budget gfx1151 Omni: invalid system RAM reading");
    }
    const uint64_t reserve = std::max(8 * GiB, total_ram / 10);
    if (available_ram <= reserve + 6 * GiB) {
        throw std::runtime_error("Not enough available system RAM for gfx1151 Omni (including host reserve)");
    }
    // gfx1151 is UMA. Use physical host RAM as an upper bound on HIP's
    // total-addressable GPU pool, not the VRAM 'used' sysfs counter.
    // Limit *all* colocated stages together, not each stage separately.
    const uint64_t cap = std::min({32 * GiB, (total_ram / 100) * 35,
                                   ((available_ram - reserve) / 100) * 65});
    if (cap < 6 * GiB) {
        throw std::runtime_error("Not enough available system RAM for gfx1151 Omni stages");
    }
    const double fraction = std::floor(
        (static_cast<double>(cap) / static_cast<double>(total_ram)) * 10000.0) / 10000.0;
    if (fraction <= 0.0 || fraction >= 0.9) {
        throw std::runtime_error("Invalid gfx1151 Omni memory fraction");
    }
    return {total_ram, available_ram, cap, fraction};
}

inline uint64_t read_omni_cgroup_free() {
    auto parse = [](const char* path, uint64_t& value) -> bool {
        std::ifstream in(path);
        std::string word;
        if (!(in >> word) || word == "max") return false;
        try {
            size_t end = 0;
            const auto parsed = std::stoull(word, &end);
            if (end != word.size()) return false;
            value = parsed;
            return true;
        } catch (...) {
            return false;
        }
    };
    uint64_t limit = 0, current = 0;
    if (parse("/sys/fs/cgroup/memory.max", limit) &&
        parse("/sys/fs/cgroup/memory.current", current)) {
        return limit > current ? limit - current : 0;
    }
    if (parse("/sys/fs/cgroup/memory/memory.limit_in_bytes", limit) &&
        parse("/sys/fs/cgroup/memory/memory.usage_in_bytes", current) &&
        limit < (1ULL << 60)) {
        return limit > current ? limit - current : 0;
    }
    return std::numeric_limits<uint64_t>::max();
}

inline OmniUmaBudget detect_omni_uma_budget() {
    std::ifstream in("/proc/meminfo");
    std::string line;
    uint64_t total = 0, available = 0;
    while (std::getline(in, line)) {
        std::istringstream entry(line);
        std::string key, unit;
        uint64_t kib = 0;
        if (!(entry >> key >> kib >> unit) || unit != "kB") continue;
        if (key == "MemTotal:") total = kib * 1024;
        if (key == "MemAvailable:") available = kib * 1024;
    }
    if (total == 0 || available == 0) {
        throw std::runtime_error("Cannot read /proc/meminfo; refusing unbounded gfx1151 Omni start");
    }
    return calculate_omni_uma_budget(total, std::min(available, read_omni_cgroup_free()));
}

} // namespace lemon::backends
