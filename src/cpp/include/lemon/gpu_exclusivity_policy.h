#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "lemon/backends/backend_descriptor.h"
#include "lemon/model_types.h"

namespace lemon {

// One already-loaded server's facts, as needed for GPU exclusivity decisions.
// model_name is carried for logging only, not used in the decision itself.
struct LoadedGpuServerInfo {
    std::string model_name;
    DeviceType device = DEVICE_NONE;
    SlotPolicy policy = SlotPolicy::Standard;
    bool is_amd_gpu = false;
};

struct IncomingLoadGpuInfo {
    DeviceType device = DEVICE_NONE;
    SlotPolicy policy = SlotPolicy::Standard;
    bool is_amd_gpu = false;
};

// Pure decision: indices into `loaded` that must be evicted before `incoming`
// loads, to enforce AMD-GPU-scoped ExclusiveGpu semantics bidirectionally. No
// I/O, no WrappedServer/Router dependency.
std::vector<size_t> gpu_exclusivity_eviction_targets(
    const IncomingLoadGpuInfo& incoming,
    const std::vector<LoadedGpuServerInfo>& loaded);

}  // namespace lemon
