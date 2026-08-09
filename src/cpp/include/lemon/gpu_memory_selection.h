#pragma once

#include <lemon/system_info.h>

#include <string>
#include <vector>

namespace lemon {

enum class GpuMemoryVendor { Any, Amd, Nvidia, Metal };

struct GpuMemoryPool {
    double total_gb = 0.0;
    std::string label;
};

inline GpuMemoryVendor gpu_memory_vendor_for_target(const std::string& backend,
                                                     const std::string& device) {
    if (backend == "cuda" || device.rfind("CUDA", 0) == 0) return GpuMemoryVendor::Nvidia;
    if (backend.rfind("rocm", 0) == 0 || device.rfind("ROCm", 0) == 0) return GpuMemoryVendor::Amd;
    if (backend == "metal" || device.rfind("Metal", 0) == 0) return GpuMemoryVendor::Metal;
    return GpuMemoryVendor::Any;
}

inline GpuMemoryPool select_gpu_memory_pool(GpuMemoryVendor vendor,
                                             const GPUInfo& amd_igpu,
                                             const std::vector<GPUInfo>& amd_dgpus,
                                             const std::vector<GPUInfo>& nvidia_gpus,
                                             const GPUInfo& apple) {
    auto select_amd = [&]() -> GpuMemoryPool {
        if (amd_igpu.available && amd_igpu.vram_gb > 0) {
            return {amd_igpu.vram_gb + amd_igpu.virtual_gb, "AMD iGPU"};
        }
        for (const auto& gpu : amd_dgpus) {
            if (gpu.available && gpu.vram_gb > 0) return {gpu.vram_gb, "AMD dGPU"};
        }
        return {};
    };
    auto select_nvidia = [&]() -> GpuMemoryPool {
        for (const auto& gpu : nvidia_gpus) {
            if (gpu.available && gpu.vram_gb > 0) return {gpu.vram_gb, "NVIDIA"};
        }
        return {};
    };
    auto select_metal = [&]() -> GpuMemoryPool {
        if (apple.available && apple.vram_gb > 0) return {apple.vram_gb, "Metal"};
        return {};
    };

    if (vendor == GpuMemoryVendor::Amd) return select_amd();
    if (vendor == GpuMemoryVendor::Nvidia) return select_nvidia();
    if (vendor == GpuMemoryVendor::Metal) return select_metal();

    auto pool = select_amd();
    if (pool.total_gb > 0) return pool;
    pool = select_nvidia();
    if (pool.total_gb > 0) return pool;
    return select_metal();
}

} // namespace lemon
