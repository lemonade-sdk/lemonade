#pragma once

#include <lemon/model_manager.h>
#include <lemon/models/catalog.h>

#include <nlohmann/json.hpp>
#include <functional>
#include <optional>
#include <string>

namespace lemon {
namespace models {

using json = nlohmann::json;

// A row's `source` / `registry_source` pair, normalized into info.source and
// info.registry_source. Throws when the two name different hosts.
void parse_model_source_fields(ModelInfo& info, const json& row);

// The checkpoint a legacy `mmproj` key stands for, in repo:variant form.
std::string legacy_mmproj_to_checkpoint(std::string main, std::string mmproj);

// `checkpoint`, `checkpoints` and the legacy `mmproj` key, in that order: an
// explicit checkpoints entry wins over one derived from mmproj.
void parse_checkpoints(ModelInfo& info, const json& row);

void parse_components(ModelInfo& info, const json& row);

void parse_image_defaults(ModelInfo& info, const json& row);

// Keys no typed field consumed, kept so a backend can read custom per-model
// fields in load(). Keep the known-key set in step with what this file parses.
void parse_extras(ModelInfo& info, const json& row);

// Null when the row carries no `recipe_options` block.
nlohmann::json row_recipe_options(const json& row);

// The facts a document cannot answer about its own rows.
struct RegistryRowRules {
    // Minted onto every row key: "user." for registrations, "" for built-ins.
    std::string name_prefix;
    // `suggested` when the row omits it.
    bool suggested_default = false;
    // Names the document wherever a refused row is logged.
    std::string source_label;

    std::function<DeviceType(const std::string& recipe)> device_for_recipe;

    // Stamps the recipe's deployment label. Returns the reason the row's labels
    // are illegal, empty when they are not.
    std::function<std::string(ModelInfo& info)> stamp_labels;

    std::function<void(ModelInfo& info)> populate_components;
    std::function<void(ModelInfo& info)> resolve_paths;

    // Reports each row's authored `recipe_options` block; the caller folds it.
    std::function<void(const std::string& name, const json& authored)> recipe_options;
};

// One registry row into the model it defines. Returns nothing when the row must
// not become a model, and logs why: a bad row costs one model, never the set.
std::optional<ModelInfo> parse_registry_row(const std::string& key, const json& row,
                                            const RegistryRowRules& rules);

// A JSON document whose keys name models. Its owner hands over a pinned
// document, so a build enumerates one version however the owner writes meanwhile.
class JsonCatalog : public ModelCatalog {
  public:
    JsonCatalog(CatalogDescriptor descriptor, std::shared_ptr<const json> document,
                RegistryRowRules rules);

    const CatalogDescriptor& descriptor() const override { return descriptor_; }

    void enumerate(CatalogAccumulator& accumulator) const override;

  private:
    CatalogDescriptor descriptor_;
    std::shared_ptr<const json> document_;
    RegistryRowRules rules_;
};

}  // namespace models
}  // namespace lemon
