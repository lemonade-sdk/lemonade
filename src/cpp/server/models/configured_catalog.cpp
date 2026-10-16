#include <lemon/models/configured_catalog.h>

#include <lemon/models/catalog_descriptor.h>
#include <lemon/utils/path_utils.h>

#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace lemon {
namespace models {

namespace {

fs::path body_cache_path(const std::string& cache_dir, const std::string& id) {
    const std::string& root = cache_dir.empty() ? utils::get_cache_dir() : cache_dir;
    return fs::path(root) / "catalogs" / (id + ".json");
}

bool known_setting(const std::string& key) {
    static const std::array<const char*, 5> known{"id", "path", "url", "required",
                                                  "allow_insecure_http"};
    return std::any_of(known.begin(), known.end(),
                       [&key](const char* name) { return key == name; });
}

bool valid_id(const std::string& id) {
    if (id.empty() || id == "." || id == "..") {
        return false;
    }
    for (const char c : id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

std::string parse_entry(const nlohmann::json& entry, const std::string& what,
                        ConfiguredCatalog& catalog) {
    for (const char* key : {"id", "path", "url"}) {
        if (entry.contains(key) && !entry[key].is_string()) {
            return "'" + std::string(key) + "' must be a string";
        }
    }
    for (const char* key : {"required", "allow_insecure_http"}) {
        if (entry.contains(key) && !entry[key].is_boolean()) {
            return "'" + std::string(key) + "' must be a boolean";
        }
    }

    for (const auto& held : entry.items()) {
        if (!known_setting(held.key())) {
            return what + " has no setting named '" + held.key() + "'";
        }
    }
    catalog.id = entry.value("id", std::string());
    catalog.path = entry.value("path", std::string());
    catalog.url = entry.value("url", std::string());
    catalog.required = entry.value("required", false);
    catalog.allow_insecure_http = entry.value("allow_insecure_http", false);

    if (!valid_id(catalog.id)) {
        return what + " needs an id of letters, digits, '.', '_' or '-'";
    }
    const int named_sources = (catalog.path.empty() ? 0 : 1) + (catalog.url.empty() ? 0 : 1);
    if (named_sources != 1) {
        return what + " must name exactly one of 'path' or 'url'";
    }
    if (catalog.url.empty()) {
        return std::string();
    }

    if (catalog.url.find('@') != std::string::npos) {
        return what + " url must not carry credentials";
    }
    const bool https = catalog.url.rfind("https://", 0) == 0;
    const bool http = catalog.url.rfind("http://", 0) == 0;
    if (!https && !http) {
        return what + " url must start with http:// or https://";
    }
    if (http && !catalog.allow_insecure_http) {
        return what + " is a plaintext url and needs allow_insecure_http";
    }
    return std::string();
}

std::string describe_entry(const nlohmann::json& entry, size_t index) {
    if (entry.is_object() && entry.contains("id") && entry["id"].is_string()) {
        return "catalog '" + entry["id"].get<std::string>() + "'";
    }
    return "catalog at position " + std::to_string(index);
}

}  // namespace

ParsedCatalogs parse_configured_catalogs(const nlohmann::json& value) {
    if (!value.is_null() && !value.is_array()) {
        throw std::invalid_argument("'model_catalogs' must be an array");
    }

    ParsedCatalogs parsed;
    if (value.is_null()) {
        return parsed;
    }

    for (size_t index = 0; index < value.size(); ++index) {
        const nlohmann::json& entry = value[index];
        if (!entry.is_object()) {
            throw std::invalid_argument("every entry of 'model_catalogs' must be an object");
        }
        const std::string what = describe_entry(entry, index);
        ConfiguredCatalog catalog;
        const std::string refusal = parse_entry(entry, what, catalog);
        if (!refusal.empty()) {
            parsed.refusals.push_back(refusal);
            continue;
        }
        bool named_twice = false;
        for (const auto& held : parsed.catalogs) {
            if (held.id == catalog.id) {
                parsed.refusals.push_back("catalog id '" + catalog.id + "' is named twice");
                named_twice = true;
                break;
            }
        }
        if (!named_twice) {
            parsed.catalogs.push_back(std::move(catalog));
        }
    }
    return parsed;
}

void reject_refused_catalogs(const nlohmann::json& value) {
    const ParsedCatalogs parsed = parse_configured_catalogs(value);
    if (!parsed.refusals.empty()) {
        throw std::invalid_argument(parsed.refusals.front());
    }
}

nlohmann::json configured_catalogs_to_config(const std::vector<ConfiguredCatalog>& catalogs) {
    nlohmann::json value = nlohmann::json::array();
    for (const auto& catalog : catalogs) {
        nlohmann::json entry{{"id", catalog.id}};
        if (!catalog.path.empty()) {
            entry["path"] = catalog.path;
        }
        if (!catalog.url.empty()) {
            entry["url"] = catalog.url;
        }
        if (catalog.required) {
            entry["required"] = true;
        }
        if (catalog.allow_insecure_http) {
            entry["allow_insecure_http"] = true;
        }
        value.push_back(std::move(entry));
    }
    return value;
}

std::shared_ptr<ModelCatalog> make_configured_catalog(const ConfiguredCatalog& catalog,
                                                      const RegistryRowRules& rules,
                                                      const std::string& cache_dir) {
    std::shared_ptr<CatalogLocator> locator;
    const std::string source = !catalog.path.empty()
                                   ? "file " + utils::path_to_utf8(fs::path(catalog.path))
                                   : catalog.url;
    if (!catalog.path.empty()) {
        locator = file_catalog_locator(catalog.id, catalog.path);
    } else {
        HttpCatalogSource http;
        http.url = catalog.url;
        http.cache_path = body_cache_path(cache_dir, catalog.id).string();
        http.allow_insecure_http = catalog.allow_insecure_http;
        locator = http_catalog_locator(catalog.id, std::move(http));
    }

    RegistryRowRules catalog_rows = rules;
    catalog_rows.source_label = "configured model";

    return std::make_shared<JsonCatalog>(
        make_catalog_descriptor(
            catalog.id, catalog_rank::configured, Conflict::KeepExisting,
            DocEntry{catalog.id, "Configured: " + catalog.id,
                     "A model registry read from " + source + ".", {}, ""},
            "model_catalogs", true, catalog.required),
        std::move(locator), std::move(catalog_rows));
}

}  // namespace models
}  // namespace lemon
