#pragma once

#include <string>

namespace lemon {
namespace models {

struct Placement {
    std::string cache_dir;  // provider-qualified repository cache directory
    std::string revision;
    std::string sibling;  // the file to sit beside
};

// Where a checkpoint's bytes sit; consumers never branch on checkpoint type.
class ArtifactLayout {
  public:
    virtual ~ArtifactLayout() = default;

    virtual std::string directory(const Placement&) const = 0;

    // Empty when the name cannot be placed here.
    virtual std::string target(const Placement&, const std::string& path) const = 0;

    // Writes refs/main.
    virtual void activate(const Placement&) const = 0;
};

const ArtifactLayout& hf_hub_layout();
const ArtifactLayout& sibling_layout();

}  // namespace models
}  // namespace lemon
