#pragma once

#include <lemon/system_info.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace lemon {

enum class GpuMemoryVendor { Any, Amd, Nvidia, Metal };

struct GpuMemoryPool {
    double total_gb = 0.0;
    double used_gb = -1.0;
    std::string label;
    GpuMemoryVendor vendor = GpuMemoryVendor::Any;
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
                                             const std::optional<std::string>& cuda_visible_devices =
                                                 std::nullopt) {
    auto pool_for = [](const GPUInfo& gpu, const std::string& label,
                       GpuMemoryVendor vendor, bool unified = false) {
        double used_gb = gpu.vram_used_gb;
        if (unified) {
            used_gb = gpu.vram_used_gb >= 0.0 && gpu.virtual_used_gb >= 0.0
                ? gpu.vram_used_gb + gpu.virtual_used_gb
                : -1.0;
        }
        return GpuMemoryPool{gpu.vram_gb + (unified ? gpu.virtual_gb : 0.0),
                             used_gb, label, vendor};
    };
    auto most_constrained = [](const std::vector<GpuMemoryPool>& pools) {
        GpuMemoryPool result;
        for (const auto& pool : pools) {
            if (pool.total_gb <= 0) continue;
            const double available = pool.used_gb < 0.0
                ? 0.0
                : pool.total_gb - pool.used_gb;
            const double result_available = result.used_gb < 0.0
                ? 0.0
                : result.total_gb - result.used_gb;
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
            // ROCm numbers devices by runtime ordinal (ascending KFD node number on
            // Linux, reported as GPUInfo::index). Platforms that cannot supply a stable
            // ordinal (index == -1, e.g. Windows) fall back to iGPU-then-dGPU
            // enumeration order.
            const bool have_ordinals = std::any_of(
                devices.begin(), devices.end(),
                [](const GPUInfo* gpu) { return gpu->index >= 0; });
            std::vector<GpuMemoryPool> pools;
            for (int index : target.indices) {
                const GPUInfo* gpu = nullptr;
                if (have_ordinals) {
                    for (const auto* candidate : devices) {
                        if (candidate->index == index) { gpu = candidate; break; }
                    }
                } else if (index < static_cast<int>(devices.size())) {
                    gpu = devices[index];
                }
                if (!gpu) continue;
                pools.push_back(pool_for(*gpu, "AMD ROCm" + std::to_string(index),
                                         GpuMemoryVendor::Amd,
                                         gpu == &amd_igpu));
            }
            return most_constrained(pools);
        }
        if (amd_igpu.available && amd_igpu.vram_gb > 0) {
            return pool_for(amd_igpu, "AMD iGPU", GpuMemoryVendor::Amd, true);
        }
        for (const auto& gpu : amd_dgpus) {
            if (gpu.available && gpu.vram_gb > 0)
                return pool_for(gpu, "AMD dGPU", GpuMemoryVendor::Amd);
        }
        return {};
    };
    auto select_nvidia = [&](bool apply_visibility) -> GpuMemoryPool {
        auto target = gpu_indices_for_target(device, "CUDA");
        if (target.targets_vendor && !target.valid) return {};
        std::vector<const GPUInfo*> visible_gpus;
        if (!apply_visibility || !cuda_visible_devices.has_value()) {
            for (const auto& gpu : nvidia_gpus) {
                if (gpu.available && gpu.vram_gb > 0) visible_gpus.push_back(&gpu);
            }
        } else {
            std::istringstream stream(*cuda_visible_devices);
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
                                         "NVIDIA CUDA" + std::to_string(index),
                                         GpuMemoryVendor::Nvidia));
            }
            return most_constrained(pools);
        }
        if (!visible_gpus.empty())
            return pool_for(*visible_gpus.front(), "NVIDIA", GpuMemoryVendor::Nvidia);
        return {};
    };
    auto select_metal = [&]() -> GpuMemoryPool {
        if (apple.available && apple.vram_gb > 0)
            return pool_for(apple, "Metal", GpuMemoryVendor::Metal);
        return {};
    };

    if (vendor == GpuMemoryVendor::Amd) return select_amd();
    if (vendor == GpuMemoryVendor::Nvidia) return select_nvidia(true);
    if (vendor == GpuMemoryVendor::Metal) return select_metal();

    auto pool = select_amd();
    if (pool.total_gb > 0) return pool;
    pool = select_nvidia(false);
    if (pool.total_gb > 0) return pool;
    return select_metal();
}

/// Count GPUs with usable VRAM, restricted to `vendor` (or across all vendors when
/// `vendor` is Any). Used to decide whether an unscoped or ambiguous device selection
/// is actually a problem — a single-GPU host has nothing else the ctx_size estimate
/// could be scoped to, so no warning is warranted there.
inline int count_gpu_candidates(GpuMemoryVendor vendor,
                                 const GPUInfo& amd_igpu,
                                 const std::vector<GPUInfo>& amd_dgpus,
                                 const std::vector<GPUInfo>& nvidia_gpus,
                                 const GPUInfo& apple) {
    int count = 0;
    if (vendor == GpuMemoryVendor::Any || vendor == GpuMemoryVendor::Amd) {
        if (amd_igpu.available && amd_igpu.vram_gb > 0) count++;
        for (const auto& gpu : amd_dgpus)
            if (gpu.available && gpu.vram_gb > 0) count++;
    }
    if (vendor == GpuMemoryVendor::Any || vendor == GpuMemoryVendor::Nvidia) {
        for (const auto& gpu : nvidia_gpus)
            if (gpu.available && gpu.vram_gb > 0) count++;
    }
    if (vendor == GpuMemoryVendor::Any || vendor == GpuMemoryVendor::Metal) {
        if (apple.available && apple.vram_gb > 0) count++;
    }
    return count;
}

enum class CtxScopeWarning { None, Unscoped, Ambiguous };

struct CtxMemoryScope {
    bool is_gpu = false;
    // True when `device` named a specific ordinal (e.g. "ROCm1"/"CUDA0"), so the
    // headroom estimate is unambiguously scoped to one physical GPU.
    bool per_device = false;
    // True when a backend/device string identified a GPU vendor at all (even without
    // an ordinal), e.g. "--llamacpp-backend rocm" with no explicit "ROCm<N>".
    bool device_named = false;
    // True when no device was named and more than one GPU is reachable, so the
    // "most constrained" pool picked below may not be the one the caller meant.
    bool ambiguous = false;
    int reachable_gpu_count = 0;
};

inline CtxScopeWarning classify_ctx_scope(const CtxMemoryScope& scope) {
    if (scope.is_gpu && !scope.per_device && scope.device_named && scope.reachable_gpu_count > 1)
        return CtxScopeWarning::Unscoped;
    if (scope.ambiguous) return CtxScopeWarning::Ambiguous;
    return CtxScopeWarning::None;
}

} // namespace lemon
