#pragma once

#include <lemon/system_info.h>

#include <algorithm>
#include <cctype>
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

struct GpuTargetIndices {
    bool targets_vendor = false;
    bool valid = true;
    std::vector<int> indices;
};

inline std::string gpu_memory_ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

inline GpuTargetIndices gpu_indices_for_target(const std::string& device,
                                                const std::string& prefix) {
    GpuTargetIndices result;
    const std::string lower_device = gpu_memory_ascii_lower(device);
    const std::string lower_prefix = gpu_memory_ascii_lower(prefix);
    if (lower_device.rfind(lower_prefix, 0) != 0) return result;

    result.targets_vendor = true;
    if (lower_device == lower_prefix) return result;

    std::istringstream stream(lower_device);
    std::string token;
    while (std::getline(stream, token, ',')) {
        if (token.rfind(lower_prefix, 0) != 0 || token.size() == lower_prefix.size()) {
            result.valid = false;
            result.indices.clear();
            return result;
        }
        try {
            size_t consumed = 0;
            int index = std::stoi(token.substr(lower_prefix.size()), &consumed);
            if (consumed != token.size() - lower_prefix.size() || index < 0) {
                result.valid = false;
                result.indices.clear();
                return result;
            }
            result.indices.push_back(index);
        } catch (...) {
            result.valid = false;
            result.indices.clear();
            return result;
        }
    }
    return result;
}

inline GpuMemoryVendor gpu_memory_vendor_for_target(const std::string& backend,
                                                     const std::string& device) {
    const std::string lower_backend = gpu_memory_ascii_lower(backend);
    const std::string lower_device = gpu_memory_ascii_lower(device);
    if (lower_backend == "cuda" || lower_device.rfind("cuda", 0) == 0)
        return GpuMemoryVendor::Nvidia;
    if (lower_backend.rfind("rocm", 0) == 0 || lower_device.rfind("rocm", 0) == 0)
        return GpuMemoryVendor::Amd;
    if (lower_backend == "metal" || lower_device.rfind("metal", 0) == 0)
        return GpuMemoryVendor::Metal;
    return GpuMemoryVendor::Any;
}

inline GpuMemoryPool select_gpu_memory_pool(GpuMemoryVendor vendor,
                                             const GPUInfo& amd_igpu,
                                             const std::vector<GPUInfo>& amd_dgpus,
                                             const std::vector<GPUInfo>& nvidia_gpus,
                                             const GPUInfo& apple,
                                             const std::string& device = "",
                                             const std::string& cuda_visible_devices = "") {
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
        auto target = gpu_indices_for_target(device, "ROCm");
        if (target.targets_vendor && !target.valid) return {};
        if (!target.indices.empty()) {
            std::vector<GpuMemoryPool> pools;
            // llama.cpp numbers ROCm devices by HSA-agent order. SystemInfo currently
            // exposes the iGPU followed by dGPUs, which is the order used here until it
            // carries a runtime ordinal for each AMD device.
            for (int index : target.indices) {
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
        auto target = gpu_indices_for_target(device, "CUDA");
        if (target.targets_vendor && !target.valid) return {};
        std::vector<const GPUInfo*> visible_gpus;
        if (cuda_visible_devices.empty()) {
            for (const auto& gpu : nvidia_gpus) {
                if (gpu.available && gpu.vram_gb > 0) visible_gpus.push_back(&gpu);
            }
        } else {
            std::istringstream stream(cuda_visible_devices);
            std::string token;
            while (std::getline(stream, token, ',')) {
                const GPUInfo* match = nullptr;
                try {
                    size_t consumed = 0;
                    int physical_index = std::stoi(token, &consumed);
                    if (consumed == token.size() && physical_index >= 0) {
                        for (const auto& gpu : nvidia_gpus) {
                            if (gpu.available && gpu.vram_gb > 0 &&
                                gpu.index == physical_index) {
                                match = &gpu;
                                break;
                            }
                        }
                    }
                } catch (...) {
                }
                if (!match && token.rfind("GPU-", 0) == 0) {
                    for (const auto& gpu : nvidia_gpus) {
                        if (gpu.available && gpu.vram_gb > 0 &&
                            gpu.uuid.rfind(token, 0) == 0) {
                            if (match) {
                                match = nullptr;  // Ambiguous UUID prefix.
                                break;
                            }
                            match = &gpu;
                        }
                    }
                }
                // CUDA stops exposing devices at the first invalid entry (for example,
                // CUDA_VISIBLE_DEVICES=2,-1,3 exposes only physical GPU 2).
                if (!match) break;
                visible_gpus.push_back(match);
            }
        }

        if (!target.indices.empty()) {
            std::vector<GpuMemoryPool> pools;
            for (int index : target.indices) {
                if (index >= static_cast<int>(visible_gpus.size())) continue;
                pools.push_back(pool_for(*visible_gpus[index],
                                         "NVIDIA CUDA" + std::to_string(index)));
            }
            return most_constrained(pools);
        }
        if (!visible_gpus.empty()) return pool_for(*visible_gpus.front(), "NVIDIA");
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
