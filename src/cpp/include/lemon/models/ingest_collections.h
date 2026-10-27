#pragma once

#include <lemon/models/ingest.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lemon {
namespace models {

// Collections: the manifest a registry-backed collection points at, the components it
// names, and whether a definition may be registered at all. ModelIngest forwards these;
// the rules live here, together, because they all read the same manifest.
class IngestCollections {
  public:
    // `fetch` is the ingest that owns this unit: a collection pulls its own components
    // through the same path a plain model does, and registers them the same way.
    IngestCollections(const ModelIngest::Deps& deps, ModelIngest& fetch);

    // Why a `collection.omni` / `collection.router` definition cannot be registered,
    // in the words the caller returns; empty when it can.
    std::optional<std::string> validate(const std::string& model_name,
                                        const nlohmann::json& model_data);

    // Known entries keep their local copy; unknown ones are added from what the list
    // itself carries. Returns canonical names in the order given.
    std::vector<std::string> register_components(const nlohmann::json& component_names,
                                                 const nlohmann::json& component_defs,
                                                 const std::string& registry_source);

    // Fetches the manifest, then registers what it names.
    std::vector<std::string> resolve_from_manifest(const std::string& repo_id,
                                                   const std::string& registry_source,
                                                   bool do_not_upgrade);

    // Fills a slim registry entry from the collection manifest already on disk.
    void populate_from_cached_manifest(ModelInfo& info);

  private:
    nlohmann::json fetch_manifest(const std::string& repo_id, const std::string& registry_source,
                                  bool do_not_upgrade);

    const ModelIngest::Deps& deps_;
    ModelIngest& fetch_;
};

}  // namespace models
}  // namespace lemon
