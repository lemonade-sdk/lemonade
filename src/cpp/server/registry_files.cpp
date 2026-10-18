#include <lemon/registry_files.h>

#include <lemon/backends/backend_registry.h>
#include <lemon/backends/hf_cache_util.h>
#include <lemon/gguf_reader.h>
#include <lemon/gguf_shard_utils.h>
#include <lemon/hf_variants.h>
#include <lemon/models/checkpoint_grammar.h>
#include <lemon/utils/aixlog.hpp>
#include <lemon/utils/path_utils.h>
#include <lemon/utils/http_client.h>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/safe_fs.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace fs = std::filesystem;

namespace lemon {
namespace registry_files {

using namespace lemon::utils;

namespace {

// Structure to hold identified GGUF files
struct GGUFFiles {
    std::map<std::string, std::string> core_files;  // {"variant": "file.gguf", "mmproj": "file.mmproj"}
    std::vector<std::string> sharded_files;         // Additional shard files
};

// Identifies GGUF model files matching the variant (Python equivalent of identify_gguf_models)
GGUFFiles identify_gguf_models(
    const std::string& checkpoint,
    const std::string& variant,
    const std::vector<std::string>& repo_files
) {
    const std::string hint = R"(
    The CHECKPOINT:VARIANT scheme is used to specify model files in remote registry repositories.

    The VARIANT format can be one of several types:
    0. wildcard (*): download all .gguf files in the repo
    1. Full filename: exact file to download
    2. None/empty: gets the first .gguf file in the repository (excludes mmproj files)
    3. Quantization variant: find a single file ending with the variant name (case insensitive)
    4. Folder name: downloads all .gguf files in the folder that matches the variant name (case insensitive)

    Examples:
    - "ggml-org/gpt-oss-120b-GGUF:*" -> downloads all .gguf files in repo
    - "unsloth/Qwen3-8B-GGUF:qwen3.gguf" -> downloads "qwen3.gguf"
    - "unsloth/Qwen3-30B-A3B-GGUF" -> downloads "Qwen3-30B-A3B-GGUF.gguf"
    - "unsloth/Qwen3-8B-GGUF:Q4_1" -> downloads "Qwen3-8B-GGUF-Q4_1.gguf"
    - "unsloth/Qwen3-30B-A3B-GGUF:Q4_0" -> downloads all files in "Q4_0/" folder
    )";

    GGUFFiles result;
    std::vector<std::string> sharded_files;
    std::string variant_name;
    auto join_files = [](const std::vector<std::string>& files) {
        std::string out;
        for (size_t i = 0; i < files.size(); ++i) {
            if (i > 0) out += ", ";
            out += files[i];
        }
        return out;
    };

