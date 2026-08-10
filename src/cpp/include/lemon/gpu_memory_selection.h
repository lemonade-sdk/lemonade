#pragma once

#include <lemon/system_info.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace lemon {

enum class GpuMemoryVendor { Any, Amd, Nvidia, Metal };

struct GpuMemoryPool {
    double total_gb = 0.0;
    double used_gb = -1.0;
    std::string label;
};

inline std::vector<int> gpu_indices_for_target(const std::string& device,
                                                const std::string& prefix) {
    std::vector<int> indices;
    std::istringstream stream(device);
    std::string token;
    while (std::getline(stream, token, ',')) {
        if (token.rfind(prefix, 0) != 0 || token.size() == prefix.size()) return {};
        try {
            size_t consumed = 0;
            int index = std::stoi(token.substr(prefix.size()), &consumed);
            if (consumed != token.size() - prefix.size() || index < 0) return {};
            indices.push_back(index);
        } catch (...) {
            return {};
        }
    }
    return indices;
}

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
                                             const GPUInfo& apple,
                                             const std::string& device = "") {
    auto pool_for = [](const GPUInfo& gpu, const std::string& label, bool unified = false) {
        double used_gb = gpu.vram_used_gb;
        if (unified) {
            used_gb = gpu.vram_used_gb >= 0.0 && gpu.virtual_used_gb >= 0.0
                ? gpu.vram_used_gb + gpu.virtual_used_gb
                : -1.0;
        }
        return GpuMemoryPool{gpu.vram_gb + (unified ? gpu.virtual_gb : 0.0),
                             used_gb,
                             label};
    };
    auto most_constrained = [](const std::vector<GpuMemoryPool>& pools) {
        GpuMemoryPool result;
        for (const auto& pool : pools) {
            if (pool.total_gb <= 0) continue;
            const double available = pool.total_gb - (std::max)(0.0, pool.used_gb);
            const double result_available =
                result.total_gb - (std::max)(0.0, result.used_gb);
            if (result.total_gb <= 0 || available < result_available)
                result = pool;
        }
        return result;
    };
    auto select_amd = [&]() -> GpuMemoryPool {
        std::vector<const GPUInfo*> devices;
        if (amd_igpu.available && amd_igpu.vram_gb > 0) devices.push_back(&amd_igpu);
        for (const auto& gpu : amd_dgpus)
            if (gpu.available && gpu.vram_gb > 0) devices.push_back(&gpu);
        auto indices = gpu_indices_for_target(device, "ROCm");
        if (device.rfind("ROCm", 0) == 0 && indices.empty()) return {};
        if (!indices.empty()) {
            std::vector<GpuMemoryPool> pools;
            for (int index : indices) {
                if (index >= static_cast<int>(devices.size())) continue;
                const auto& gpu = *devices[index];
                pools.push_back(pool_for(gpu, "AMD ROCm" + std::to_string(index),
                                         &gpu == &amd_igpu));
            }
            return most_constrained(pools);
        }
        if (amd_igpu.available && amd_igpu.vram_gb > 0) {
            return pool_for(amd_igpu, "AMD iGPU", true);
        }
        for (const auto& gpu : amd_dgpus) {
            if (gpu.available && gpu.vram_gb > 0) return pool_for(gpu, "AMD dGPU");
        }
        return {};
    };
    auto select_nvidia = [&]() -> GpuMemoryPool {
        auto indices = gpu_indices_for_target(device, "CUDA");
        if (device.rfind("CUDA", 0) == 0 && indices.empty()) return {};
        if (!indices.empty()) {
            std::vector<GpuMemoryPool> pools;
            for (int index : indices) {
                for (size_t position = 0; position < nvidia_gpus.size(); ++position) {
                    const auto& gpu = nvidia_gpus[position];
                    if (!gpu.available || gpu.vram_gb <= 0) continue;
                    if ((gpu.index >= 0 ? gpu.index : static_cast<int>(position)) == index)
                        pools.push_back(pool_for(gpu, "NVIDIA CUDA" + std::to_string(index)));
                }
            }
            return most_constrained(pools);
        }
        for (const auto& gpu : nvidia_gpus) {
            if (gpu.available && gpu.vram_gb > 0) return pool_for(gpu, "NVIDIA");
        }
        return {};
    };
    auto select_metal = [&]() -> GpuMemoryPool {
        if (apple.available && apple.vram_gb > 0) return pool_for(apple, "Metal");
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
