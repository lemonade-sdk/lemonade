#pragma once

#include <string>

namespace lemon {
namespace models {

// Where a repository's bytes land, and how a staged revision becomes current.
// Consumers take the resolved layout and never branch on a checkpoint type.
struct Placement {
    std::string cache_dir;  // provider-qualified repository cache directory
    std::string revision;
    std::string sibling;  // SiblingLayout: the file the bytes must land beside
};

class ArtifactLayout {
  public:
    virtual ~ArtifactLayout() = default;

    // Directory a plan writes this placement's files to.
    virtual std::string directory(const Placement&) const = 0;

    // Final path of one repository-relative name. Empty means the name cannot be
    // placed here.
    virtual std::string target(const Placement&, const std::string& path) const = 0;

    // Records the placement's revision as the cache directory's current one.
    virtual void activate(const Placement&) const = 0;
};

const ArtifactLayout& hf_hub_layout();
const ArtifactLayout& sibling_layout();

}  // namespace models
}  // namespace lemon
