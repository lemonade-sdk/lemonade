#include "lemon/gpu_exclusivity_policy.h"

namespace lemon {

std::vector<size_t> gpu_exclusivity_eviction_targets(
        const IncomingLoadGpuInfo& incoming,
        const std::vector<LoadedGpuServerInfo>& loaded) {
    std::vector<size_t> targets;
    if (!(incoming.device & DEVICE_GPU) || !incoming.is_amd_gpu) {
        return targets;
    }

    if (incoming.policy == SlotPolicy::ExclusiveGpu) {
        for (size_t i = 0; i < loaded.size(); ++i) {
            if ((loaded[i].device & DEVICE_GPU) && loaded[i].is_amd_gpu) {
                targets.push_back(i);
            }
        }
        return targets;
    }

    for (size_t i = 0; i < loaded.size(); ++i) {
        if ((loaded[i].device & DEVICE_GPU) && loaded[i].is_amd_gpu &&
            loaded[i].policy == SlotPolicy::ExclusiveGpu) {
            targets.push_back(i);
            break;
        }
    }
    return targets;
}

}  // namespace lemon
