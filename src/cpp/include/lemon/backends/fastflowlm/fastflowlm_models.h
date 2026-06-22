#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace lemon {

struct ModelInfo;

namespace backends {
namespace fastflowlm {

// FLM-specific model-file helpers. FLM stores models under FLM_MODEL_PATH /
// platform-default roots and describes them with a config.json; this knowledge
// lives in the fastflowlm backend folder rather than in the shared model manager.

// Derive the on-disk repo directory name from an FLM model URL.
std::string repo_dir_from_url(const std::string& url);

// Locate config.json for an FLM repo dir across the candidate model roots.
std::filesystem::path find_flm_config_path_from_repo_dir(const std::string& repo_dir);

// Read the model's max context window from its FLM config.json (0 if unknown).
int64_t read_flm_max_context_window(const ModelInfo& info);

} // namespace fastflowlm
} // namespace backends
} // namespace lemon
