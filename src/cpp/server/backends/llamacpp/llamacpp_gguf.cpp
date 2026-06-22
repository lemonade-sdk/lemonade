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
    if (bytes > static_cast<uint64_t>((std::numeric_limits<std::streamoff>::max)())) return false;
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
            if (v > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) return false;
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
        if (count > (std::numeric_limits<uint64_t>::max)() / elem_size) return false;
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

    // Collect the (sorted, mmproj-excluded) GGUF files under a search root.
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
        // Sort for consistent ordering (important for sharded models) and so the
        // active/whole-cache sets compare equal when they hold the same files.
        std::sort(files.begin(), files.end());
        return files;
    };

    const std::string variant_lower = to_lower(variant);

    // Resolve the requested GGUF variant within a candidate list of files.
    // Returns the matched absolute path, or "" if this candidate set does not
    // contain the variant. Factored into a lambda so the search can be retried
    // against a broader set of snapshots (see #2300 below) without duplicating
    // the matching logic.
    auto resolve_gguf_variant = [&](const std::vector<std::string>& gguf_files) -> std::string {
        if (gguf_files.empty()) {
            return "";
        }

        // Case 0: Wildcard (*) - return first file (llama-server auto-loads shards)
        if (variant == "*") {
            return gguf_files[0];
        }

        // Case 1: Empty variant - return first file
        if (variant.empty()) {
            return gguf_files[0];
        }

        // Case 2: Exact filename match (variant ends with .gguf)
        if (variant.find(".gguf") != std::string::npos) {
            for (const auto& filepath : gguf_files) {
                if (path_from_utf8(filepath).filename().string() == variant) {
                    return filepath;
                }
            }
            return "";  // Exact variant not found in this candidate set
        }

        // Case 3: Files ending with {variant}.gguf (case insensitive)
        const std::string suffix = variant_lower + ".gguf";
        for (const auto& filepath : gguf_files) {
            std::string filename_lower = to_lower(path_from_utf8(filepath).filename().string());
            if (filename_lower.size() >= suffix.size() &&
                filename_lower.substr(filename_lower.size() - suffix.size()) == suffix) {
                return filepath;
            }
        }

        // Case 4: Folder-based sharding (files in variant/ folder)
        const std::string folder_prefix_lower = variant_lower + "/";
        for (const auto& filepath : gguf_files) {
            std::string relative_lower = to_lower(path_to_utf8(
                path_from_utf8(filepath).lexically_relative(model_cache_path_fs)));
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

        for (const auto& filepath : gguf_files) {
            std::string relative_path = path_to_utf8(
                path_from_utf8(filepath).lexically_relative(model_cache_path_fs));
            relative_path = repo_relative_from_cache_relative(relative_path);

            // Multiple HF snapshots can contain the same repo-relative file.
            // Keep the first absolute path from the sorted file list so
            // duplicates do not create false ambiguity.
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

        // No match in this candidate set. Do not fall back to another
        // quantization in the same Hugging Face repo; otherwise a custom
        // download with a different quant can make a built-in model appear
        // downloaded and allow deleting the wrong file.
        return "";
    };

    // Prefer the active refs/main snapshot so that when upstream only changed
    // README/metadata Lemonade keeps using the previous snapshot's artifacts.
    std::vector<std::string> active_gguf_files =
        collect_gguf_files(hf_cache::active_snapshot_path(model_cache_path_fs));

    // Whole-repo-cache candidates spanning every snapshot, populated on demand.
    std::vector<std::string> all_cache_gguf_files;
    bool all_cache_computed = false;
    auto whole_cache_gguf_files = [&]() -> const std::vector<std::string>& {
        if (!all_cache_computed) {
            all_cache_gguf_files = collect_gguf_files(model_cache_path_fs);
            all_cache_computed = true;
        }
        return all_cache_gguf_files;
    };

    if (active_gguf_files.empty() && whole_cache_gguf_files().empty()) {
        return model_cache_path;  // Return directory if no GGUF found anywhere
    }

    std::string resolved_path = resolve_gguf_variant(active_gguf_files);

    // #2300: a sibling variant that shares this HF repo can live in a snapshot
    // other than the one refs/main points at. refs/main advances to the
    // snapshot of whichever variant was pulled or updated last, leaving the
    // other variants' symlinks behind in earlier snapshots; after a restart the
    // refs/main-only search above then reports them as missing. If the active
    // snapshot did not contain the requested variant, broaden the search to
    // every snapshot in this repo's cache before declaring it missing. Blobs are
    // content-addressed and shared, so reading an older snapshot is safe, and
    // resolving against the active snapshot first preserves the CHECKPOINT:VARIANT
    // contract (a different quant is never substituted while the exact one exists).
    //
    // The whole-cache set is a superset of the active set, so the two are equal
    // only when refs/main's snapshot is the sole snapshot holding GGUFs — in
    // which case the broader search is identical and skipped.
    if (resolved_path.empty()) {
        const std::vector<std::string>& all_files = whole_cache_gguf_files();
        if (all_files != active_gguf_files) {
            resolved_path = resolve_gguf_variant(all_files);
        }
    }

    return resolved_path;
}

} // namespace llamacpp
} // namespace backends
} // namespace lemon
