#pragma once

#include "registry_id.h"

#include <optional>
#include <string>

namespace lemon {
namespace models {

// A ref is an address, nothing more: two refs that compare equal name the same bytes.
// Where those bytes land, and which backend's file selection applies to them, is a
// property of the source that reads them rather than of the address, so placement —
// layout, sibling and recipe — lives beside the ref in SourcePlacement.
//
// checkpoint_type stays because it is still an address: it names a different set of bytes.
// A recipe names whose policy about them, which is why it moved.
//
// repo_id is a repository id, or a path when no host holds the bytes.
struct ArtifactRef {
    std::optional<RegistryId> registry;  // set only when a host holds the bytes
    std::string checkpoint_type;
    std::string repo_id;
    std::string variant;
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
