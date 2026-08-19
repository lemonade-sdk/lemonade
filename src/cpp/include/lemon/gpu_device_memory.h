#pragma once

#include <algorithm>
#include <cctype>
#include <lemon/system_info.h>
#include <string>
#include <vector>

namespace lemon {

// A GPU memory pool a model can be placed into, flattened from GPUInfo so the
// selection logic below stays independent of how each vendor reports memory
// (an APU's pool spans VRAM + GTT, a dGPU's does not).
struct GpuMemoryCandidate {
    int index = -1;          // Runtime ordinal, matching the N in "ROCm<N>"/"CUDA<N>".
    double total_gb = 0.0;
    double used_gb = -1.0;   // -1 when the platform cannot report per-device usage.
    std::string label;
};

struct GpuMemoryPool {
    bool resolved = false;
    // True when a pool was picked without the caller naming one, so the choice is
    // a conservative guess rather than the device the model will actually land on.
    bool ambiguous = false;
    double total_gb = 0.0;
    double used_gb = 0.0;
    std::string label;

    double free_gb() const { return (std::max)(0.0, total_gb - used_gb); }
};

enum class GpuVendor {
    Unknown,  // No vendor implied; consider every candidate.
    AMD,
    NVIDIA,
    // Vulkan enumerates across vendors, so a "Vulkan<N>" ordinal cannot be
    // attributed to any one vendor's pool list.
    Vulkan,
};

struct GpuDeviceSelection {
    GpuVendor vendor = GpuVendor::Unknown;
    std::vector<int> ordinals;
    // A device string that was given but could not be understood. Distinct from an
    // empty selection, which just means the caller named no device.
    bool malformed = false;
};

inline std::string gpu_to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Backend ids carry a channel suffix ("rocm-stable", "rocm-nightly"), so match on prefix.
inline GpuVendor gpu_vendor_from_backend(const std::string& backend) {
    const std::string lower = gpu_to_lower(backend);
    if (lower.rfind("rocm", 0) == 0) return GpuVendor::AMD;
    if (lower.rfind("cuda", 0) == 0) return GpuVendor::NVIDIA;
    if (lower.rfind("vulkan", 0) == 0) return GpuVendor::Vulkan;
    return GpuVendor::Unknown;
}

// Parse a llama.cpp --device string: a comma-separated list of "<Vendor><ordinal>"
// tokens, e.g. "ROCm1" or "CUDA0,CUDA1". Tokens must all name the same vendor.
inline GpuDeviceSelection parse_gpu_device_selection(const std::string& device_string) {
    GpuDeviceSelection selection;
    if (device_string.empty()) return selection;

    size_t pos = 0;
    bool any_token = false;
    while (pos <= device_string.size()) {
        const size_t comma = device_string.find(',', pos);
        const size_t end = (comma == std::string::npos) ? device_string.size() : comma;

        std::string token = device_string.substr(pos, end - pos);
        pos = end + 1;

        // Trim surrounding whitespace.
        const size_t first = token.find_first_not_of(" \t");
        const size_t last = token.find_last_not_of(" \t");
        token = (first == std::string::npos) ? "" : token.substr(first, last - first + 1);

        if (token.empty()) {
            selection.malformed = true;
            break;
        }
        any_token = true;

        const std::string lower = gpu_to_lower(token);
        GpuVendor vendor = GpuVendor::Unknown;
        size_t digits_at = 0;
        if (lower.rfind("rocm", 0) == 0) {
            vendor = GpuVendor::AMD;
            digits_at = 4;
        } else if (lower.rfind("cuda", 0) == 0) {
            vendor = GpuVendor::NVIDIA;
            digits_at = 4;
        } else if (lower.rfind("vulkan", 0) == 0) {
            vendor = GpuVendor::Vulkan;
            digits_at = 6;
        } else {
            selection.malformed = true;
            break;
        }

        if (selection.vendor != GpuVendor::Unknown && selection.vendor != vendor) {
            selection.malformed = true;  // Mixed vendors in one selection.
            break;
        }
        selection.vendor = vendor;

        const std::string digits = token.substr(digits_at);
        if (digits.empty() ||
            !std::all_of(digits.begin(), digits.end(),
                         [](unsigned char c) { return std::isdigit(c) != 0; })) {
            selection.malformed = true;
            break;
        }

        try {
            selection.ordinals.push_back(std::stoi(digits));
        } catch (...) {
            selection.malformed = true;
            break;
        }

        if (comma == std::string::npos) break;
    }

    if (!any_token) selection.malformed = true;
    if (selection.malformed) selection.ordinals.clear();
    return selection;
}

inline std::vector<GpuMemoryCandidate> amd_memory_candidates(
        const GPUInfo& igpu, const std::vector<GPUInfo>& dgpus) {
    std::vector<GpuMemoryCandidate> candidates;
    // An APU's usable pool is dedicated VRAM plus the GTT it can borrow from
    // system RAM; a dGPU is limited to its own VRAM.
    if (igpu.available && igpu.vram_gb > 0) {
        GpuMemoryCandidate c;
        c.index = igpu.index;
        c.total_gb = igpu.vram_gb + igpu.virtual_gb;
        c.used_gb = (igpu.vram_used_gb < 0 || igpu.virtual_used_gb < 0)
            ? -1.0
            : igpu.vram_used_gb + igpu.virtual_used_gb;
        c.label = "AMD iGPU";
        candidates.push_back(c);
    }
    for (const auto& gpu : dgpus) {
        if (!gpu.available || gpu.vram_gb <= 0) continue;
        GpuMemoryCandidate c;
        c.index = gpu.index;
        c.total_gb = gpu.vram_gb;
        c.used_gb = gpu.vram_used_gb;
        c.label = "AMD dGPU";
        candidates.push_back(c);
    }
    return candidates;
}

inline std::vector<GpuMemoryCandidate> nvidia_memory_candidates(const std::vector<GPUInfo>& gpus) {
    std::vector<GpuMemoryCandidate> candidates;
    for (const auto& gpu : gpus) {
        if (!gpu.available || gpu.vram_gb <= 0) continue;
        GpuMemoryCandidate c;
        c.index = gpu.index;
        c.total_gb = gpu.vram_gb;
        c.used_gb = gpu.vram_used_gb;
        c.label = "NVIDIA";
        candidates.push_back(c);
    }
    return candidates;
}

namespace detail {

inline GpuMemoryPool to_pool(const GpuMemoryCandidate& c, bool ambiguous) {
    GpuMemoryPool pool;
    // A candidate with no usage reading cannot answer "how much is free", which is
    // the only question this selection exists to answer.
    if (c.used_gb < 0 || c.total_gb <= 0) return pool;
    pool.resolved = true;
    pool.ambiguous = ambiguous;
    pool.total_gb = c.total_gb;
    pool.used_gb = c.used_gb;
    pool.label = c.label + " " + std::to_string(c.index);
    return pool;
}

// llama.cpp splits the KV cache across the devices it is given, but the split is
// not necessarily even, so size against the tightest one.
inline GpuMemoryPool most_constrained(const std::vector<GpuMemoryCandidate>& candidates,
                                      bool ambiguous) {
    const GpuMemoryCandidate* tightest = nullptr;
    for (const auto& c : candidates) {
        if (c.used_gb < 0 || c.total_gb <= 0) return GpuMemoryPool{};
        if (!tightest || (c.total_gb - c.used_gb) < (tightest->total_gb - tightest->used_gb)) {
            tightest = &c;
        }
    }
    if (!tightest) return GpuMemoryPool{};
    return to_pool(*tightest, ambiguous);
}

}  // namespace detail

/// Resolve the memory pool backing a model placement.
///
/// `device_string` is the llama.cpp --device selection ("ROCm1", "CUDA0,CUDA1"); when
/// it is empty the vendor implied by `backend` narrows the search. An unresolved pool
/// means "no trustworthy per-device answer" — callers must fall back rather than
/// treat it as an empty GPU.
inline GpuMemoryPool select_gpu_memory_pool(const std::vector<GpuMemoryCandidate>& amd,
                                            const std::vector<GpuMemoryCandidate>& nvidia,
                                            const std::string& backend,
                                            const std::string& device_string) {
    const GpuDeviceSelection selection = parse_gpu_device_selection(device_string);
    if (selection.malformed) return GpuMemoryPool{};

    GpuVendor vendor = selection.vendor;
    if (vendor == GpuVendor::Unknown) vendor = gpu_vendor_from_backend(backend);
    if (vendor == GpuVendor::Vulkan) return GpuMemoryPool{};

    if (!selection.ordinals.empty()) {
        const std::vector<GpuMemoryCandidate>& pool =
            (vendor == GpuVendor::NVIDIA) ? nvidia : amd;
        std::vector<GpuMemoryCandidate> chosen;
        for (int ordinal : selection.ordinals) {
            auto it = std::find_if(pool.begin(), pool.end(),
                                   [ordinal](const GpuMemoryCandidate& c) { return c.index == ordinal; });
            // A named device we cannot find must not silently become a different one.
            if (it == pool.end()) return GpuMemoryPool{};
            chosen.push_back(*it);
        }
        return detail::most_constrained(chosen, /*ambiguous=*/false);
    }

    // No device named: consider everything the backend could pick from.
    std::vector<GpuMemoryCandidate> reachable;
    if (vendor == GpuVendor::AMD || vendor == GpuVendor::Unknown)
        reachable.insert(reachable.end(), amd.begin(), amd.end());
    if (vendor == GpuVendor::NVIDIA || vendor == GpuVendor::Unknown)
        reachable.insert(reachable.end(), nvidia.begin(), nvidia.end());

    if (reachable.empty()) return GpuMemoryPool{};
    if (reachable.size() == 1) return detail::to_pool(reachable.front(), /*ambiguous=*/false);
    return detail::most_constrained(reachable, /*ambiguous=*/true);
}

/// How trustworthy the headroom figure behind an auto-tuned ctx_size is.
enum class CtxScopeWarning {
    None,
    // A device was named but its memory could not be scoped, so the figure mixes one
    // card's capacity with the machine's busiest-card usage.
    Unscoped,
    // No device was named and several could be chosen, so the tightest one was assumed.
    Ambiguous,
};

struct CtxMemoryScope {
    bool is_gpu = false;
    bool per_device = false;
    bool ambiguous = false;
    bool device_named = false;
};

inline CtxScopeWarning classify_ctx_scope(const CtxMemoryScope& scope) {
    if (scope.is_gpu && !scope.per_device && scope.device_named) return CtxScopeWarning::Unscoped;
    if (scope.ambiguous) return CtxScopeWarning::Ambiguous;
    return CtxScopeWarning::None;
}

}  // namespace lemon
