#include "lemon/backends/llamacpp/llamacpp_gguf.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <istream>
#include <limits>
#include <map>
#include <vector>
#include "lemon/backends/hf_cache_util.h"
#include "lemon/hf_variants.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/path_utils.h"

namespace fs = std::filesystem;

namespace lemon {
namespace backends {
namespace llamacpp {
namespace {

using lemon::utils::path_from_utf8;
using lemon::utils::path_to_utf8;

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Local copies of the tiny case-insensitive string helpers (kept out of a shared
// util to keep this GGUF reader self-contained).
bool ends_with_ignore_case(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin(),
                      [](char a, char b) { return std::tolower(a) == std::tolower(b); });
}

bool contains_ignore_case(const std::string& str, const std::string& substr) {
    auto it = std::search(str.begin(), str.end(), substr.begin(), substr.end(),
                          [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    return it != str.end();
}

template <typename T>
static bool read_le(std::istream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

static bool read_gguf_string(std::istream& in, std::string& value) {
    uint64_t len = 0;
    if (!read_le(in, len)) return false;
    if (len > 1024 * 1024) return false;
    value.assign(static_cast<size_t>(len), '\0');
    if (len == 0) return true;
    in.read(&value[0], static_cast<std::streamsize>(len));
    return static_cast<bool>(in);
}

static bool skip_bytes(std::istream& in, uint64_t bytes) {
    if (bytes > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) return false;
    in.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
    return static_cast<bool>(in);
}

static uint64_t gguf_scalar_size(uint32_t type) {
    switch (type) {
        case 0:  // UINT8
        case 1:  // INT8
        case 7:  // BOOL
            return 1;
        case 2:  // UINT16
        case 3:  // INT16
            return 2;
        case 4:  // UINT32
        case 5:  // INT32
        case 6:  // FLOAT32
            return 4;
        case 10: // UINT64
        case 11: // INT64
        case 12: // FLOAT64
            return 8;
        default:
            return 0;
    }
}

static bool skip_gguf_value(std::istream& in, uint32_t type);

static bool read_gguf_integer_value(std::istream& in, uint32_t type, int64_t& value) {
    switch (type) {
        case 0: { uint8_t v = 0; if (!read_le(in, v)) return false; value = v; return true; }
        case 1: { int8_t v = 0; if (!read_le(in, v)) return false; value = v; return true; }
        case 2: { uint16_t v = 0; if (!read_le(in, v)) return false; value = v; return true; }
        case 3: { int16_t v = 0; if (!read_le(in, v)) return false; value = v; return true; }
        case 4: { uint32_t v = 0; if (!read_le(in, v)) return false; value = v; return true; }
        case 5: { int32_t v = 0; if (!read_le(in, v)) return false; value = v; return true; }
        case 10: {
            uint64_t v = 0;
            if (!read_le(in, v)) return false;
            if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
            value = static_cast<int64_t>(v);
            return true;
        }
        case 11: { int64_t v = 0; if (!read_le(in, v)) return false; value = v; return true; }
        default:
            return skip_gguf_value(in, type) && false;
    }
}

static bool skip_gguf_value(std::istream& in, uint32_t type) {
    if (type == 8) {  // STRING
        std::string ignored;
        return read_gguf_string(in, ignored);
    }

    if (type == 9) {  // ARRAY
        uint32_t elem_type = 0;
        uint64_t count = 0;
        if (!read_le(in, elem_type) || !read_le(in, count)) return false;

        if (elem_type == 8) {
            for (uint64_t i = 0; i < count; ++i) {
                std::string ignored;
                if (!read_gguf_string(in, ignored)) return false;
            }
            return true;
        }

        if (elem_type == 9) return false;
        uint64_t elem_size = gguf_scalar_size(elem_type);
        if (elem_size == 0) return false;
        if (count > std::numeric_limits<uint64_t>::max() / elem_size) return false;
        return skip_bytes(in, count * elem_size);
    }

    uint64_t size = gguf_scalar_size(type);
    return size > 0 && skip_bytes(in, size);
}

} // namespace

bool read_gguf_metadata(GgufMetadata& out, const std::string& path) {
    std::ifstream in(path_from_utf8(path), std::ios::binary);
    if (!in) return false;

    char magic[4] = {};
    in.read(magic, sizeof(magic));
    if (!in || std::memcmp(magic, "GGUF", 4) != 0) return false;

    uint32_t version = 0;
    uint64_t tensor_count = 0;
    uint64_t kv_count = 0;
    if (!read_le(in, version) || !read_le(in, tensor_count) || !read_le(in, kv_count)) return false;
    (void)version;
    (void)tensor_count;

    int64_t pending_context_length = 0;

    for (uint64_t i = 0; i < kv_count; ++i) {
        std::string key;
        uint32_t type = 0;
        if (!read_gguf_string(in, key) || !read_le(in, type)) return false;

        // Read architecture
        if (key == "general.architecture" && type == 8) {
            if (!read_gguf_string(in, out.architecture)) return false;
            if (pending_context_length > 0) {
                out.context_length = pending_context_length;
            }
            continue;
        }

        // Context length
        const bool context_key = !out.architecture.empty() && key == out.architecture + ".context_length";
        const bool possible_context_key = out.architecture.empty() && key.size() > std::strlen(".context_length") &&
                                          ends_with_ignore_case(key, ".context_length");
        if (context_key || possible_context_key) {
            int64_t value = 0;
            if (read_gguf_integer_value(in, type, value) && value > 0) {
                if (context_key) {
                    out.context_length = value;
                } else {
                    pending_context_length = value;
                }
            }
            continue;
        }

        // Architecture fields for KV cache estimation
        if (!out.architecture.empty()) {
            if (key == out.architecture + ".block_count") {
                int64_t value = 0;
                if (read_gguf_integer_value(in, type, value) && value > 0)
                    out.block_count = value;
                continue;
            }
            if (key == out.architecture + ".embedding_length") {
                int64_t value = 0;
                if (read_gguf_integer_value(in, type, value) && value > 0)
                    out.embedding_length = value;
                continue;
            }
            if (key == out.architecture + ".attention.head_count_kv") {
                int64_t value = 0;
                if (read_gguf_integer_value(in, type, value) && value > 0)
                    out.head_count_kv = value;
                continue;
            }
            if (key == out.architecture + ".attention.key_length") {
                int64_t value = 0;
                if (read_gguf_integer_value(in, type, value) && value > 0)
                    out.key_length = value;
                continue;
            }
        }

        // Capability detection (vision, tool-calling, MTP)
        if (type == 4) {
            uint32_t val = 0;
            if (read_le(in, val)) {
                if (contains_ignore_case(key, "nextn_predict_layers") && val > 0)
                    out.caps.mtp = true;
            }
        } else if (type == 8) {
            std::string value;
            if (read_gguf_string(in, value)) {
                inspect_gguf_string(key, value, out.caps);
            }
        } else if (type == 9) {
            // Array — check string elements for capability hints
            uint32_t elem_type = 0;
            uint64_t count = 0;
            if (read_le(in, elem_type) && read_le(in, count)) {
                if (elem_type == 8) {
                    for (uint64_t j = 0; j < count; ++j) {
                        std::string value;
                        if (!read_gguf_string(in, value)) return false;
                        inspect_gguf_string(key, value, out.caps);
                    }
                } else if (elem_type != 9) {
                    uint64_t elem_size = gguf_scalar_size(elem_type);
                    if (elem_size == 0) return false;
                    if (!skip_bytes(in, count * elem_size)) return false;
                } else {
                    return false;
                }
            } else {
                return false;
            }
        } else {
            if (!skip_gguf_value(in, type)) return false;
        }
    }

    if (out.context_length == 0 && pending_context_length > 0) {
        out.context_length = pending_context_length;
    }
    return true;
}


std::string resolve_gguf_path(const std::string& model_cache_path, const std::string& variant) {
    fs::path model_cache_path_fs = path_from_utf8(model_cache_path);
        if (!hf_cache::exists(model_cache_path_fs)) {
            return model_cache_path;  // Return directory path even if not found
        }

        // Prefer the active HF snapshot recorded in refs/main. This lets
        // Lemonade keep using the previous snapshot when upstream only changed
        // README/metadata and the requested model artifacts are unchanged.
        auto collect_gguf_files = [](const fs::path& search_root) {
            std::vector<std::string> files;
            if (search_root.empty() || !hf_cache::exists(search_root)) {
                return files;
            }

            std::error_code ec;
            for (const auto& entry : fs::recursive_directory_iterator(search_root, hf_cache::dir_options(), ec)) {
                if (ec) break;
                if (!entry.is_regular_file(ec)) {
                    ec.clear();
                    continue;
                }

                std::string filename = entry.path().filename().string();
                std::string filename_lower = filename;
                std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(), ::tolower);

                if (filename.find(".gguf") != std::string::npos && filename_lower.find("mmproj") == std::string::npos) {
                    files.push_back(path_to_utf8(entry.path()));
                }
            }
            return files;
        };

        std::vector<std::string> all_gguf_files = collect_gguf_files(hf_cache::active_snapshot_path(model_cache_path_fs));
        if (all_gguf_files.empty()) {
            // Backward-compatible fallback for caches without refs/main and for
            // partially migrated/manual HF cache layouts.
            all_gguf_files = collect_gguf_files(model_cache_path_fs);
        }

        if (all_gguf_files.empty()) {
            return model_cache_path;  // Return directory if no GGUF found
        }

        // Sort files for consistent ordering (important for sharded models)
        std::sort(all_gguf_files.begin(), all_gguf_files.end());

        // Case 0: Wildcard (*) - return first file (llama-server will auto-load shards)
        if (variant == "*") {
            return all_gguf_files[0];
        }

        // Case 1: Empty variant - return first file
        if (variant.empty()) {
            return all_gguf_files[0];
        }

        // Case 2: Exact filename match (variant ends with .gguf)
        if (variant.find(".gguf") != std::string::npos) {
            for (const auto& filepath : all_gguf_files) {
                std::string filename = path_from_utf8(filepath).filename().string();
                if (filename == variant) {
                    return filepath;
                }
            }
            return "";  // Exact variant not found — signal not downloaded
        }

        // Case 3: Files ending with {variant}.gguf (case insensitive)
        std::string variant_lower = variant;
        std::transform(variant_lower.begin(), variant_lower.end(), variant_lower.begin(), ::tolower);
        std::string suffix = variant_lower + ".gguf";

        std::vector<std::string> matching_files;
        for (const auto& filepath : all_gguf_files) {
            std::string filename = path_from_utf8(filepath).filename().string();
            std::string filename_lower = filename;
            std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(), ::tolower);

            if (filename_lower.size() >= suffix.size() &&
                filename_lower.substr(filename_lower.size() - suffix.size()) == suffix) {
                matching_files.push_back(filepath);
            }
        }

        if (!matching_files.empty()) {
            return matching_files[0];
        }

        // Case 4: Folder-based sharding (files in variant/ folder)
        std::string folder_prefix_lower = variant_lower + "/";

        for (const auto& filepath : all_gguf_files) {
            // Get relative path from model cache path
            std::string relative_path = path_to_utf8(
                path_from_utf8(filepath).lexically_relative(model_cache_path_fs));
            std::string relative_lower = relative_path;
            // Normalize path separators and case so folder-variant matching works cross-platform.
            std::transform(relative_lower.begin(), relative_lower.end(), relative_lower.begin(), ::tolower);
            std::replace(relative_lower.begin(), relative_lower.end(), '\\', '/');

            if (relative_lower.find(folder_prefix_lower) != std::string::npos) {
                return filepath;
            }
        }

        // Case 5: Local quant-token fallback.
        //
        // Keep the existing resolver cases above as the primary logic: exact
        // filenames, suffix matches, and folder-based sharding are more
        // specific and preserve the CHECKPOINT:VARIANT contract.
        //
        // Some GGUF repositories name files with the quant token in the middle,
        // for example:
        //   Qwen3.6-27B-MTP-IMAT-IQ4_XS-Q8nextn.gguf
        // for variant:
        //   IQ4_XS
        // That file does not end with IQ4_XS.gguf, so mirror the downloader's
        // GGUF variant enumeration over the files that are already present in
        // the local HF cache before declaring the model missing.
        //
        // HF cache paths have an extra snapshots/<revision>/ prefix that is not
        // part of the repository-relative filename. Strip it before calling
        // enumerate_gguf_variants(); otherwise the enumerator treats
        // "snapshots" as a top-level sharded-folder variant and never extracts
        // the quant token from the actual GGUF filename.
        std::vector<std::string> relative_gguf_files;
        std::map<std::string, std::string> absolute_by_relative;
        auto repo_relative_from_cache_relative = [](std::string rel) {
            std::replace(rel.begin(), rel.end(), '\\', '/');

            static const std::string snapshots_prefix = "snapshots/";
            if (rel.rfind(snapshots_prefix, 0) == 0) {
                size_t revision_end = rel.find('/', snapshots_prefix.size());
                if (revision_end != std::string::npos && revision_end + 1 < rel.size()) {
                    rel = rel.substr(revision_end + 1);
                }
            }

            return rel;
        };

        for (const auto& filepath : all_gguf_files) {
            std::string relative_path = path_to_utf8(
                path_from_utf8(filepath).lexically_relative(model_cache_path_fs));
            relative_path = repo_relative_from_cache_relative(relative_path);

            // Multiple HF snapshots can contain the same repo-relative file.
            // Keep the first absolute path from the sorted all_gguf_files list
            // so duplicates do not create false ambiguity.
            if (absolute_by_relative.emplace(relative_path, filepath).second) {
                relative_gguf_files.push_back(relative_path);
            }
        }

        std::vector<std::string> enumerated_matches;
        auto local_variants = lemon::enumerate_gguf_variants(relative_gguf_files);
        for (const auto& local_variant : local_variants.variants) {
            if (to_lower(local_variant.name) != variant_lower) {
                continue;
            }

            auto it = absolute_by_relative.find(local_variant.primary_file);
            if (it != absolute_by_relative.end()) {
                enumerated_matches.push_back(it->second);
            }
        }

        if (enumerated_matches.size() == 1) {
            LOG(INFO, "ModelManager")
                << "Resolved local GGUF variant '" << variant
                << "' via quant-token fallback: " << enumerated_matches[0] << std::endl;
            return enumerated_matches[0];
        }

        if (enumerated_matches.size() > 1) {
            LOG(WARNING, "ModelManager")
                << "Multiple local GGUF files matched variant '" << variant
                << "' via quant-token fallback; refusing to guess" << std::endl;
            return "";
        }

        // No match found for the requested GGUF variant. Do not fall back to
        // another quantization in the same Hugging Face repo; otherwise a
        // custom download with a different quant can make a built-in model
        // appear downloaded and allow deleting the wrong file.
        return "";
}

} // namespace llamacpp
} // namespace backends
} // namespace lemon
