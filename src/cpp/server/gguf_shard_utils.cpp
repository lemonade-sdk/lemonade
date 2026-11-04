#include <lemon/gguf_shard_utils.h>

#include <lemon/utils/safe_fs.h>

#include <filesystem>
#include <string>

namespace lemon {

namespace fs = std::filesystem;
using lemon::utils::safe_dir_options;
using lemon::utils::safe_is_directory;

std::uintmax_t sharded_gguf_size_bytes(const fs::path& shard_path) {
    std::string base;
    int total = 0;
    if (!is_gguf_shard_filename(shard_path.filename().string(), &base, &total)) {
        return 0;
    }

    const fs::path dir = shard_path.parent_path();
    if (!safe_is_directory(dir)) {
        return 0;
    }

    uintmax_t sum = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, safe_dir_options, ec)) {
        if (!entry.is_regular_file(ec)) {
            if (ec) ec.clear();
            continue;
        }
        if (!same_shard_family(entry.path().filename().string(), base, total)) {
            continue;
        }

        auto size = fs::file_size(entry.path(), ec);
        if (!ec) {
            sum += size;
        } else {
            ec.clear();
        }
    }
    return sum;
}

}  // namespace lemon
