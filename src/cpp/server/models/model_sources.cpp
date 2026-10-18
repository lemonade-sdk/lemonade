#include <lemon/models/registry_types.h>
#include <lemon/models/model_sources.h>
#include <lemon/models/registry_id.h>

#include <string>
#include <vector>

namespace lemon {
namespace models {

namespace {

json registry_json(const RegistryDescriptor& descriptor) {
    json env = json::array();
    for (const auto& var : descriptor.env) {
        env.push_back({{"name", var.name}, {"purpose", var.purpose}, {"required", var.required}});
    }
    return {
        {"token", descriptor.id.str()},
        {"aliases", descriptor.aliases},
        {"display_name", descriptor.display_name},
        {"title", descriptor.doc.title},
        {"summary", descriptor.doc.summary},
        {"notes", descriptor.doc.notes},
        {"example", descriptor.doc.example},
        {"endpoint_default", descriptor.endpoint_default},
        {"scheme", descriptor.scheme_prefix.empty() ? "" : descriptor.scheme_prefix + "://"},
        {"cache_dir_prefix", descriptor.cache_dir_prefix},
        {"default_revision", descriptor.default_revision},
        {"revision_semantics", descriptor.revision_semantics},
        {"staleness_basis", descriptor.staleness_basis},
        {"download_protocol", descriptor.download_protocol},
        {"search", descriptor.supports_search},
        {"env", std::move(env)},
    };
}

json catalog_json(const CatalogDescriptor& descriptor) {
    return {
        {"id", descriptor.id},
        {"title", descriptor.doc.title},
        {"summary", descriptor.doc.summary},
        {"notes", descriptor.doc.notes},
        {"example", descriptor.doc.example},
        {"required", descriptor.required},
        {"config_key", descriptor.config_key},
        {"host_specific", descriptor.host_specific}
    };
}

}  // namespace

json model_sources_json(const StoreSnapshot& snapshot) {
    json registries = json::array();
    json source_tokens = json::array();
    json pull_sources = json::array();
    for (const auto& descriptor : all_registry_descriptors()) {
        registries.push_back(registry_json(descriptor));
        source_tokens.push_back(descriptor.id.str());
        pull_sources.push_back(descriptor.id.str());
    }

    json catalogs = json::array();
    for (const auto& descriptor : snapshot.catalogs) {
        catalogs.push_back(catalog_json(descriptor));
    }

    json local_origins = json::array();
    for (const std::string& token : local_origin_tokens()) {
        local_origins.push_back(token);
        pull_sources.push_back(token);
    }

    return {
        {"registries", std::move(registries)},
        {"catalogs", std::move(catalogs)},
        {"source_tokens", std::move(source_tokens)},
        {"local_origin_tokens", std::move(local_origins)},
        {"pull_source_values", std::move(pull_sources)},
    };
}

}  // namespace models
}  // namespace lemon
