#pragma once

#include <lemon/models/doc_entry.h>

#include <string>
#include <utility>

namespace lemon {
namespace models {

enum class Conflict { KeepExisting, Overwrite };

namespace catalog_rank {
// A lower rank enumerates first. The gaps leave room for a catalog that joins later.
inline constexpr int builtin_registry = 0;
inline constexpr int registrations = 10;
inline constexpr int configured = 15;
inline constexpr int scan = 20;
inline constexpr int backend_discovery = 30;
inline constexpr int cloud_listing = 40;
}  // namespace catalog_rank

struct CatalogDescriptor {
    std::string id;
    bool required = false;
    DocEntry doc;
    std::string config_key;
    // Installed backends and providers decide which of these exist, so a table
    // committed to the repo cannot list them.
    bool host_specific = false;
    // Precedence is declared here rather than inherited from a construction order:
    // a lower rank enumerates first, and `claim` decides whether it can take a
    // name an earlier catalog already has.
    int rank = catalog_rank::scan;
    Conflict claim = Conflict::KeepExisting;
};

inline CatalogDescriptor make_catalog_descriptor(std::string id, int rank, Conflict claim,
                                                 DocEntry doc, std::string config_key = {},
                                                 bool host_specific = false,
                                                 bool required = false) {
    CatalogDescriptor descriptor;
    descriptor.id = std::move(id);
    descriptor.required = required;
    descriptor.doc = std::move(doc);
    descriptor.config_key = std::move(config_key);
    descriptor.host_specific = host_specific;
    descriptor.rank = rank;
    descriptor.claim = claim;
    return descriptor;
}

}  // namespace models
}  // namespace lemon
