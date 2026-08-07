#include "lemon/backends/hf_cache_util.h"

#include <fstream>

#include "lemon/model_registry.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace backends {
namespace hf_cache {

bool exists(const fs::path& p) {
#ifdef _WIN32
    // The HF cache uses symlinks for dedup; MSVC's std::filesystem refuses
    // "untrusted" reparse points when the token lacks symlink privilege, so use
    // the Win32 API which has no such restriction.
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    std::error_code ec;
    return fs::exists(p, ec);
#endif
}

fs::directory_options dir_options() {
#ifdef _WIN32
    return fs::directory_options::skip_permission_denied;
#else
    return fs::directory_options::none;
#endif
}

namespace {
std::string read_ref_main(const fs::path& model_cache_path) {
    std::ifstream refs_file(model_cache_path / "refs" / "main");
    if (!refs_file.is_open()) {
        return "";
    }
    std::string ref;
    std::getline(refs_file, ref);
    ref.erase(0, ref.find_first_not_of(" \t\r\n"));
    size_t last = ref.find_last_not_of(" \t\r\n");
    if (last == std::string::npos) {
        return "";
    }
    ref.erase(last + 1);
    return ref;
}
} // namespace

fs::path active_snapshot_path(const fs::path& model_cache_path) {
    std::string ref = read_ref_main(model_cache_path);
    if (ref.empty()) {
        return fs::path();
    }
    fs::path snapshot_path = model_cache_path / "snapshots" / ref;
    return lemon::backends::hf_cache::exists(snapshot_path) ? snapshot_path : fs::path();
}

std::string repo_id_to_cache_dir_name(const std::string& repo_id,
                                      const std::string& registry_source) {
    return registry_repo_cache_dir_name(repo_id,
        parse_remote_registry_source(registry_source));
}

} // namespace hf_cache
} // namespace backends
} // namespace lemon
