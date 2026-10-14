#pragma once

#include "locator.h"
#include "registry_id.h"

#include <optional>
#include <string>

namespace lemon {
namespace models {

// What a checkpoint is, apart from where its bytes sit. This is the join between
// a catalog entry and the registry cache layout: blob naming, update checks and
// provenance files all key on it.
struct ArtifactRef {
    // Set only when the bytes came from a pullable host. A local file names no
    // host, so no default here: defaulting would claim provenance that is absent.
    std::optional<RegistryId> registry;
    std::string repo_id;
    std::string variant;
    std::string revision;
    bool required = true;

    // A locator says where bytes come from; the ref adds whether their absence
    // leaves the model incomplete. Bytes that never came from a host are named
    // by where they were found, so two files never share a ref.
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
