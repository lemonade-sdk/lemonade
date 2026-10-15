#pragma once

#include <lemon/models/artifact_fetch.h>
#include <lemon/models/registry_documents.h>
#include <lemon/models/registry_id.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lemon {

struct ModelInfo;

namespace models {

class ModelArtifactStore;

// Collection files name components by public name while a definition may carry the
// `user.` prefix from an export.
std::string bare_component_name(const std::string& name);

// False when the definition would fail partway through its own fetch.
bool collection_component_def_is_valid(const nlohmann::json& def);

// Pull and import settle through one path, so neither redefines "installed".
class ModelIngest : public ArtifactFetchService {
  public:
    struct Deps {
        const ModelArtifactStore* artifacts;
        // The server-wide default pull source; null when none is configured.
        std::function<std::optional<RegistryId>()> default_registry_source;

        // The published model set.
        std::function<bool(const std::string&)> model_exists;
        std::function<bool(const std::string&)> model_exists_unfiltered;
        std::function<ModelInfo(const std::string&)> model_info;
        std::function<std::string(const std::string&)> filter_reason;
        std::function<std::string(const std::string&)> resolve_name;
        std::function<bool(const std::string&)> is_downloaded;
        std::function<std::map<std::string, std::filesystem::path>(const std::string&)>
            resolved_paths;

        // Registration, and keeping the published set in step with it.
        std::function<void(const std::string&, const nlohmann::json&)> register_model;
        std::function<void(const std::string&)> unregister_model;
        std::function<void(const std::string&, bool)> patch_downloaded;
        std::function<void()> invalidate;
        std::function<void(const std::string&)> persist_size;
        std::function<RegistryDocuments::Pin()> pin;
        std::function<void(const ModelInfo&)> save_options;
        std::function<void(const std::string&)> model_updated;
    };

    explicit ModelIngest(Deps deps);

    RegistryId download_source_for(const ModelInfo& info) const override;

    void fetch_registry_artifacts(const ModelInfo& info,
                                  DownloadProgressCallback progress) override;

    // Why a `collection.omni` / `collection.router` definition cannot be registered,
    // in the words the caller returns; empty when it can.
    std::optional<std::string> validate_collection(const std::string& model_name,
                                                   const nlohmann::json& model_data);

    // Register a definition and its recipe options without fetching anything.
    void register_model(const std::string& model_name, const nlohmann::json& model_data,
                        bool allow_missing_checkpoint, bool replace_existing);

    // Register if needed, then fetch. A `collection.omni` entry resolves its
    // components and fetches each one.
    void pull(const std::string& model_name, const nlohmann::json& model_data,
              bool do_not_upgrade, DownloadProgressCallback progress);

    // Fetch an already-registered model, serialized per checkpoint repository.
    void fetch_model(const ModelInfo& info, bool do_not_upgrade,
                     DownloadProgressCallback progress);

    // Disk state only: no request, no registration.
    void populate_from_cached_manifest(ModelInfo& info);

  private:
    // Cycle-detecting form used by the collection fan-out. `visited` holds the
    // collection names entered on the current call chain; re-entering one throws.
    void pull(const std::string& model_name, const nlohmann::json& model_data,
              bool do_not_upgrade, DownloadProgressCallback progress,
              std::set<std::string>& visited, bool register_only,
              bool allow_missing_checkpoint, bool replace_existing);

    nlohmann::json fetch_collection_manifest(const std::string& repo_id,
                                             const std::string& registry_source,
                                             bool do_not_upgrade);

    // Known entries keep their local copy; unknown ones are added from what the
    // list itself carries. Returns canonical names in the order given.
    std::vector<std::string> register_components(const nlohmann::json& component_names,
                                                 const nlohmann::json& component_defs,
                                                 const std::string& registry_source);
    std::vector<std::string> resolve_components_from_manifest(const std::string& repo_id,
                                                              const std::string& registry_source,
                                                              bool do_not_upgrade);

    void download_from_registry(const ModelInfo& info,
                                DownloadProgressCallback progress_callback) const;
    void download_from_manifest(const nlohmann::json& manifest,
                                std::map<std::string, std::string>& headers,
                                DownloadProgressCallback progress_callback) const;

    const ModelArtifactStore& artifacts() const;

    Deps deps_;

    // Serializes downloads that write the same repository's `.partial` files.
    std::mutex download_locks_mutex_;
    std::map<std::string, std::shared_ptr<std::mutex>> download_locks_;
};

}  // namespace models
}  // namespace lemon
