#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace lemon {
namespace backends {

std::string detect_vllm_quant_method(
    const std::filesystem::path& local_snapshot,
    const std::filesystem::path& repo_cache,
    bool offline,
    const std::function<std::string()>& fetch_config);

} // namespace backends
} // namespace lemon
