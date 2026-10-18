#pragma once

#include "registry_id.h"

#include <optional>
#include <string>

namespace lemon {
namespace models {

// A ref is an address, nothing more: two refs that compare equal name the same bytes.
// Where those bytes land is a property of the source that reads them, not of the address,
// so placement — layout and sibling — lives beside the ref instead of inside it.
//
// repo_id is a repository id, or a path when no host holds the bytes.
struct ArtifactRef {
    std::optional<RegistryId> registry;  // set only when a host holds the bytes
    std::string checkpoint_type;
    std::string repo_id;
    std::string variant;
    std::string recipe;  // decides which files a repository provides
    std::string revision;
    bool required = true;

    bool operator==(const ArtifactRef& other) const;
    bool operator!=(const ArtifactRef& other) const { return !(*this == other); }
};

// One checkpoint of one model.
struct CheckpointRef {
    std::string canonical_id;
    std::string checkpoint_type;
};

}  // namespace models
}  // namespace lemon