    // (case 0) Wildcard, download everything
    if (!variant.empty() && variant == "*") {
        for (const auto& f : repo_files) {
            if (gguf_reader_detail::ends_with_ignore_case(f, ".gguf")) {
                sharded_files.push_back(f);
            }
        }

        if (sharded_files.empty()) {
            throw std::runtime_error("No .gguf files found in repository " + checkpoint + ". " + hint);
        }

        // Sort to ensure consistent ordering
        std::sort(sharded_files.begin(), sharded_files.end());

        // Use first file as primary (this is how llamacpp handles it)
        variant_name = sharded_files[0];
    }
    // (case 1) If variant ends in .gguf or .bin, use it directly
    else if (!variant.empty() && (gguf_reader_detail::ends_with_ignore_case(variant, ".gguf") || gguf_reader_detail::ends_with_ignore_case(variant, ".bin"))) {
        variant_name = variant;

        // Validate file exists in repo
        bool found = false;
        for (const auto& f : repo_files) {
            if (f == variant) {
                found = true;
                break;
            }
        }

        if (!found) {
            throw std::runtime_error(
                "File " + variant + " not found in remote model repository " + checkpoint + ". " + hint
            );
        }
    }
    // (case 2) If no variant is provided, get the first .gguf file in the repository
    else if (variant.empty()) {
        std::vector<std::string> all_variants;
        for (const auto& f : repo_files) {
            if (gguf_reader_detail::ends_with_ignore_case(f, ".gguf") && !gguf_reader_detail::contains_ignore_case(f, "mmproj")) {
                all_variants.push_back(f);
            }
        }

        if (all_variants.empty()) {
            throw std::runtime_error(
                "No .gguf files found in remote model repository " + checkpoint + ". " + hint
            );
        }

        variant_name = all_variants[0];
    }
    else {
        auto vset = lemon::enumerate_gguf_variants(repo_files);
        std::vector<lemon::GgufVariant> exact_matches;
        for (const auto& v : vset.variants) {
            if (gguf_reader_detail::to_lower(v.name) == gguf_reader_detail::to_lower(variant)) {
                exact_matches.push_back(v);
            }
        }

        if (exact_matches.size() == 1) {
            variant_name = exact_matches[0].primary_file;
            sharded_files = exact_matches[0].files;
        } else if (exact_matches.size() > 1) {
            std::vector<std::string> matching_files;
            for (const auto& match : exact_matches) {
                matching_files.insert(matching_files.end(), match.files.begin(), match.files.end());
            }
            std::sort(matching_files.begin(), matching_files.end());
            throw std::runtime_error(
                "Multiple GGUF variant groups matched '" + variant + "': " +
                join_files(matching_files) + ". " + hint
            );
        } else {
            // Fallback for repos that require direct filename matching rather than
            // the canonical variant set used by /pull/variants.
            std::vector<std::string> end_with_variant;
            std::string variant_suffix = variant + ".gguf";

            for (const auto& f : repo_files) {
                if (gguf_reader_detail::ends_with_ignore_case(f, variant_suffix) && !gguf_reader_detail::contains_ignore_case(f, "mmproj")) {
                    end_with_variant.push_back(f);
                }
            }

            if (end_with_variant.size() == 1) {
                variant_name = end_with_variant[0];
            }
            else if (end_with_variant.size() > 1) {
                std::sort(end_with_variant.begin(), end_with_variant.end());
                throw std::runtime_error(
                    "Multiple .gguf files matched variant '" + variant + "': " +
                    join_files(end_with_variant) + ". " + hint
                );
            }
            // (case 4) Check whether the variant corresponds to a folder with sharded files (case insensitive)
            else {
                std::string folder_prefix = variant + "/";
                for (const auto& f : repo_files) {
                    if (gguf_reader_detail::ends_with_ignore_case(f, ".gguf") && gguf_reader_detail::starts_with_ignore_case(f, folder_prefix)) {
                        sharded_files.push_back(f);
                    }
                }

                // If no exact folder match, try folders ending with -{variant}/ or _{variant}/
                // This handles repos where the folder is prefixed with the model name,
                // e.g. "Qwen3-Coder-Next-Q4_K_M/" instead of just "Q4_K_M/"
                if (sharded_files.empty()) {
                    std::string suffix_dash = "-" + variant + "/";
                    std::string suffix_underscore = "_" + variant + "/";
                    for (const auto& f : repo_files) {
                        if (!gguf_reader_detail::ends_with_ignore_case(f, ".gguf")) continue;
                        size_t slash_pos = f.find('/');
                        if (slash_pos != std::string::npos) {
                            std::string folder = f.substr(0, slash_pos + 1);
                            if (gguf_reader_detail::ends_with_ignore_case(folder, suffix_dash) ||
                                gguf_reader_detail::ends_with_ignore_case(folder, suffix_underscore)) {
                                sharded_files.push_back(f);
                            }
                        }
                    }
                }

                if (sharded_files.empty()) {
                    throw std::runtime_error(
                        "No .gguf files found for variant " + variant + ". " + hint
                    );
                }

                // Sort to ensure consistent ordering
                std::sort(sharded_files.begin(), sharded_files.end());

                // Use first file as primary (this is how llamacpp handles it)
                variant_name = sharded_files[0];
            }
        }
    }

