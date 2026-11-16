#include "lemon/models/download_markers.h"

#include <lemon/utils/path_utils.h>
#include <lemon/utils/safe_fs.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace lemon {
namespace models {

namespace {

// A pull writes its manifest at the snapshot root, which can sit above a checkpoint
// nested in the repository (Moonshine's onnx/tiny).
fs::path find_manifest(const fs::path& start) {
    for (fs::path dir = start; !dir.empty(); dir = dir.parent_path()) {
        const fs::path manifest = dir / ".download_manifest.json";
        if (utils::safe_exists(manifest)) return manifest;
        if (dir.parent_path().filename() == "snapshots" || dir == dir.parent_path()) break;
    }
    return {};
}

}  // namespace

std::string interrupted_download(const std::string& resolved_path) {
    if (resolved_path.empty()) return "";

    const fs::path resolved = utils::path_from_utf8(resolved_path);
    const bool directory = utils::safe_is_directory(resolved);

    const fs::path manifest = find_manifest(directory ? resolved : resolved.parent_path());
    if (!manifest.empty()) {
        return "download in progress: " + utils::path_to_utf8(manifest);
    }

    if (!directory) {
        return utils::safe_exists(utils::path_from_utf8(resolved_path + ".partial"))
                   ? "download in progress: " + resolved_path + ".partial"
                   : "";
    }
    return "";
}

}  // namespace models
}  // namespace lemon
