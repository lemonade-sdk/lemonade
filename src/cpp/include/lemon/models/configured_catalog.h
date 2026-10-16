#pragma once

#include <lemon/models/catalog.h>
#include <lemon/models/json_catalog.h>

#include <lemon/models/catalog_fetch_clock.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace lemon {
namespace models {

// A registry named in `config.json`, read from a file or fetched over HTTP.
struct ConfiguredCatalog {
    std::string id;
    std::string path;
    std::string url;
    bool required = false;
    bool allow_insecure_http = false;
};

// A refused entry costs its own catalog, not the set.
struct ParsedCatalogs {
    std::vector<ConfiguredCatalog> catalogs;
    std::vector<std::string> refusals;
};

// Throws when the value is not an array at all.
ParsedCatalogs parse_configured_catalogs(const nlohmann::json& value);

// Throws std::invalid_argument naming the first refused entry.
void reject_refused_catalogs(const nlohmann::json& value);

nlohmann::json configured_catalogs_to_config(const std::vector<ConfiguredCatalog>& catalogs);

// The catalog the record names. A fetched body is kept under `cache_dir`.
std::shared_ptr<ModelCatalog> make_configured_catalog(const ConfiguredCatalog& catalog,
                                                      const RegistryRowRules& rules,
                                                      const std::string& cache_dir,
                                                      std::shared_ptr<const CatalogFetchClock> clock = nullptr);

}  // namespace models
}  // namespace lemon
