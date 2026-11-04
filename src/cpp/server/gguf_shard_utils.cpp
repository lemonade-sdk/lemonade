#include <lemon/gguf_shard_utils.h>

#include <lemon/utils/safe_fs.h>

#include <filesystem>
#include <string>
#include <vector>

namespace lemon {

namespace fs = std::filesystem;
using lemon::utils::safe_dir_options;
using lemon::utils::safe_is_directory;

// The shards in `shard_path`'s family, or none when it is not a shard.
static std::vector<fs::path> shard_family(const fs::path& shard_path) {
    std::vector<fs::path> shards;
    std::string base;
    int total = 0;
    if (!is_gguf_shard_filename(shard_path.filename().string(), &base, &total)) {
        return shards;
    }

    const fs::path dir = shard_path.parent_path();
    if (!safe_is_directory(dir)) {
        return shards;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, safe_dir_options, ec)) {
        if (!entry.is_regular_file(ec)) {
            if (ec) ec.clear();
            continue;
        }
        if (same_shard_family(entry.path().filename().string(), base, total)) {
            shards.push_back(entry.path());
        }
    }
    return shards;
}

std::uintmax_t sharded_gguf_size_bytes(const fs::path& shard_path) {
    uintmax_t sum = 0;
    for (const auto& shard : shard_family(shard_path)) {
        std::error_code ec;
        auto size = fs::file_size(shard, ec);
        if (!ec) {
            sum += size;
        }
    }
    return sum;
}

std::vector<std::string> gguf_files(const std::string& gguf_path) {
    std::vector<std::string> files = {gguf_path};
    const fs::path path(gguf_path);
    for (const auto& shard : shard_family(path)) {
        if (shard != path) {
            files.push_back(shard.string());
        }
    }
    return files;
}

}  // namespace lemon
