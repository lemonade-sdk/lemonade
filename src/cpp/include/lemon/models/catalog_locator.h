#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace lemon {
namespace models {

// What a locator observed at its last read. A rebuild compares this instead of
// parsing the document again.
struct CatalogState {
    bool present = false;
    std::uint64_t size = 0;
    // Same clock the directory watcher compares against.
    std::int64_t modified = 0;
    std::string etag;
    std::string last_modified;
};

// Where a JSON catalog document comes from and how it says it changed. A body
// that is simply not there reads as nullopt; whether that is fatal belongs to the
// catalog's `required()`, not to the locator.
class CatalogLocator {
  public:
    virtual ~CatalogLocator() = default;

    virtual const std::string& id() const = 0;

    // Throws when a body exists but is not readable as a document of model rows.
    virtual std::optional<nlohmann::json> read(CatalogState* observed) const = 0;

    // True when the body differs from the one that produced `known`. A poll, so
    // it must not cost more than the answer is worth.
    virtual bool changed(const CatalogState& known) const = 0;
};

std::shared_ptr<CatalogLocator> file_catalog_locator(std::string id, std::string path);

// A document fetched over HTTP. `cache_path` holds the last good body, which is
// what turns an unreachable host into a stale catalog rather than an empty one.
struct HttpCatalogSource {
    std::string url;
    std::string cache_path;
    // A credential belongs here: a URL lands in config.json, a header does not.
    std::map<std::string, std::string> headers;
    // A plaintext url is refused unless this is set; https never needs it.
    bool allow_insecure_http = false;
};

std::shared_ptr<CatalogLocator> http_catalog_locator(std::string id, HttpCatalogSource source);

}  // namespace models
}  // namespace lemon
