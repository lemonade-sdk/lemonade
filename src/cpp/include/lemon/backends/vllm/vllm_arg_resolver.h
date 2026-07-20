#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace lemon {
namespace backends {

struct VLLMArgResolution {
    std::vector<std::string> args;
    bool has_memory_budget_arg = false;
    // True when the user/family already supplied --dtype, so backend code
    // should not force its own (e.g. the AWQ float16 default).
    bool has_dtype_arg = false;
    // True when the user asked for eager. The resolver has already removed the flag from
    // `args` — load() re-emits it from the launch policy.
    bool has_enforce_eager = false;
    bool has_quantization_arg = false;
    std::string quantization_arg;
    // Structured rather than a passthrough flag: the `vllm_args` tokenizer strips quotes
    // and would corrupt inline JSON. The backend re-serializes it.
    std::string speculative_config;
};

VLLMArgResolution resolve_vllm_args(const std::string& model_name,
                                    const std::string& checkpoint,
                                    const nlohmann::json& config,
                                    const std::string& user_vllm_args);

// Discrete-HBM datacenter GPUs (AMD Instinct, gfx9xx) get vLLM's native memory budgeting
// and graph capture; APUs and consumer GPUs keep the conservative launch defaults. This
// predicate is the seam a future memory planner replaces.
bool is_discrete_hbm_arch(const std::string& arch);

struct DeviceClassLaunchPolicy {
    bool enforce_eager;     // push --enforce-eager (disables CUDA-graph capture)
    bool force_awq_kernel;  // force the 'awq' kernel (+ float16) for AWQ models
    bool cap_kv_cache;      // push the fixed --kv-cache-memory-bytes cap
};

DeviceClassLaunchPolicy device_class_launch_policy(const std::string& arch,
                                                   bool has_memory_budget_arg,
                                                   bool has_enforce_eager = false);

} // namespace backends
} // namespace lemon
