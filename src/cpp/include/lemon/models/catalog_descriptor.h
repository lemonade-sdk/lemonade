#pragma once

#include <lemon/models/doc_entry.h>

#include <string>

namespace lemon {
namespace models {

struct CatalogDescriptor {
    std::string id;
    bool required = false;
    DocEntry doc;
    std::string config_key;
    // Installed backends and providers decide which of these exist, so a table
    // committed to the repo cannot list them.
    bool host_specific = false;
};

}  // namespace models
}  // namespace lemon