    result.core_files["variant"] = variant_name;
    result.sharded_files = sharded_files;

    return result;
}

}  // namespace

std::string snapshot_id_from_resolved_path(
    const std::string& resolved_path,
    const fs::path& model_cache_path) {
    if (resolved_path.empty()) {
        return "";
    }

    const fs::path snapshots_path = model_cache_path / "snapshots";
    const fs::path relative =
        path_from_utf8(resolved_path).lexically_relative(snapshots_path);
    if (relative.empty()) {
        return "";
    }

    auto first = relative.begin();
    if (first == relative.end() || *first == "." || *first == "..") {
        return "";
    }
    return path_to_utf8(*first);
}

std::string active_local_snapshot(
    const std::string& resolved_path,
    const fs::path& model_cache_path) {
    std::string snapshot = snapshot_id_from_resolved_path(resolved_path, model_cache_path);
    if (snapshot.empty()) {
        snapshot = backends::hf_cache::read_ref_main(model_cache_path);
    }
    return snapshot;
}

std::map<std::string, std::vector<std::string>> group_aux_checkpoint_variants(
    const std::string& recipe, const std::map<std::string, std::string>& checkpoints) {
    std::map<std::string, std::vector<std::string>> grouped;
    for (const auto& [type, checkpoint] : checkpoints) {
        if (type == "main" || backends::ops_for(recipe)->acquires_lazily(type)) continue;
        const std::string repo_id = models::repo_id_from_checkpoint(checkpoint);
        const std::string variant = models::variant_from_checkpoint(checkpoint);
        if (repo_id.empty() || variant.empty()) {
            continue;
        }
        grouped[repo_id].push_back(variant);
    }
    return grouped;
}

std::vector<std::string> select_main_repo_files(
    const std::string& repo_id,
    const std::string& recipe,
    const std::string& variant,
    const std::vector<std::string>& repo_files) {
    auto backend_files =
        backends::ops_for(recipe)->select_checkpoint_files(variant, repo_files);

    std::vector<std::string> selected;
    if (!variant.empty()) {
        auto ends_with = [](const std::string& value, const std::string& suffix) {
            return value.size() >= suffix.size() &&
                   value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        const bool direct_file = ends_with(variant, ".safetensors") ||
                                 ends_with(variant, ".pth") ||
                                 ends_with(variant, ".ckpt");

        if (direct_file) {
            if (std::find(repo_files.begin(), repo_files.end(), variant) ==
                repo_files.end()) {
                throw std::runtime_error("Model file not found in repository " + repo_id +
                                         ": " + variant);
            }
            selected.push_back(variant);
        } else if (backend_files) {
            selected = std::move(*backend_files);
        } else {
            GGUFFiles gguf_files = identify_gguf_models(repo_id, variant, repo_files);
            std::unordered_set<std::string> added_files;
            for (const auto& [key, filename] : gguf_files.core_files) {
                (void)key;
                selected.push_back(filename);
                added_files.insert(filename);
            }
            for (const auto& filename : gguf_files.sharded_files) {
                if (!added_files.count(filename)) selected.push_back(filename);
            }
        }

        for (const std::string& config_file : {
                 "config.json", "tokenizer.json", "tokenizer_config.json", "tokenizer.model"}) {
            if (std::find(repo_files.begin(), repo_files.end(), config_file) !=
                    repo_files.end() &&
                std::find(selected.begin(), selected.end(), config_file) == selected.end()) {
                selected.push_back(config_file);
            }
        }
    } else if (backend_files) {
        selected = std::move(*backend_files);
    } else {
        selected = repo_files;
    }
    return selected;
}

std::vector<std::string> select_main_repo_files_union(
    const std::string& repo_id,
    const std::string& recipe,
    const std::string& variant,
    const std::vector<std::string>& repo_files_a,
    const std::vector<std::string>& repo_files_b) {
    std::vector<std::string> selected =
        select_main_repo_files(repo_id, recipe, variant, repo_files_a);
    std::unordered_set<std::string> seen(selected.begin(), selected.end());
    for (auto& filename : select_main_repo_files(repo_id, recipe, variant, repo_files_b)) {
        if (seen.insert(filename).second) {
            selected.push_back(std::move(filename));
        }
    }
    return selected;
}

std::vector<std::string> merge_reuse_comparison_files(
    const std::vector<std::string>& base_files,
    const std::string& repo_id,
    const std::string& recipe,
    const std::string& variant,
    const std::vector<std::string>& repo_files_a,
    const std::vector<std::string>& repo_files_b) {
    std::vector<std::string> merged = base_files;
    std::unordered_set<std::string> seen(merged.begin(), merged.end());
    for (auto& filename :
         select_main_repo_files_union(repo_id, recipe, variant, repo_files_a, repo_files_b)) {
        if (seen.insert(filename).second) {
            merged.push_back(std::move(filename));
        }
    }
    return merged;
}

std::string hf_file_metadata_key(const std::string& repo_id, const std::string& filename) {
    return repo_id + ':' + filename;
}

std::map<std::string, HfFileMetadata> select_metadata(
    const std::string& repo_id,
    const std::vector<std::string>& selected_files,
    const std::map<std::string, HfFileMetadata>& tree_entries) {
    std::map<std::string, HfFileMetadata> selected;
    for (const auto& filename : selected_files) {
        const auto it = tree_entries.find(filename);
        if (it != tree_entries.end()) {
            selected[hf_file_metadata_key(repo_id, filename)] = it->second;
        }
    }
    return selected;
}

bool can_reuse_previous_hf_snapshot(
    const std::string& repo_id,
    const std::vector<std::string>& selected_files,
    const fs::path& previous_snapshot,
    const std::map<std::string, HfFileMetadata>& current_metadata,
    const std::map<std::string, HfFileMetadata>& previous_metadata) {
    if (repo_id.empty() || selected_files.empty() || previous_snapshot.empty() || !safe_exists(previous_snapshot)) {
        return false;
    }

    for (const auto& filename : selected_files) {
        const std::string key = hf_file_metadata_key(repo_id, filename);
        auto current_it = current_metadata.find(key);
        auto previous_it = previous_metadata.find(key);
        if (current_it == current_metadata.end() || previous_it == previous_metadata.end() ||
            !current_it->second.has_content_id() || !previous_it->second.has_content_id() ||
            current_it->second.content_id != previous_it->second.content_id) {
            return false;
        }

        fs::path previous_file = previous_snapshot / path_from_utf8(filename);
        fs::path previous_partial = path_from_utf8(path_to_utf8(previous_file) + ".partial");
        std::error_code ec;
        if (!fs::is_regular_file(previous_file, ec) || safe_exists(previous_partial)) {
            return false;
        }

        if (current_it->second.size > 0) {
            const auto previous_size = fs::file_size(previous_file, ec);
            if (ec || previous_size != current_it->second.size) {
                return false;
            }
        }
    }

    return true;
}

static HfFileMetadata hf_file_metadata_from_tree_file(const nlohmann::json& file) {
    HfFileMetadata entry;
    if (file.contains("size") && file["size"].is_number_unsigned()) {
        entry.size = file["size"].get<size_t>();
    }

    if (file.contains("lfs") && file["lfs"].is_object()) {
        const auto& lfs = file["lfs"];
        if (lfs.contains("size") && lfs["size"].is_number()) {
            entry.size = lfs["size"].get<size_t>();
        }
        if (lfs.contains("oid") && lfs["oid"].is_string()) {
            entry.hash_algorithm = "sha256";
            entry.hash_value = lfs["oid"].get<std::string>();
            entry.content_id = "lfs:" + entry.hash_value;
        }
        return entry;
    }

    if (file.contains("oid") && file["oid"].is_string()) {
        entry.hash_algorithm = "git-sha1";
        entry.hash_value = file["oid"].get<std::string>();
        entry.content_id = "git:" + entry.hash_value;
    }

    return entry;
}

std::map<std::string, HfFileMetadata> fetch_file_metadata_for_ref(
    const std::string& repo_id,
    const std::string& ref,
    const std::vector<std::string>& selected_files,
    const std::map<std::string, std::string>& headers,
    HfTreePageCache* tree_cache) {
    if (repo_id.empty() || ref.empty() || selected_files.empty()) {
        return {};
    }

    std::set<std::string> subdirs_to_fetch;
    subdirs_to_fetch.insert("");

    for (const auto& filename : selected_files) {
        auto last_slash_pos = filename.rfind('/');
        if (last_slash_pos != std::string::npos) {
            subdirs_to_fetch.insert(filename.substr(0, last_slash_pos));
        }
    }

    std::string hf_endpoint = "https://huggingface.co";
    if (const char* configured = std::getenv("HF_ENDPOINT"); configured && configured[0]) {
        hf_endpoint = configured;
        while (!hf_endpoint.empty() && hf_endpoint.back() == '/') hf_endpoint.pop_back();
    }

    // Full page entries, unfiltered, so a cached page serves every model
    // selecting from it; selection happens once at the end.
    std::map<std::string, registry_files::HfFileMetadata> tree_entries;
    for (const auto& subdir : subdirs_to_fetch) {
        if (tree_cache) {
            const auto cached = tree_cache->find(repo_id + '\n' + ref + '\n' + subdir);
            if (cached != tree_cache->end()) {
                tree_entries.insert(cached->second.begin(), cached->second.end());
                continue;
            }
        }

        std::string tree_url = hf_endpoint + "/api/models/" + repo_id + "/tree/" + ref;
        if (!subdir.empty()) {
            tree_url += "/" + subdir;
        }

        auto tree_response = HttpClient::get(tree_url, headers);
        if (tree_response.status_code != 200) {
            LOG(DEBUG, "ModelManager") << "Could not fetch Hugging Face tree metadata for "
                                       << repo_id << " at " << ref << std::endl;
            continue;
        }

        auto tree_info = JsonUtils::parse(tree_response.body);
        if (!tree_info.is_array()) {
            continue;
        }

        std::map<std::string, registry_files::HfFileMetadata> page;
        for (const auto& file : tree_info) {
            if (!file.contains("path") || !file["path"].is_string()) {
                continue;
            }
            page[file["path"].get<std::string>()] = hf_file_metadata_from_tree_file(file);
        }
        if (tree_cache) {
            tree_cache->emplace(repo_id + '\n' + ref + '\n' + subdir, page);
        }
        tree_entries.insert(page.begin(), page.end());
    }

    return select_metadata(repo_id, selected_files, tree_entries);
}

bool selected_artifacts_unchanged(
    const std::string& repo_id,
    const std::string& active_ref,
    const std::string& current_ref,
    const fs::path& active_snapshot,
    const std::vector<std::string>& selected_files,
    const std::map<std::string, std::string>& headers,
    HfTreePageCache* tree_cache) {
    if (repo_id.empty() || active_ref.empty() || current_ref.empty() ||
        active_ref == current_ref || selected_files.empty()) {
        return false;
    }

    try {
        const auto current_metadata =
            fetch_file_metadata_for_ref(repo_id, current_ref, selected_files, headers,
                                           tree_cache);
        const auto previous_metadata =
            fetch_file_metadata_for_ref(repo_id, active_ref, selected_files, headers,
                                           tree_cache);
        return can_reuse_previous_hf_snapshot(
            repo_id, selected_files, active_snapshot, current_metadata, previous_metadata);
    } catch (...) {
        return false;
    }
}

}  // namespace registry_files
}  // namespace lemon
