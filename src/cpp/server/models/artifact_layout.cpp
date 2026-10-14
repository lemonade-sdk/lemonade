#include <lemon/models/artifact_layout.h>

#include <lemon/backends/hf_cache_util.h>
#include <lemon/utils/path_utils.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace lemon {
namespace models {

namespace {

using lemon::utils::path_from_utf8;
using lemon::utils::path_to_utf8;

// refs/main, snapshots/<revision>/ — docs/dev/model-registries.md.
class HfHubLayout : public ArtifactLayout {
  public:
    std::string directory(const Placement& placement) const override {
        if (placement.cache_dir.empty() || placement.revision.empty()) {
            return "";
        }
        return path_to_utf8(path_from_utf8(placement.cache_dir) / "snapshots" /
                            placement.revision);
    }

    std::string target(const Placement& placement, const std::string& path) const override {
        if (path.empty() || fs::path(path).is_absolute()) {
            return "";
        }
        const std::string dir = directory(placement);
        return dir.empty() ? "" : path_to_utf8(path_from_utf8(dir) / path_from_utf8(path));
    }

    void activate(const Placement& placement) const override {
        if (placement.cache_dir.empty()) {
            return;
        }
        backends::hf_cache::write_ref_main(path_from_utf8(placement.cache_dir), placement.revision);
    }
};

// The file lands next to the checkpoint it belongs to, which is the one shape
// that can leave the model directory, so only the final component is honored.
class SiblingLayout : public ArtifactLayout {
  public:
    std::string directory(const Placement& placement) const override {
        if (placement.sibling.empty()) {
            return "";
        }
        return path_to_utf8(path_from_utf8(placement.sibling).parent_path());
    }

    std::string target(const Placement& placement, const std::string& path) const override {
        const std::string dir = directory(placement);
        if (dir.empty() || path.empty()) {
            return "";
        }
        const fs::path parent = path_from_utf8(dir);
        const fs::path target = parent / path_from_utf8(path).filename();
        return target.parent_path() == parent ? path_to_utf8(target) : "";
    }

    void activate(const Placement&) const override {}
};

}  // namespace

const ArtifactLayout& hf_hub_layout() {
    static const HfHubLayout layout;
    return layout;
}

const ArtifactLayout& sibling_layout() {
    static const SiblingLayout layout;
    return layout;
}

}  // namespace models
}  // namespace lemon
