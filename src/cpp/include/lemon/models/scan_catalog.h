#pragma once

#include <lemon/models/model_info.h>

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace models {

inline constexpr const char extra_model_prefix[] = "extra.";
inline constexpr const char extra_model_recipe[] = "llamacpp";
inline constexpr const char extra_model_source[] = "extra_models_dir";

// Models an operator placed in the extra models directory.
class ScanCatalog {
  public:
    // A GGUF whose architecture the shipped llama.cpp builds cannot run is left
    // out. With `local_llamacpp` (llama.cpp comes from the operator's own build,
    // whose architectures are unknown) a file under chat/, embeddings/ or
    // reranking/ is kept anyway: putting it there declares it a model.
    ScanCatalog(std::string extra_models_dir, DeviceType device, bool local_llamacpp = false)
        : extra_models_dir_(std::move(extra_models_dir)), device_(device),
          local_llamacpp_(local_llamacpp) {}

    // The directory as the operator set it, scanned with the extra-model recipe's
    // own device.
    static ScanCatalog for_dir(const std::string& extra_models_dir, bool local_llamacpp = false);

    // registered_paths are cache directories another model already claims.
    std::map<std::string, ModelInfo> enumerate(
        const std::set<std::filesystem::path>& registered_paths) const;

  private:
    ModelInfo init_info(const std::string& name) const;

    void scan_directory(const std::filesystem::path& dir_path,
                        const std::vector<std::filesystem::path>& gguf_files,
                        std::map<std::string, ModelInfo>& discovered,
                        const std::filesystem::path& search_path,
                        std::set<std::string>& claimed_names) const;

    std::string extra_models_dir_;
    DeviceType device_;
    bool local_llamacpp_ = false;
};

}  // namespace models
}  // namespace lemon
