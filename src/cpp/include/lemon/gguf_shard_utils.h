#pragma once

#include <cstdint>
#include <filesystem>
#include <regex>
#include <string>

namespace lemon {

// Separators mirror hf_variants.cpp and visible_extra_variant_name(), which
// both accept `-`, `.` and `_` before the shard index.
inline bool is_gguf_shard_filename(const std::string& filename, std::string* base,
                                   int* total = nullptr, int* index = nullptr) {
    static const std::regex shard_pattern(R"([-._](\d+)-of-(\d+)\.gguf$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(filename, match, shard_pattern)) {
        return false;
    }

    int parsed_index = 0;
    int parsed_total = 0;
    try {
        parsed_index = std::stoi(match[1].str());
        parsed_total = std::stoi(match[2].str());
    } catch (...) {
        return false;
    }
    if (parsed_index <= 0 || parsed_total <= 0 || parsed_index > parsed_total) {
        return false;
    }

    if (base) {
        *base = filename.substr(0, match.position(0));
    }
    if (total) {
        *total = parsed_total;
    }
    if (index) {
        *index = parsed_index;
    }
    return true;
}

inline bool same_shard_family(const std::string& filename, const std::string& base, int total) {
    std::string candidate_base;
    int candidate_total = 0;
    if (!is_gguf_shard_filename(filename, &candidate_base, &candidate_total)) {
        return false;
    }
    return candidate_base == base && candidate_total == total;
}

// Total on-disk size of every sibling shard sharing `shard_path`'s family.
// Returns 0 when `shard_path` is not a shard. Defined in model_manager.cpp,
// which owns the Windows-safe filesystem helpers.
std::uintmax_t sharded_gguf_size_bytes(const std::filesystem::path& shard_path);

} // namespace lemon
