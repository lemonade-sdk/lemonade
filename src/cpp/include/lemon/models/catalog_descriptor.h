#pragma once

#include <lemon/models/doc_entry.h>

#include <string>

namespace lemon {
namespace models {

// What a catalog declares at construction.
struct CatalogDescriptor {
    std::string id;
    bool required = false;
    DocEntry doc;
    std::string config_key;  // empty when the catalog is not user-configurable
};

}  // namespace models
}  // namespace lemon
