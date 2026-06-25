#include "lemon/backends/llamacpp/llamacpp_gguf.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
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

} // namespace

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

