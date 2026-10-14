#pragma once

#include "artifact_layout.h"
#include "locator.h"
#include "registry_id.h"

#include <optional>
#include <string>

namespace lemon {
namespace models {

// repo_id is a repository id, or a path when no host holds the bytes.
struct ArtifactRef {
    std::optional<RegistryId> registry;  // set only when a host holds the bytes
    std::string checkpoint_type;
    std::string repo_id;
    std::string variant;
    std::string recipe;  // decides which files a repository provides
    std::string revision;
    bool required = true;

    // see ArtifactLayout; null means the snapshot layout
    const ArtifactLayout* layout = nullptr;
    std::string sibling;

    static ArtifactRef from_locator(const Locator& locator, bool required);

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
