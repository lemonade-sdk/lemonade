#pragma once

#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

#include "model_manager.h"

namespace lemon {

using json = nlohmann::json;

enum class MemoryBackendClass {
    CPU,
    GPU,
    NPU
};

struct SystemMemoryProbe {
    uint64_t total_bytes = 0;
    uint64_t available_bytes = 0;
    uint64_t ram_limit_bytes = 0;       // 0 means unlimited
    uint64_t effective_available_bytes = 0;
    std::string source;
};

struct ModelMemoryEstimate {
    uint64_t weight_bytes = 0;
    uint64_t overhead_bytes = 0;
    // Conservative minimum startup footprint before llama.cpp dynamic context
    // fitting. This intentionally includes model buffers, repack/runtime
    // overhead, multimodal sidecars, compute warmup, and only a small probe
    // context. It must be large enough to reject impossible loads without
    // hiding a full default/128k context inside the margin.
    uint64_t minimum_startup_required_bytes = 0;
    // Legacy alias for the startup footprint in the primary execution domain.
    // Kept for API compatibility with existing callers/log consumers.
    uint64_t base_required_bytes = 0;
    // Estimated startup footprint that counts against host/system RAM limits.
    // This is the value used for hard preflight checks and ram_limit eviction.
    uint64_t host_base_required_bytes = 0;
    // Estimated startup footprint that is expected to live in a separate device
    // memory domain such as dGPU VRAM.
    uint64_t device_base_required_bytes = 0;
    // Best-effort detected total memory of the selected separate device-memory
    // domain. 0 means unknown/not applicable. This is used only for deterministic
    // base-model rejections such as a 30 GiB model on an 8 GiB dGPU.
    uint64_t device_total_bytes = 0;
    uint64_t kv_cache_bytes_per_token = 0;
    // KV/cache bytes per token that count against host/system RAM. This equals
    // kv_cache_bytes_per_token for CPU/APU/unified-memory/NPU paths and is 0
    // for discrete GPU paths where llama.cpp's fit logic owns device memory.
    uint64_t host_kv_cache_bytes_per_token = 0;
    bool separate_device_memory = false;
    std::string memory_domain;
    int model_max_context = 0;
    int target_context = 0;
    int probe_context = 1024;
    int final_context = 0;
    bool dynamic_context = false;
    bool hard_error = false;
    bool restricted_context_warning = false;
    bool user_context_override = false;
    bool model_context_limit_is_trustworthy = false;
    std::string warning;
    std::string detail;

    uint64_t total_required_bytes() const;
    json to_json() const;
};

class MemoryManager {
public:
    static constexpr int kDefaultContextTarget = 131072;
    static constexpr int kProbeContext = 1024;
    static constexpr int kRestrictedContextWarningThreshold = 32768;

    // Config ram_limit is expressed in MiB. A value < 0 means unlimited.
    static uint64_t ram_limit_mib_to_bytes(int64_t ram_limit_mib);

    static SystemMemoryProbe probe_system_memory(int64_t ram_limit_mib = -1);

    static ModelMemoryEstimate estimate_llamacpp_memory(
        const ModelInfo& model_info,
        const std::string& gguf_path,
        MemoryBackendClass backend,
        int context_target,
        int64_t ram_limit_mib);

    static uint64_t estimate_loaded_model_bytes(const ModelMemoryEstimate& estimate);
    static ModelMemoryEstimate estimate_non_llamacpp_memory(
        const ModelInfo& model_info,
        MemoryBackendClass backend,
        int context_target,
        int64_t ram_limit_mib);
    static uint64_t estimate_non_llamacpp_loaded_bytes(const ModelInfo& model_info,
                                                       MemoryBackendClass backend);

    // Returns the GGUF training/context length when it can be read from metadata.
    // Returns 0 for non-GGUF files, missing files, or metadata that does not
    // expose a context_length key.
    static int get_llamacpp_model_max_context(const std::string& gguf_path);
    static std::string get_llamacpp_architecture(const std::string& gguf_path);

    static std::string backend_to_string(MemoryBackendClass backend);
    static std::string format_bytes(uint64_t bytes);

private:
    static uint64_t get_weight_size_bytes(const ModelInfo& model_info,
                                          const std::string& gguf_path);
    static uint64_t backend_overhead_bytes(uint64_t weight_bytes,
                                           MemoryBackendClass backend);
};

} // namespace lemon
