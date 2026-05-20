#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace lemon {
namespace backends {

struct VLLMArgResolution {
    std::vector<std::string> args;
    bool user_has_memory_budget_arg = false;
};

VLLMArgResolution resolve_vllm_args(const std::string& model_name,
                                    const std::string& checkpoint,
                                    const nlohmann::json& config,
                                    const std::string& user_vllm_args);

} // namespace backends
} // namespace lemon
