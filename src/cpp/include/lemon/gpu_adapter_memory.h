#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace lemon {

struct GpuAdapterMemory {
    uint64_t dedicated_bytes = 0;
};

// Windows reports one row per adapter LUID: a single card's usage is the largest
// row, not the sum, and rows cannot be attributed once a second card is present.
inline double single_physical_gpu_dedicated_gb(size_t physical_gpu_count,
                                               const std::vector<GpuAdapterMemory>& adapters) {
    if (physical_gpu_count != 1 || adapters.empty()) return -1.0;

    uint64_t dedicated_bytes = 0;
    for (const auto& adapter : adapters) {
        dedicated_bytes = (std::max)(dedicated_bytes, adapter.dedicated_bytes);
    }
    return static_cast<double>(dedicated_bytes) / (1024.0 * 1024.0 * 1024.0);
}

} // namespace lemon
