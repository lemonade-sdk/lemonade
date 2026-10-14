#pragma once

#include "locator.h"
#include "registry_id.h"

#include <optional>
#include <string>

namespace lemon {
namespace models {

// What a checkpoint is, apart from where its bytes sit: the join between a
// catalog entry and the cache layout.
struct ArtifactRef {
    // Set only when the bytes came from a pullable host.
    std::optional<RegistryId> registry;
    std::string repo_id;
    std::string variant;
    std::string revision;
    bool required = true;

    // Adds whether absence leaves the model incomplete; bytes from no host are
    // named by where they were found.
    static ArtifactRef from_locator(const Locator& locator, bool required);

    bool operator==(const ArtifactRef& other) const;
    bool operator!=(const ArtifactRef& other) const { return !(*this == other); }
};

// The key a source is built from: one checkpoint of one model.
struct CheckpointRef {
    std::string canonical_id;
    std::string checkpoint_type;
};

}  // namespace models
}  // namespace lemon
