#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "lemon/model_manager.h"  // ModelInfo, DownloadProgressCallback

namespace lemon {

namespace backends {
namespace fastflowlm {

// Locate the FLM executable (install dir on Windows, system PATH on Linux).
std::string find_flm_binary();

// Installed FLM model checkpoints (from `flm list --filter installed`).
std::vector<std::string> flm_installed_checkpoints();

// Discover all available FLM models (from `flm list --json`), each with its
// downloaded status set. Returns empty if FLM is not ready.
std::vector<ModelInfo> flm_discover_models();

// FLM-specific model-file helpers. FLM stores models under FLM_MODEL_PATH /
// platform-default roots and describes them with a config.json.

// Derive the on-disk repo directory name from an FLM model URL.
std::string repo_dir_from_url(const std::string& url);

// Locate config.json for an FLM repo dir across the candidate model roots.
std::filesystem::path find_flm_config_path_from_repo_dir(const std::string& repo_dir);

// Read the model's max context window from its FLM config.json (0 if unknown).
int64_t read_flm_max_context_window(const ModelInfo& info);

// Locate a user-managed flm on the system PATH ("" if not found).
std::string find_flm_in_path();

// Locate the flm executable: config override, PATH, then install dir ("" if not found).
std::string find_flm_executable();

// Run `flm validate` and report readiness; error_message on failure.
bool run_flm_validate(const std::string& flm_path, std::string& error_message);

// Detect the installed FLM version via `flm version` ("unknown" if unavailable).
std::string flm_version();

// Download (pull) an FLM model by checkpoint via the `flm` CLI.
void flm_download(const std::string& checkpoint, bool do_not_upgrade,
                  DownloadProgressCallback progress_callback);

// Remove an installed FLM model by checkpoint via `flm remove`; throws on failure.
void flm_remove(const std::string& checkpoint);

} // namespace fastflowlm
} // namespace backends
} // namespace lemon
