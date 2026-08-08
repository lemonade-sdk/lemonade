#pragma once

#include <cstdint>
#include <filesystem>
#include <regex>
#include <string>

namespace lemon {

// llama.cpp shards follow the <base>-NNNNN-of-NNNNN.gguf convention. A
// resolved checkpoint may point at a single shard (e.g. the first file of a
// folder-sharded quant layout), so a model-level size computation must reach
// the marshalling sum of all sibling shards rather than just that one part.
//
// Returns true when `filename` matches the shard-naming convention and, when
// `base` is non-null, writes the shard-family base (everything before the
// "-NNNNN-of-NNNNN" suffix). The "-of-NNNNN" tail is required so that bare
// numbered files such as "model-7.gguf" are not mistaken for shards.
inline bool is_gguf_shard_filename(const std::string& filename, std::string* base) {
    static const std::regex shard_pattern(R"(-(\d+)-of-(\d+)\.gguf$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(filename, match, shard_pattern)) {
        return false;
    }
    if (base) {
        *base = filename.substr(0, match.position(0));
    }
    return true;
}

// Sums the byte sizes of every sibling shard that shares `shard_path`'s family
// by walking its parent directory and matching strictly on the shard-naming
// convention plus an identical base. Returns 0 when `shard_path` is not a
// shard or the scan finds nothing. std::filesystem::file_size() follows
// symlinks, so HuggingFace cache blobs (content-addressed via symlinks under
// the snapshots tree) are measured at their real target size.
inline std::uintmax_t sharded_gguf_size_bytes(const std::filesystem::path& shard_path) {
    std::string base;
    if (!is_gguf_shard_filename(shard_path.filename().string(), &base)) {
        return 0;
    }

    const std::filesystem::path dir = shard_path.parent_path();
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return 0;
    }

    std::uintmax_t total = 0;
    for (const auto& entry : std::filesystem::directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!entry.is_regular_file(ec)) {
            if (ec) ec.clear();
            continue;
        }

        std::string sibling_base;
        if (!is_gguf_shard_filename(entry.path().filename().string(), &sibling_base)) {
            continue;
        }
        if (sibling_base != base) {
            continue;
        }

        auto size = std::filesystem::file_size(entry.path(), ec);
        if (!ec) {
            total += size;
        } else {
            ec.clear();
        }
    }
    return total;
}

} // namespace lemon
