#include <lemon/models/json_catalog.h>

#include <lemon/canonical_id.h>
#include <lemon/models/registry_types.h>
#include <lemon/models/checkpoint_grammar.h>
#include <lemon/utils/aixlog.hpp>
#include <lemon/utils/json_utils.h>

#include <set>
#include <stdexcept>

namespace lemon {
namespace models {

void parse_model_source_fields(ModelInfo& info, const json& row) {
    const std::string raw_source = utils::JsonUtils::get_or_default<std::string>(row, "source", "");
    const std::string explicit_registry =
        utils::JsonUtils::get_or_default<std::string>(row, "registry_source", "");

    std::string normalized_explicit;
    if (!explicit_registry.empty()) {
        normalized_explicit =
            remote_registry_source_name(parse_remote_registry_source(explicit_registry));
        info.registry_source = normalized_explicit;
    }
    if (is_remote_registry_source(raw_source)) {
        const std::string normalized_public =
            remote_registry_source_name(parse_remote_registry_source(raw_source));
        if (!normalized_explicit.empty() && normalized_explicit != normalized_public) {
            throw std::invalid_argument(
                "Model source and registry_source identify different registries");
        }
        info.registry_source = normalized_public;
        info.source.clear();
    } else {
        info.source = raw_source;
    }
}

std::string legacy_mmproj_to_checkpoint(std::string main, std::string mmproj) {
    return repo_id_from_checkpoint(main) + ":" + mmproj;
}

void parse_checkpoints(ModelInfo& info, const json& row) {
    info.checkpoints["main"] = utils::JsonUtils::get_or_default<std::string>(row, "checkpoint", "");

    const std::string mmproj = utils::JsonUtils::get_or_default<std::string>(row, "mmproj", "");
    if (!mmproj.empty()) {
        info.checkpoints["mmproj"] = legacy_mmproj_to_checkpoint(
            utils::JsonUtils::get_or_default<std::string>(row, "checkpoint", ""), mmproj);
    }

    if (row.contains("checkpoints") && row["checkpoints"].is_object()) {
        for (const auto& [key, value] : row["checkpoints"].items()) {
            if (!value.is_string()) {
                throw std::invalid_argument("checkpoint '" + key + "' is not a string");
            }
            info.checkpoints[key] = value.get<std::string>();
        }
    }
}

void parse_components(ModelInfo& info, const json& row) {
    if (!row.contains("components") || !row["components"].is_array()) {
        return;
    }
    for (const auto& component : row["components"]) {
        if (component.is_string()) {
            info.components.push_back(component.get<std::string>());
        }
    }
}

void parse_image_defaults(ModelInfo& info, const json& row) {
    if (!row.contains("image_defaults") || !row["image_defaults"].is_object()) {
        return;
    }
    const auto& img_defaults = row["image_defaults"];
    info.image_defaults.has_defaults = true;
    info.image_defaults.steps = utils::JsonUtils::get_or_default<int>(img_defaults, "steps", 20);
    info.image_defaults.cfg_scale = utils::JsonUtils::get_or_default<float>(img_defaults, "cfg_scale", 7.0f);
    info.image_defaults.width = utils::JsonUtils::get_or_default<int>(img_defaults, "width", 512);
    info.image_defaults.height = utils::JsonUtils::get_or_default<int>(img_defaults, "height", 512);
    info.image_defaults.sampling_method =
        utils::JsonUtils::get_or_default<std::string>(img_defaults, "sampling_method", "");
    info.image_defaults.flow_shift =
        utils::JsonUtils::get_or_default<float>(img_defaults, "flow_shift", 0.0f);
    const auto edit_cap = img_defaults.find("soft_pixel_cap");
    if (edit_cap != img_defaults.end() && edit_cap->is_number_unsigned()) {
        info.image_defaults.soft_pixel_cap = edit_cap->get<std::uint64_t>();
    }
}

void parse_extras(ModelInfo& info, const json& row) {
    static const std::set<std::string> kKnownKeys = {
        "checkpoint",   "checkpoints",     "components",   "mmproj",  "recipe",
        "suggested",    "source",          "registry_source", "size", "cloud_provider",
        "labels",       "image_defaults",  "recipe_options"};
    if (!row.is_object()) {
        return;
    }
    for (const auto& [key, value] : row.items()) {
        if (kKnownKeys.count(key) == 0) {
            info.extras[key] = value;
        }
    }
}

std::optional<RegistryRowRef> find_registry_row(const RegistryDocuments::Pin& pin,
                                                const std::string& requested) {
    // Built-ins are keyed bare.
    if (pin.contains(RegistryDocument::Builtin, requested)) {
        return RegistryRowRef{RegistryDocument::Builtin, requested, requested};
    }
    const auto canonical = parse_canonical_id(requested);
    if (!canonical) {
        return std::nullopt;
    }
    if (canonical->source == ModelSource::Builtin &&
        pin.contains(RegistryDocument::Builtin, canonical->bare_name)) {
        return RegistryRowRef{RegistryDocument::Builtin, canonical->bare_name,
                              canonical->bare_name};
    }
    if (canonical->source == ModelSource::Registered &&
        pin.contains(RegistryDocument::User, canonical->bare_name)) {
        return RegistryRowRef{RegistryDocument::User, canonical->bare_name,
                              canonical_id(ModelSource::Registered, canonical->bare_name)};
    }
    return std::nullopt;
}

std::optional<ModelInfo> parse_registry_row(const std::string& key, const json& row,
                                            const RegistryRowRules& rules) {
    ModelInfo info;
    info.model_name = rules.name_prefix + key;
    parse_checkpoints(info, row);
    parse_components(info, row);
    info.recipe = utils::JsonUtils::get_or_default<std::string>(row, "recipe", "");
    info.suggested =
        utils::JsonUtils::get_or_default<bool>(row, "suggested", rules.suggested_default);
    try {
        parse_model_source_fields(info, row);
    } catch (const std::exception& e) {
        LOG(ERROR, "ModelManager") << "Skipping invalid " << rules.source_label << " '"
                                   << info.model_name << "': " << e.what() << std::endl;
        return std::nullopt;
    }
    info.size = utils::JsonUtils::get_or_default<double>(row, "size", 0.0);
    info.min_resident_gb = utils::JsonUtils::get_or_default<double>(row, "min_resident_gb", 0.0);
    info.cloud_provider = utils::JsonUtils::get_or_default<std::string>(row, "cloud_provider", "");
    info.system_prompt = utils::JsonUtils::get_or_default<std::string>(row, "system_prompt", "");
    if (row.contains("auto_update") && row["auto_update"].is_boolean()) {
        info.auto_update = row["auto_update"].get<bool>();
    }

    // A repo pointer means the cached manifest holds the components, not this row.
    if (is_model_collection_recipe(info.recipe) &&
        (info.components.empty() || !info.checkpoint().empty())) {
        info.components.clear();
        if (rules.populate_components) {
            rules.populate_components(info);
        }
    }

    if (row.contains("labels") && row["labels"].is_array()) {
        for (const auto& label : row["labels"]) {
            info.labels.push_back(label.get<std::string>());
        }
    }

    // A row whose mode claims contradict its recipe costs the row, never the set.
    if (rules.stamp_labels) {
        const std::string illegal = rules.stamp_labels(info);
        if (!illegal.empty()) {
            LOG(ERROR, "ModelManager") << "Skipping " << illegal << std::endl;
            return std::nullopt;
        }
    }

    parse_image_defaults(info, row);
    parse_extras(info, row);

    if (rules.recipe_options) {
        const json authored = row_recipe_options(row);
        if (!authored.is_null()) {
            rules.recipe_options(info.model_name, authored);
        }
    }

    info.type = get_model_type_from_labels(info.labels);
    if (rules.device_for_recipe) {
        info.device = rules.device_for_recipe(info.recipe);
    }

    // An unresolvable path leaves the model listed and not-downloaded, which is
    // what a reader needs in order to say why it cannot run.
    if (rules.resolve_paths) {
        try {
            rules.resolve_paths(info);
        } catch (const std::exception& e) {
            LOG(ERROR, "ModelManager")
                << "  EXCEPTION resolving '" << info.model_name << "': " << e.what() << std::endl;
        }
    }

    return info;
}

json row_recipe_options(const json& row) {
    if (!row.is_object() || !row.contains("recipe_options") ||
        !row["recipe_options"].is_object()) {
        return json(nullptr);
    }
    return row["recipe_options"];
}

JsonCatalog::JsonCatalog(CatalogDescriptor descriptor,
                         std::shared_ptr<const CatalogLocator> locator, RegistryRowRules rules)
    : descriptor_(std::move(descriptor)),
      locator_(std::move(locator)),
      rules_(std::move(rules)) {}

// One unreadable document costs its own models, unless it declared it could not.
json JsonCatalog::read_located() const {
    try {
        const std::optional<json> body = locator_->read(nullptr);
        if (!body) {
            if (descriptor_.required) {
                throw std::runtime_error("catalog '" + descriptor_.id +
                                         "' has no document to read");
            }
            return json::object();
        }
        return *body;
    } catch (const std::exception& e) {
        if (descriptor_.required) {
            throw;
        }
        LOG(ERROR, "ModelManager") << "Catalog '" << descriptor_.id
                                   << "' contributed nothing: " << e.what() << std::endl;
        return json::object();
    }
}

JsonCatalog::JsonCatalog(CatalogDescriptor descriptor, std::shared_ptr<const json> document,
                         RegistryRowRules rules)
    : descriptor_(std::move(descriptor)),
      document_(std::move(document)),
      rules_(std::move(rules)) {}

void JsonCatalog::enumerate(CatalogAccumulator& accumulator) const {
    // A pinned document is enumerated where it lives; only a located body lands
    // in this scope.
    json located;
    const json* document = nullptr;
    if (locator_) {
        located = read_located();
        document = &located;
    } else if (document_) {
        document = document_.get();
    } else {
        return;
    }

    for (const auto& [key, row] : document->items()) {
        std::optional<ModelInfo> info;
        try {
            info = parse_registry_row(key, row, rules_);
        } catch (const std::exception& e) {
            // Nothing here promises a well-typed document: a hand-edited file, or
            // one served by someone else. A row that cannot be read costs its own
            // model, and the build goes on with the rows that can.
            LOG(ERROR, "ModelManager") << "Skipping invalid " << rules_.source_label << " '"
                                       << key << "': " << e.what() << std::endl;
            continue;
        }
        if (!info) {
            continue;
        }
        accumulator.add(CatalogEntry{descriptor_.id, std::move(*info)}, descriptor_.claim);
    }
}

}  // namespace models
}  // namespace lemon
