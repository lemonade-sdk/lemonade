#include "lemon/models/download_markers.h"

#include <lemon/utils/path_utils.h>
#include <lemon/utils/safe_fs.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace lemon {
namespace models {

namespace {

// Non-recursive scan for .partial markers to confirm folder integrity.
bool has_partial_files(const fs::path& dir) {
    std::error_code ec;
    if (!utils::safe_is_directory(dir)) return false;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".partial") {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string interrupted_download(const std::string& resolved_path) {
    if (resolved_path.empty()) return "";

    const fs::path resolved = utils::path_from_utf8(resolved_path);
    const bool directory = utils::safe_is_directory(resolved);

    const fs::path manifest =
        (directory ? resolved : resolved.parent_path()) / ".download_manifest.json";
    if (utils::safe_exists(manifest)) {
        return "download in progress: " + utils::path_to_utf8(manifest);
    }

    if (!directory) {
        return utils::safe_exists(utils::path_from_utf8(resolved_path + ".partial"))
                   ? "download in progress: " + resolved_path + ".partial"
                   : "";
    }

    return has_partial_files(resolved) ? "download in progress: a .partial inside " +
                                             resolved_path
                                       : "";
}

}  // namespace models
}  // namespace lemon
