#pragma once

#include <lemon/models/catalog_fetch_clock.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
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
    // `observed` receives what the read saw, so the next read can ask
    // conditionally; invalidation itself is the caller's event, not a probe.
    virtual std::optional<nlohmann::json> read(CatalogState* observed) const = 0;
};

std::shared_ptr<CatalogLocator> file_catalog_locator(std::string id, std::string path);

// A document fetched over HTTP; `cache_path` is the body used when a fetch fails.
struct HttpCatalogSource {
    std::string url;
    std::string cache_path;
    bool allow_insecure_http = false;
    // Inside this window the cached body answers and the url is not asked.
    std::chrono::seconds fresh_for{0};
    std::shared_ptr<const CatalogFetchClock> clock;
    // A document this large is not a model registry. 0 accepts any size.
    std::size_t max_body_bytes = 0;
};

std::shared_ptr<CatalogLocator> http_catalog_locator(std::string id, HttpCatalogSource source);

}  // namespace models
}  // namespace lemon
